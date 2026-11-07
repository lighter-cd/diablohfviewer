// src/tools/mpq_tools.cpp
//
// MPQ tools — the C/C++ port of the former TypeScript orchestration layer
// (tools/mpq-inspect.ts and tools/mpq-recover.ts). Each tool is a registered
// handler returning JSON; both the stdio transport and the WebSocket bridge
// dispatch through the same ToolRegistry.
//
//   mpq.list     {archive}                  -> {files:[{name,size}]}
//   mpq.extract  {archive, name, out?, as_base64?}
//                                         -> {saved_path,size} | {base64,size}
//   mpq.verify   {archive, name}            -> {exists:bool}
//   mpq.recover  {archive, dict_path}       -> {checked,found,recovered:[...]}
//
// `archive` is a filesystem path to the .mpq. Backslashes in MPQ names are the
// native separators (StormLib accepts either '/' or '\').

#include "tools/registry.h"
#include "tools/base64.h"
#include "engine/archive.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

// Pull a required string field from params; throw if missing / wrong type.
std::string reqStr(const mcp::json& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end() || !it->is_string())
        throw std::invalid_argument(std::string("missing string param: ") + key);
    return it->get<std::string>();
}

// Open an archive, throwing a descriptive error on failure.
engine::Archive openOrThrow(const std::string& path) {
    engine::Archive a;
    if (!a.open(path))
        throw std::runtime_error("cannot open archive: " + path);
    return a;
}

mcp::ToolResult toolList(const mcp::json& params) {
    auto a = openOrThrow(reqStr(params, "archive"));
    auto entries = a.list();
    mcp::json files = mcp::json::array();
    for (const auto& e : entries)
        files.push_back(mcp::json{{"name", e.name}, {"size", e.size}});
    mcp::ToolResult r;
    r.value = mcp::json{{"archive", reqStr(params, "archive")},
                        {"count",   files.size()},
                        {"files",   files}};
    return r;
}

mcp::ToolResult toolExtract(const mcp::json& params) {
    std::string archive = reqStr(params, "archive");
    std::string name    = reqStr(params, "name");
    auto a = openOrThrow(archive);

    bool asBase64 = params.value("as_base64", false);
    std::string outPath = params.value("out", std::string());

    if (!asBase64 && outPath.empty()) {
        // Default: write to ./out/<sanitized name> so we never bloat JSON.
        std::string sane = name;
        for (char& c : sane)
            if (c == '\\' || c == '/') c = '_';
        outPath = "out/" + sane;
    }

    if (asBase64) {
        std::vector<uint8_t> buf;
        if (!a.extract(name, buf))
            throw std::runtime_error("extract failed (name not found?): " + name);
        mcp::ToolResult r;
        r.value = mcp::json{{"name",    name},
                            {"size",    buf.size()},
                            {"base64",  mcp::base64::encode(buf)}};
        return r;
    }

    // Write to file.
    if (!a.extractToFile(name, outPath))
        throw std::runtime_error("extract failed (name not found?): " + name);
    uint64_t sz = 0;
    a.querySize(name, sz);
    mcp::ToolResult r;
    r.value = mcp::json{{"name",       name},
                        {"size",       sz},
                        {"saved_path", outPath}};
    return r;
}

mcp::ToolResult toolVerify(const mcp::json& params) {
    auto a = openOrThrow(reqStr(params, "archive"));
    bool exists = a.verify(reqStr(params, "name"));
    mcp::ToolResult r;
    r.value = mcp::json{{"name",    reqStr(params, "name")},
                        {"exists",  exists}};
    return r;
}

// Port of tools/mpq-recover.ts: read a dictionary file (one candidate MPQ name
// per line), and for each name that resolves in the archive, record it with its
// size. A name that resolves == a real file (one-way hash matches).
mcp::ToolResult toolRecover(const mcp::json& params) {
    std::string archive  = reqStr(params, "archive");
    std::string dictPath = reqStr(params, "dict_path");
    auto a = openOrThrow(archive);

    std::ifstream f(engine::Archive::normalizePath(dictPath));
    if (!f)
        throw std::runtime_error("cannot open dictionary: " + dictPath);

    mcp::json recovered = mcp::json::array();
    unsigned checked = 0, found = 0;
    std::string line;
    while (std::getline(f, line)) {
        // strip trailing CR/LF
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty())
            continue;
        checked++;
        uint64_t sz = 0;
        if (a.verify(line) && a.querySize(line, sz)) {
            recovered.push_back(mcp::json{{"name", line}, {"size", sz}});
            found++;
        }
    }

    mcp::ToolResult r;
    r.value = mcp::json{{"archive",   archive},
                        {"dict_path", dictPath},
                        {"checked",   checked},
                        {"found",     found},
                        {"recovered", recovered}};
    return r;
}

