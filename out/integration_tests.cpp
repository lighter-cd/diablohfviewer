// Integration tests for the engine (StormLib wrapper) and the mpq_tools group.
//
// These exercise the real StormLib-backed paths:
//   - engine::Archive: normalizePath, open, list, verify, querySize, extract,
//     extractToFile
//   - mpq_tools: registerMpqTools + dispatch of mpq.list / mpq.verify /
//     mpq.extract / mpq.dump / mpq.recover through the JSON-RPC layer.
//
// A real Diablo 1 / Hellfire MPQ is REQUIRED. Resolve it from the env var
// DHFC_TEST_MPQ, defaulting to the local DevilutionX install. If the fixture is
// absent the MPQ-dependent suites print SKIP and still exit 0 (so the suite can
// run in environments without the game assets); the pure normalizePath checks
// always run.
//
// Build & run (MinGW-w64, from repo root):
//   g++ -I 3rdparty/StormLib/src -I 3rdparty -I src -std=c++17 \
//       out/integration_tests.cpp \
//       src/engine/archive.cpp src/tools/registry.cpp \
//       src/tools/mpq_tools.cpp src/tools/sprite_tools.cpp \
//       src/decoders/palette.cpp src/decoders/clx.cpp src/decoders/cel.cpp \
//       src/decoders/cl2.cpp src/decoders/pcx.cpp src/decoders/sprite_width.cpp \
//       src/mcp/jsonrpc.cpp \
//       3rdparty/StormLib/build/libStormLib.a \
//       -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
//       -o out/integration_tests.exe && out/integration_tests.exe
//

// Exit code 0 = all pass (or skip), 1 = at least one failure.

#include "engine/archive.h"
#include "tools/registry.h"
#include "tools/mpq_tools.h"
#include "tools/sprite_tools.h"
#include "tools/base64.h"
#include "mcp/jsonrpc.h"

#include <cstdio>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace mcp;
namespace engine = engine;

static bool allOk = true;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            allOk = false;                                                  \
            printf("  FAIL: %s\n", msg);                                    \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// engine::Archive::normalizePath  (pure, no MPQ needed)