// Port of the legacy `mpqextract dump` subcommand: extract a file and return a
// hex preview of its first bytes (the one feature not otherwise covered by the
// other mpq.* tools). Handy for quickly eyeballing a file's raw header.
mcp::ToolResult toolDump(const mcp::json& params) {
    std::string archive = reqStr(params, "archive");
    std::string name    = reqStr(params, "name");
    int maxBytes        = params.value("max_bytes", 256);
    auto a = openOrThrow(archive);

    std::vector<uint8_t> buf;
    if (!a.extract(name, buf))
        throw std::runtime_error("extract failed (name not found?): " + name);

    size_t n = buf.size();
    if (maxBytes > 0 && (size_t)maxBytes < n) n = (size_t)maxBytes;

    static const char* hexd = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        hex.push_back(hexd[buf[i] >> 4]);
        hex.push_back(hexd[buf[i] & 0x0F]);
        if ((i & 0x0F) == 0x0F) hex.push_back('\n');
        else if ((i & 3) == 3)   hex.push_back(' ');
    }

    mcp::ToolResult r;
    r.value = mcp::json{{"name",         name},
                        {"size",         buf.size()},
                        {"preview_hex",  hex}};
    return r;
}

} // namespace

namespace mcp {
namespace tools {

void registerMpqTools(ToolRegistry& reg) {
    reg.registerTool(ToolDef{
        "mpq.list",
        "List every file in an MPQ archive. Without a (listfile) the names are "
        "synthesized placeholders (FileNNNNNNN.xxx); use mpq.recover to resolve "
        "real names.",
        json{{"type", "object"},
             {"properties", json{{"archive", json{{"type","string"},
                                                 {"description","path to .mpq"}}}}},
             {"required", json::array({"archive"})}},
        toolList});

    reg.registerTool(ToolDef{
        "mpq.extract",
        "Extract one file from an MPQ by its in-archive name. By default writes "
        "to ./out/<name>; pass as_base64=true to return the bytes inline instead.",
        json{{"type", "object"},
             {"properties", json{
                 {"archive",    json{{"type","string"}}},
                 {"name",       json{{"type","string"},
                                     {"description","in-archive name, e.g. gendata\\cut2.pal"}}},
                 {"out",        json{{"type","string"}}},
                 {"as_base64",  json{{"type","boolean"}}},
             }},
             {"required", json::array({"archive", "name"})}},
        toolExtract});

    reg.registerTool(ToolDef{
        "mpq.verify",
        "Return whether a given in-archive name resolves to a real file "
        "(hash lookup). Used to validate candidate filenames.",
        json{{"type", "object"},
             {"properties", json{
                 {"archive", json{{"type","string"}}},
                 {"name",    json{{"type","string"}}},
             }},
             {"required", json::array({"archive", "name"})}},
        toolVerify});

    reg.registerTool(ToolDef{
        "mpq.recover",
        "Filename recovery: read a dictionary file (one candidate MPQ name per "
        "line) and report which names resolve in the archive, with sizes. A "
        "resolving name proves it is a real file.",
        json{{"type", "object"},
             {"properties", json{
                 {"archive",   json{{"type","string"}}},
                 {"dict_path", json{{"type","string"},
                                    {"description","path to candidate-name list"}}},
             }},
             {"required", json::array({"archive", "dict_path"})}},
        toolRecover});

    reg.registerTool(ToolDef{
        "mpq.dump",
        "Extract a file and return a hex preview of its first bytes (default 256), "
        "plus its total size. Quick way to eyeball a file's raw header.",
        json{{"type", "object"},
             {"properties", json{
                 {"archive",   json{{"type","string"}}},
                 {"name",      json{{"type","string"},
                                   {"description","in-archive name, e.g. gendata\\cut2.pal"}}},
                 {"max_bytes", json{{"type","integer"},
                                   {"description","preview length in bytes (default 256)"}}},
             }},
             {"required", json::array({"archive", "name"})}},
        toolDump});
}

} // namespace tools
} // namespace mcp