// ---------------------------------------------------------------------------
static void testNormalize() {
    CHECK(engine::Archive::normalizePath("/d/foo/bar") == "D:/foo/bar",
          "normalize /d/foo/bar -> D:/foo/bar");
    CHECK(engine::Archive::normalizePath("/c/") == "C:/",
          "normalize /c/ -> C:/");
    CHECK(engine::Archive::normalizePath("D:/foo/bar") == "D:/foo/bar",
          "windows path passes through unchanged");
    CHECK(engine::Archive::normalizePath("C:\\x\\y") == "C:\\x\\y",
          "backslash windows path passes through");
    CHECK(engine::Archive::normalizePath("relative/path") == "relative/path",
          "relative path passes through");
    printf("normalizePath: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// engine::Archive against a real MPQ
// ---------------------------------------------------------------------------
static void testEngineArchive(const std::string& mpq) {
    engine::Archive a;
    CHECK(a.open(mpq), "Archive::open(real mpq) succeeds");
    if (!a.isOpen()) return;
    CHECK(a.isOpen(), "isOpen() true after open");

    auto entries = a.list();
    CHECK(!entries.empty(), "list() returns non-empty population");
    if (entries.empty()) return;

    const std::string firstName = entries[0].name;
    uint64_t listSize = entries[0].size;

    CHECK(a.verify(firstName), "verify(known name) == true");
    CHECK(!a.verify("__definitely_not_a_real_file__.xyz"),
          "verify(bogus name) == false");

    uint64_t qsz = 0;
    CHECK(a.querySize(firstName, qsz), "querySize(known) succeeds");
    CHECK(qsz == listSize, "querySize matches list() size");
    CHECK(qsz > 0, "querySize > 0");

    std::vector<uint8_t> buf;
    CHECK(a.extract(firstName, buf), "extract(known) succeeds");
    CHECK(buf.size() == (size_t)qsz, "extracted buffer size == querySize");

    std::string tmpPath = "out/_it_extract_tmp.bin";
    CHECK(a.extractToFile(firstName, tmpPath), "extractToFile succeeds");
    {
        std::ifstream f(tmpPath, std::ios::binary | std::ios::ate);
        CHECK(f.good() && (uint64_t)f.tellg() == qsz, "extractToFile wrote right size");
    }
    std::remove(tmpPath.c_str());

    // Close automatically when `a` leaves scope; mpq_tools dispatch in the next
    // suite opens its own handle (two simultaneous handles on the same file
    // trip Windows ERROR_SHARING_VIOLATION).
    printf("engine::Archive: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// mpq_tools group via the JSON-RPC layer against a real MPQ
// ---------------------------------------------------------------------------
static void testMpqTools(const std::string& mpq) {
    ToolRegistry reg;
    tools::registerMpqTools(reg);
    CHECK(reg.has("mpq.list") && reg.has("mpq.extract") &&
              reg.has("mpq.verify") && reg.has("mpq.recover") && reg.has("mpq.dump"),
          "all mpq.* tools registered");

    auto call = [&](const std::string& tool, const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", args}}}});
        CHECK(resp.has_value(), ("tools/call " + tool + " returns response").c_str());
        CHECK(!resp->contains("error"), ("tools/call " + tool + " no error").c_str());
        return (*resp)["result"];
    };

    // mpq.list
    json lst = call("mpq.list", json{{"archive", mpq}});
    CHECK(lst.value("count", 0) > 0, "mpq.list count > 0");
    CHECK(lst["files"].is_array() && !lst["files"].empty(), "mpq.list files array");
    const std::string firstName = lst["files"][0].value("name", "");
    CHECK(!firstName.empty(), "mpq.list first file has a name");

    // mpq.verify
    json vok = call("mpq.verify", json{{"archive", mpq}, {"name", firstName}});
    CHECK(vok.value("exists", false), "mpq.verify(known) exists == true");
    json vno = call("mpq.verify",
                    json{{"archive", mpq}, {"name", "__nope__.xyz"}});
    CHECK(!vno.value("exists", true), "mpq.verify(bogus) exists == false");

    // mpq.extract as_base64
    json ex = call("mpq.extract",
                   json{{"archive", mpq}, {"name", firstName}, {"as_base64", true}});
    CHECK(ex.contains("base64"), "mpq.extract base64 present");
    auto raw = base64::decode(ex.value("base64", std::string()));
    uint64_t qsz = 0;
    {
        engine::Archive a;
        a.open(mpq);
        a.querySize(firstName, qsz);
    }
    CHECK(raw.size() == (size_t)qsz, "mpq.extract base64 decodes to full size");

    // mpq.extract (default -> file on disk)
    json exf = call("mpq.extract", json{{"archive", mpq}, {"name", firstName}});
    CHECK(exf.contains("saved_path"), "mpq.extract default returns saved_path");
    {
        std::ifstream f(exf.value("saved_path", std::string()), std::ios::binary);
        CHECK(f.good(), "mpq.extract saved file exists");
    }
    // Close the stream first; on Windows a file in use cannot be deleted.
    std::remove(exf.value("saved_path", "").c_str());

    // mpq.dump
    json dp = call("mpq.dump", json{{"archive", mpq}, {"name", firstName}});
    CHECK(dp.value("size", 0) > 0, "mpq.dump size > 0");
    CHECK(!dp.value("preview_hex", std::string()).empty(), "mpq.dump preview non-empty");

    // mpq.recover with a self-generated dict: one real name + one bogus name.
    // This makes the assertion exact and removes any dependency on the bundled
    // dictionary file or the current working directory.
    std::string dictPath = "out/_it_recover_dict.txt";
    {
        std::ofstream d(dictPath);
        d << firstName << "\n";
        d << "__definitely_not_real__.xyz\n";
    }
    json rec = call("mpq.recover", json{{"archive", mpq}, {"dict_path", dictPath}});
    std::remove(dictPath.c_str());
    CHECK(rec.value("checked", 0) == 2, "mpq.recover checked == 2");
    CHECK(rec.value("found", 0) == 1, "mpq.recover found == 1");
    CHECK(rec["recovered"].size() == 1, "mpq.recover recovered array size == 1");

    // mpq.recover against the bundled real dictionary, if present (validates the
    // whole harvest->recover pipeline end to end).
    std::string realDict = "mpq-info/dict/diablo1-harvest.txt";
    {
        std::ifstream df(realDict);
        if (df.good()) {
            json rec2 = call("mpq.recover",
                             json{{"archive", mpq}, {"dict_path", realDict}});
            CHECK(rec2.value("checked", 0) > 0, "mpq.recover(real dict) checked > 0");
            CHECK(rec2.value("found", 0) >= 1, "mpq.recover(real dict) found >= 1");
        } else {
            printf("  (skipped bundled-dict recover; %s not present)\n", realDict.c_str());
        }
    }

    printf("mpq_tools: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// sprite_tools group (sprite.list / sprite.decode) via the JSON-RPC layer
// ---------------------------------------------------------------------------

// sprite.list runs WITHOUT a real MPQ: it only reads recovered.json. A small
// synthetic dict keeps the test independent of the bundled dictionary / cwd.
// Width resolves to 0 when the DevilutionX tables are absent (autoWidth returns
// 0), which is fine for these assertions.
static void testSpriteList() {
    ToolRegistry reg;
    tools::registerSpriteTools(reg);
    CHECK(reg.has("sprite.list") && reg.has("sprite.decode") &&
              reg.has("palette.load"),
          "all sprite.* + palette.load tools registered");

    auto call = [&](const std::string& tool, const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", args}}}});
        CHECK(resp.has_value(), ("tools/call " + tool + " returns response").c_str());
        CHECK(!resp->contains("error"), ("tools/call " + tool + " no error").c_str());
        return (*resp)["result"];
    };

    std::string dictPath = "out/_it_sprite_dict.json";
    {
        std::ofstream d(dictPath);
        d << "{ \"archives\": [ { \"name\": \"DIABDAT.MPQ\", \"path\": \"D:/fixtures/DIABDAT.MPQ\", \"recovered\": [ ";
        d << "{ \"name\": \"plrgfx\\\\warrior\\\\wha\\\\whaas.cl2\", \"size\": 1 }, ";
        d << "{ \"name\": \"plrgfx\\\\warrior\\\\wha\\\\whaat.cl2\", \"size\": 1 }, ";
        d << "{ \"name\": \"items\\\\armor2.cel\", \"size\": 1 }, ";
        d << "{ \"name\": \"gendata\\\\cut2.pal\", \"size\": 1 } ";
        d << "] } ] }";
    }
    json lst = call("sprite.list", json{{"dict", dictPath}});

    CHECK(lst.value("count", 0) == 3, "sprite.list enumerates 3 CL2/CEL (palette excluded)");
    int nWarrior = 0, nItems = 0;
    for (auto& s : lst["sprites"]) {
        CHECK(s.contains("width") && s["width"].is_number_integer(),
              "each sprite entry has integer width");
        if (s.value("family", "") == "plrgfx" && s.value("class", "") == "warrior")
            nWarrior++;
        if (s.value("family", "") == "items")
            nItems++;
    }
    CHECK(nWarrior == 2, "sprite.list classified 2 warrior plrgfx sprites");
    CHECK(nItems == 1, "sprite.list classified 1 items sprite");
    // archive_path is surfaced from recovered.json so the frontend need not ask.
    bool hasPath = false;
    for (auto& s : lst["sprites"])
        if (s.value("archive_path", std::string()) == "D:/fixtures/DIABDAT.MPQ")
            hasPath = true;
    CHECK(hasPath, "sprite.list surfaces archive_path from recovered.json");

    // kind:"palette" must enumerate only .pal files (here the gendata\cut2.pal).
    json pal = call("sprite.list", json{{"dict", dictPath}, {"kind", "palette"}});
    CHECK(pal.value("count", -1) == 1, "sprite.list kind:palette enumerates 1 .pal");
    if (pal.value("count", 0) == 1) {
        const auto& p = pal["sprites"][0];
        CHECK(p.value("family", "") == "palette", "palette entry family == palette");
        CHECK(p.value("name", "") == "gendata\\cut2.pal", "palette entry name matches");
    }
    // kind:"all" returns sprites + palettes together.
    json all = call("sprite.list", json{{"dict", dictPath}, {"kind", "all"}});
    CHECK(all.value("count", -1) == 4, "sprite.list kind:all enumerates 3 sprites + 1 palette");

    std::remove(dictPath.c_str());
    printf("sprite_tools (list): %s\n", allOk ? "PASS" : "FAIL");
}

// sprite.decode needs a real MPQ; gated by the caller. We probe the MPQ for a
// real CLX sprite + a real palette name so the test is fixture-independent. CLX
// carries its own dimensions, so no 'width' is needed. The CEL/CL2 width:"auto"
// path is covered by the sprite_width unit test and manual checks.
static void testSpriteDecode(const std::string& mpq) {
    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerSpriteTools(reg);

    auto call = [&](const std::string& tool, const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", args}}}});
        CHECK(resp.has_value(), ("tools/call " + tool + " returns response").c_str());
        CHECK(!resp->contains("error"), ("tools/call " + tool + " no error").c_str());
        return (*resp)["result"];
    };

    // Find a CLX + a PAL by listing the archive.
    json lst = call("mpq.list", json{{"archive", mpq}});
    std::string clxName, palName;
    for (auto& f : lst["files"]) {
        std::string n = f.value("name", "");
        if (clxName.empty() && n.size() >= 4 &&
            n.compare(n.size() - 4, 4, ".clx") == 0)
            clxName = n;
        if (palName.empty() && n.size() >= 4 &&
            n.compare(n.size() - 4, 4, ".pal") == 0)
            palName = n;
    }
    if (clxName.empty() || palName.empty()) {
        printf("  (skipped sprite.decode; MPQ has no CLX/PAL to decode)\n");
        printf("sprite_tools (decode): %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

    // Decode the CLX with the palette resolved by name from the MPQ.
    json dec = call("sprite.decode",
                    json{{"archive", mpq},
                         {"name", clxName},
                         {"palette_name", palName}});
    CHECK(dec.value("format", std::string()) == "clx", "sprite.decode format == clx");
    CHECK(dec.value("frame_count", 0) > 0, "sprite.decode frame_count > 0");
    CHECK(dec.contains("frames") && dec["frames"].is_array() &&
              !dec["frames"].empty(),
          "sprite.decode frames array non-empty");
    CHECK(dec["frames"][0].contains("rgba_base64"),
          "sprite.decode frame has rgba_base64");
    CHECK(dec["frames"][0].value("width", 0) > 0 &&
              dec["frames"][0].value("height", 0) > 0,
          "sprite.decode frame has positive width/height");

    // Exercise the new `archives` parameter (extra MPQ stack): pass the same MPQ
    // again so the dedupe + extractBufAny path is exercised. Cross-MPQ resolution
    // (palette in a *different* MPQ than the sprite) is verified manually with
    // DIABDAT + hellfire MPQs, since this suite has a single fixture.
    json dec2 = call("sprite.decode",
                     json{{"archive", mpq},
                          {"archives", json::array({mpq})},
                          {"name", clxName},
                          {"palette_name", palName}});
    CHECK(dec2.value("format", std::string()) == "clx",
          "sprite.decode with archives param still decodes");
    CHECK(dec2.value("frame_count", 0) > 0,
          "sprite.decode with archives param frame_count > 0");

    printf("sprite_tools (decode): %s\n", allOk ? "PASS" : "FAIL");
}

int main() {
    printf("=== DiabloHFConverter integration tests (engine + mpq_tools + sprite_tools) ===\n");
    testNormalize();
    testSpriteList();

    std::string mpq = "/d/work/xt/diablo/DevilutionX-Hell Fire/devilutionx.mpq";
    if (const char* e = std::getenv("DHFC_TEST_MPQ"))
        mpq = e;

    {
        // std::ifstream does not apply the same POSIX->Windows normalization
        // that Archive::open does, so normalize the path before probing.
        std::ifstream f(engine::Archive::normalizePath(mpq));
        if (!f.good()) {
            printf("engine::Archive + mpq_tools + sprite.decode: SKIP (fixture not found: %s)\n", mpq.c_str());
            printf("=== %s (with skips) ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
            return allOk ? 0 : 1;
        }
    }

    testEngineArchive(mpq);
    testMpqTools(mpq);
    testSpriteDecode(mpq);
    printf("=== %s ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
    return allOk ? 0 : 1;
}
