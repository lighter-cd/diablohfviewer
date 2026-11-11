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
//       tests/integration_tests.cpp \
//       src/engine/archive.cpp src/tools/registry.cpp \
//       src/tools/mpq_tools.cpp src/tools/sprite_tools.cpp src/tools/ui_tools.cpp \
//       src/tools/scene_tools.cpp \
//       src/decoders/palette.cpp src/decoders/clx.cpp src/decoders/cel.cpp \
//       src/decoders/cl2.cpp src/decoders/pcx.cpp src/decoders/sprite_width.cpp \
//       src/decoders/png.cpp src/decoders/deflate.cpp \
//       src/decoders/level_decode.cpp src/decoders/level_cel.cpp \
//       src/decoders/level_render.cpp src/decoders/level_scene.cpp \
//       src/mcp/jsonrpc.cpp \
//       3rdparty/StormLib/build/libStormLib.a \
//       -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
//       -o tests/integration_tests.exe && tests/integration_tests.exe
//

// Exit code 0 = all pass (or skip), 1 = at least one failure.

#include "engine/archive.h"
#include "tools/registry.h"
#include "tools/mpq_tools.h"
#include "tools/sprite_tools.h"
#include "tools/scene_tools.h"
#include "tools/level_tools.h"
#include "tools/ui_tools.h"
#include "tools/ui_layout.h"
#include "tools/base64.h"
#include "mcp/jsonrpc.h"
#include "decoders/level.h"
#include "decoders/palette.h"
#include "decoders/inv_widths.h"

#include <algorithm>
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

    std::string tmpPath = "tests/_it_extract_tmp.bin";
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
    std::string dictPath = "tests/_it_recover_dict.txt";
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

    std::string dictPath = "tests/_it_sprite_dict.json";
    {
        std::ofstream d(dictPath);
        d << "{ \"archives\": [ { \"name\": \"DIABDAT.MPQ\", \"path\": \"D:/fixtures/DIABDAT.MPQ\", \"recovered\": [ ";
        d << "{ \"name\": \"plrgfx\\\\warrior\\\\wha\\\\whaas.cl2\", \"size\": 1 }, ";
        d << "{ \"name\": \"plrgfx\\\\warrior\\\\wha\\\\whaat.cl2\", \"size\": 1 }, ";
        d << "{ \"name\": \"items\\\\armor2.cel\", \"size\": 1 }, ";
        d << "{ \"name\": \"gendata\\\\cut2.pal\", \"size\": 1 } ";
        d << "] }, ";
        // devilutionx-style archive: ONLY .clx UI art, no CL2/CEL/pal sprites.
        // It must still appear in archives[] -- deriving the stack from
        // sprites[] alone is exactly the bug that made charbg.clx "missing".
        d << "{ \"name\": \"devilutionx.mpq\", \"path\": \"D:/fixtures/devilutionx.mpq\", \"recovered\": [ ";
        d << "{ \"name\": \"data\\\\charbg.clx\", \"size\": 1 }, ";
        d << "{ \"name\": \"data\\\\stash.clx\", \"size\": 1 } ";
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
    // items CEL now resolves a real auto width (DevilutionX ItemAnimWidth=96)
    // instead of 0, so the directory browser can decode it without a manual width.
    for (auto& s : lst["sprites"])
        if (s.value("name", "") == "items\\armor2.cel")
            CHECK(s.value("width", 0) == 96, "sprite.list items\\armor2.cel width == 96");
    // archive_path is surfaced from recovered.json so the frontend need not ask.
    bool hasPath = false;
    for (auto& s : lst["sprites"])
        if (s.value("archive_path", std::string()) == "D:/fixtures/DIABDAT.MPQ")
            hasPath = true;
    CHECK(hasPath, "sprite.list surfaces archive_path from recovered.json");
    // Full MPQ stack: EVERY archive from recovered.json, including the one with
    // no CL2/CEL sprites (devilutionx.mpq holds only .clx UI art). The frontend
    // passes this whole stack to ui.build / scene.build.
    CHECK(lst.contains("archives") && lst["archives"].is_array() &&
              lst["archives"].size() == 2,
          "sprite.list returns archives[] with every MPQ (incl. no-sprite ones)");
    bool hasDiabdat = false, hasDx = false, dup = false;
    std::vector<std::string> seen;
    for (auto& a : lst["archives"]) {
        CHECK(a.is_string(), "archives[] entries are path strings");
        std::string p = a.get<std::string>();
        if (p == "D:/fixtures/DIABDAT.MPQ") hasDiabdat = true;
        if (p == "D:/fixtures/devilutionx.mpq") hasDx = true;
        if (std::find(seen.begin(), seen.end(), p) != seen.end()) dup = true;
        seen.push_back(p);
    }
    CHECK(hasDiabdat && hasDx, "archives[] covers both sprite and no-sprite archives");
    CHECK(!dup, "archives[] is deduplicated");
    // The .clx-only archive must NOT add any enumerated sprite/palette.
    CHECK(lst.value("count", -1) == 3, "no-sprite archive does not inflate sprite count");

    // kind:"palette" must enumerate only .pal files (here the gendata\cut2.pal).
    json pal = call("sprite.list", json{{"dict", dictPath}, {"kind", "palette"}});
    CHECK(pal.value("count", -1) == 1, "sprite.list kind:palette enumerates 1 .pal");
    CHECK(pal.value("archives", json::array()).size() == 2,
          "kind:palette still returns the full archives[] stack");
    if (pal.value("count", 0) == 1) {
        const auto& p = pal["sprites"][0];
        CHECK(p.value("family", "") == "palette", "palette entry family == palette");
        CHECK(p.value("name", "") == "gendata\\cut2.pal", "palette entry name matches");
    }
    // kind:"all" returns sprites + palettes together.
    json all = call("sprite.list", json{{"dict", dictPath}, {"kind", "all"}});
    CHECK(all.value("count", -1) == 4, "sprite.list kind:all enumerates 3 sprites + 1 palette");
    CHECK(all.value("archives", json::array()).size() == 2,
          "kind:all still returns the full archives[] stack");

    std::remove(dictPath.c_str());
    printf("sprite_tools (list): %s\n", allOk ? "PASS" : "FAIL");
}

// Monster / NPC catalog synthesis from the DevilutionX data tables. No MPQ is
// needed (it reads monstdat.tsv / towners.tsv under the DevilutionX root, not
// the game archives), so it runs even without game assets.
static void testSpriteListSynthesis() {
    ToolRegistry reg;
    tools::registerSpriteTools(reg);
    auto call = [&](const std::string& tool, const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", args}}}});
        CHECK(resp.has_value(), ("tools/call " + tool + " returns response").c_str());
        CHECK(!resp->contains("error"), ("tools/call " + tool + " no error").c_str());
        return (*resp)["result"];
    };

    json mon = call("sprite.list", json{{"family", "monsters"}});
    CHECK(mon.value("count", 0) > 0, "monster catalog synthesized (count > 0)");
    bool hasZombie = false;
    for (auto& s : mon["sprites"]) {
        if (s.value("name", "") == "monsters\\zombie\\zombien.cl2") {
            hasZombie = true;
            CHECK(s.value("anim", "") == "Stand", "zombie Stand anim label == Stand");
            CHECK(s.value("frames_per_dir", 0) == 11, "zombie Stand frames_per_dir == 11");
            CHECK(s.value("width", 0) > 0, "zombie Stand width resolved > 0");
        }
    }
    CHECK(hasZombie, "monster catalog includes monsters\\zombie\\zombien.cl2");

    json twn = call("sprite.list", json{{"family", "towners"}});
    CHECK(twn.value("count", 0) > 0, "towner catalog synthesized (count > 0)");
    bool hasCow = false;
    for (auto& s : twn["sprites"])
        if (s.value("name", "") == "towners\\animals\\cow.cel")
            hasCow = true;
    CHECK(hasCow, "towner catalog includes hardcoded cow (towners\\animals\\cow.cel)");

    printf("sprite_tools (synthesis): %s\n", allOk ? "PASS" : "FAIL");
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

// TRN remap toggle: verifies sprite.decode honours apply_trn + trn_name.
// Uses a self-describing CLX sprite + a real .trn from the same MPQ (so no width
// table is needed). SKIPs if the fixture has no CLX/TRN to exercise.
static void testSpriteDecodeTrn(const std::string& mpq) {
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

    json lst = call("mpq.list", json{{"archive", mpq}});
    std::string clxName, trnName, palName;
    for (auto& f : lst["files"]) {
        std::string n = f.value("name", "");
        auto ends = [&](const char* suf) {
            return n.size() >= std::strlen(suf) &&
                   n.compare(n.size() - std::strlen(suf), std::strlen(suf), suf) == 0;
        };
        if (clxName.empty() && ends(".clx")) clxName = n;
        if (trnName.empty() && ends(".trn")) trnName = n;
        if (palName.empty() && ends(".pal")) palName = n;
    }
    if (clxName.empty() || trnName.empty() || palName.empty()) {
        printf("  (skipped sprite.decode TRN; fixture lacks CLX/TRN/PAL)\n");
        printf("sprite_tools (trn): %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

    // Default (apply_trn omitted): no TRN applied.
    json def = call("sprite.decode", json{{"archive", mpq},
                                          {"name", clxName},
                                          {"palette_name", palName}});
    CHECK(def.value("trn_applied", true) == false,
          "sprite.decode without apply_trn leaves trn_applied false");

    // apply_trn with a real existing TRN name -> applied true, still decodes.
    json on = call("sprite.decode", json{{"archive", mpq},
                                         {"name", clxName},
                                         {"palette_name", palName},
                                         {"apply_trn", true},
                                         {"trn_name", trnName}});
    CHECK(on.value("trn_applied", false) == true,
          "sprite.decode apply_trn+trn_name sets trn_applied true");
    CHECK(on.value("trn_name", std::string()) == trnName,
          "sprite.decode reports the applied trn_name");
    CHECK(on.value("frame_count", 0) > 0,
          "sprite.decode with TRN still yields frames");

    // apply_trn with a missing TRN name -> applied false, still decodes (palette-only).
    json miss = call("sprite.decode", json{{"archive", mpq},
                                           {"name", clxName},
                                           {"palette_name", palName},
                                           {"apply_trn", true},
                                           {"trn_name", "plrgfx\\__nope__.trn"}});
    CHECK(miss.value("trn_applied", true) == false,
          "sprite.decode apply_trn with missing trn_name leaves trn_applied false");
    CHECK(miss.value("frame_count", 0) > 0,
          "sprite.decode with missing TRN still decodes (palette-only)");

    printf("sprite_tools (trn): %s\n", allOk ? "PASS" : "FAIL");
}

// Monster direction count: sprite.decode returns `directions` = frame_count /
// frames-per-dir (frames-per-dir looked up from monstdat.tsv by the sprite's
// animation letter). Verified against the well-known zombie Stand animation
// (11 frames/dir * 8 dirs = 88 frames -> directions 8).
//
// The repo's default devilutionx.mpq does not ship Diablo-1 monster CL2s, so
// this test locates an MPQ that actually contains the zombie sprite (DIABDAT or
// a user-supplied DHFC_TEST_MPQ) and SKIPs cleanly otherwise.
static std::string findMonsterMpq() {
    std::vector<std::string> cands;
    if (const char* e = std::getenv("DHFC_TEST_MPQ"))
        cands.push_back(e);
    cands.push_back("/d/work/xt/diablo/DevilutionX-Hell Fire/devilutionx.mpq");
    cands.push_back("/d/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ");
    for (auto& c : cands) {
        std::ifstream f(engine::Archive::normalizePath(c));
        if (!f.good())
            continue;
        engine::Archive a;
        if (a.open(c) && a.verify("monsters\\zombie\\zombien.cl2"))
            return c;
    }
    return "";
}

static void testSpriteDecodeMonster() {
    std::string mpq = findMonsterMpq();
    if (mpq.empty()) {
        printf("  (skipped monster dirs; no MPQ with monsters\\zombie\\zombien.cl2)\n");
        printf("sprite_tools (monster dirs): %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

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

    // A palette is required to decode; reuse a known .pal from the MPQ.
    static const char* palCands[] = {
        "gendata\\cut2.pal", "levels\\l1data\\l1.pal", "plrgfx\\plrgfx1.pal"};
    std::string palName;
    for (const char* p : palCands) {
        json v = call("mpq.verify", json{{"archive", mpq}, {"name", std::string(p)}});
        if (v.value("exists", false)) { palName = p; break; }
    }
    CHECK(!palName.empty(), "monster fixture has a .pal for decoding");
    if (palName.empty()) {
        printf("sprite_tools (monster dirs): %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

    json dec = call("sprite.decode",
                    json{{"archive", mpq},
                         {"name", "monsters\\zombie\\zombien.cl2"},
                         {"width", "auto"},
                         {"palette_name", palName}});
    CHECK(dec.value("format", std::string()) == "cl2", "zombie decode format == cl2");
    // zombien = Stand animation: 11 frames/dir * 8 dirs = 88 frames.
    CHECK(dec.value("frame_count", 0) == 88, "zombie Stand frame_count == 88");
    CHECK(dec.value("directions", 0) == 8, "zombie Stand directions == 8");
    CHECK(dec.value("frame_count", 0) % dec.value("directions", 1) == 0,
          "frame_count divisible by directions");

    printf("sprite_tools (monster dirs): %s\n", allOk ? "PASS" : "FAIL");
}

// A Hellfire monster CL2 lives in hellfire.mpq, NOT in the base DIABDAT.MPQ.
// The frontend monster viewer supplies the whole MPQ stack via `archives`, so
// sprite.decode must find the sprite in a *different* MPQ than `archive`.
// Regression test for the "extract failed (name not found?)" failures on
// monsters\antworm\wormn.cl2, monsters\bspidr\bspidrn.cl2, ... which occurred
// when only the single MPQ picked in the viewer was searched.
static void testSpriteDecodeCrossArchive() {
    std::string base = findMonsterMpq();   // has zombie, but not Hellfire monsters
    std::string hf;
    {
        size_t i = base.find_last_of("/\\");
        if (i != std::string::npos)
            hf = base.substr(0, i + 1) + "hellfire.mpq";
    }
    if (base.empty() || hf.empty()) {
        printf("  (skipped cross-archive decode; no monster fixture)\n");
        return;
    }
    std::ifstream f(engine::Archive::normalizePath(hf));
    if (!f.good()) {
        printf("  (skipped cross-archive decode; hellfire.mpq not found)\n");
        return;
    }

    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerSpriteTools(reg);
    auto raw = [&](const std::string& tool, const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", args}}}});
        CHECK(resp.has_value(), "cross-archive: tools/call returns a response");
        return resp ? *resp : json::object();
    };
    auto resultOf = [&](const std::string& tool, const json& args) -> json {
        return raw(tool, args).value("result", json::object());
    };

    const char* name = "monsters\\antworm\\wormn.cl2";
    const char* pal  = "gendata\\cut2.pal";

    json vb = resultOf("mpq.verify", json{{"archive", base}, {"name", name}});
    CHECK(!vb.value("exists", true), "Hellfire monster absent from the base MPQ");
    json vh = resultOf("mpq.verify", json{{"archive", hf}, {"name", name}});
    CHECK(vh.value("exists", false), "Hellfire monster present in hellfire.mpq");

    // Single-MPQ decode must fail: this is exactly the bug the viewer hit.
    json single = raw("sprite.decode", json{
        {"archive", base}, {"name", name}, {"width", "auto"},
        {"palette_name", pal}});
    CHECK(single.contains("error"),
          "single-MPQ decode of a Hellfire monster fails as expected");

    // With the full stack supplied via `archives` it must succeed.
    json stacked = resultOf("sprite.decode", json{
        {"archive", base}, {"archives", json::array({base, hf})},
        {"name", name}, {"width", "auto"}, {"palette_name", pal}});
    CHECK(stacked.value("format", std::string()) == "cl2",
          "cross-archive decode format == cl2");
    CHECK(stacked.value("frame_count", 0) > 0,
          "cross-archive decode returns frames");

    printf("sprite_tools (cross-archive): %s\n", allOk ? "PASS" : "FAIL");
}

// A truncated CL2 (the 96-byte monsters\darkmage\dmagew.cl2 stub in DIABDAT)
// must be rejected with a clean JSON-RPC error instead of crashing the server.
// If cl2ToClx regresses to an out-of-bounds read, this process dies here
// (SIGSEGV) and the suite never reaches "ALL PASS".
static void testSpriteDecodeRejectsTruncatedCl2() {
    std::string mpq = findMonsterMpq();
    if (mpq.empty()) {
        printf("  (skipped truncated-CL2 decode; no MPQ fixture)\n");
        return;
    }
    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerSpriteTools(reg);
    auto resp = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", json{{"name", "sprite.decode"},
                        {"arguments", json{
                            {"archive", mpq},
                            {"name", "monsters\\darkmage\\dmagew.cl2"},
                            {"width", "auto"},
                            {"palette_name", "gendata\\cut2.pal"}}}}}});
    CHECK(resp.has_value(), "truncated CL2: server returned a response (did not crash)");
    if (!resp) { printf("sprite_tools (truncated CL2): FAIL\n"); return; }
    CHECK(resp->contains("error"), "truncated CL2: returns a JSON-RPC error");
    json err  = resp->value("error", json::object());
    json data = err.value("data", json::object());
    std::string detail = data.value("error", std::string());
    CHECK(detail.find("invalid CL2") != std::string::npos,
          "truncated CL2: error reports 'invalid CL2'");
    printf("       detail: %s\n", detail.c_str());
    printf("sprite_tools (truncated CL2): %s\n", allOk ? "PASS" : "FAIL");
}

// sprite.list catalog fixes: the hardcoded cow carries its real width (128),
// ghost `worm\worm` entries are gone, but other availability=Never monsters
// (golem, unrav) must NOT be over-filtered.
static void testSpriteListCatalogFixes() {
    ToolRegistry reg;
    tools::registerSpriteTools(reg);
    auto list = [&](const std::string& fam) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", "sprite.list"},
                            {"arguments", json{{"family", fam}}}}}});
        CHECK(resp.has_value(), "catalog fixes: sprite.list returns a response");
        return resp ? resp->value("result", json::object()) : json::object();
    };

    json twn = list("towners");
    json twnSprites = twn.value("sprites", json::array());
    bool cowFound = false;
    for (auto& s : twnSprites) {
        if (s.value("name", "") == "towners\\animals\\cow.cel") {
            cowFound = true;
            CHECK(s.value("width", 0) == 128, "cow.cel catalog width == 128");
        }
    }
    CHECK(cowFound, "towner catalog still includes the hardcoded cow");

    json mon = list("monsters");
    json monSprites = mon.value("sprites", json::array());
    bool hasWorm = false, hasGolem = false, hasUnrav = false;
    for (auto& s : monSprites) {
        std::string n = s.value("name", "");
        if (n.rfind("monsters\\worm\\worm", 0) == 0) hasWorm = true;
        if (n == "monsters\\golem\\golemw.cl2") hasGolem = true;
        if (n == "monsters\\unrav\\unravw.cl2") hasUnrav = true;
    }
    CHECK(!hasWorm, "ghost worm\\worm entries excluded from monster catalog");
    CHECK(hasGolem, "availability=Never golem stays (not over-filtered)");
    CHECK(hasUnrav, "availability=Never unrav stays (not over-filtered)");
    printf("sprite_tools (catalog fixes): %s\n", allOk ? "PASS" : "FAIL");
}

// cow.cel must decode with width:"auto" (its animWidth is on the TOWN_COW
// rows, not animPath), and a wrong width must be rejected cleanly rather than
// crashing the process with SIGSEGV.
static void testSpriteDecodeCow() {
    std::string mpq = findMonsterMpq();
    if (mpq.empty()) {
        printf("  (skipped cow decode; no MPQ fixture)\n");
        return;
    }
    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerSpriteTools(reg);
    auto call = [&](const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", "sprite.decode"}, {"arguments", args}}}});
        return resp ? *resp : json::object();
    };

    json autoResp = call(json{{"archive", mpq},
                              {"name", "towners\\animals\\cow.cel"},
                              {"width", "auto"},
                              {"palette_name", "gendata\\cut2.pal"}});
    CHECK(!autoResp.contains("error"), "cow width:auto has no JSON-RPC error");
    json dec = autoResp.value("result", json::object());
    CHECK(dec.value("format", std::string()) == "cel", "cow decodes as cel");
    CHECK(dec.value("frame_count", 0) == 96, "cow frame_count == 96");

    json bad = call(json{{"archive", mpq},
                         {"name", "towners\\animals\\cow.cel"},
                         {"width", 64},
                         {"palette_name", "gendata\\cut2.pal"}});
    CHECK(bad.is_object() && (bad.contains("result") || bad.contains("error")),
          "cow with bad width returns a response (no SIGSEGV)");
    printf("sprite_tools (cow width): %s\n", allOk ? "PASS" : "FAIL");
}

// Pick an MPQ that actually holds an items CEL + a palette, so the test is
// independent of the bundled dictionary / cwd. SKIPs cleanly otherwise.
static std::string findItemsMpq() {
    std::vector<std::string> cands;
    if (const char* e = std::getenv("DHFC_TEST_MPQ"))
        cands.push_back(e);
    cands.push_back("/d/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ");
    cands.push_back("/d/work/xt/diablo/DevilutionX-Hell Fire/hellfire.mpq");
    for (auto& c : cands) {
        std::ifstream f(engine::Archive::normalizePath(c));
        if (!f.good())
            continue;
        engine::Archive a;
        if (a.open(c) && a.verify("items\\armor2.cel") &&
            a.verify("gendata\\cut2.pal"))
            return c;
    }
    return "";
}

// Every items CEL must decode with width:"auto" (DevilutionX ItemAnimWidth=96).
// Regression for the directory browser: items\armor2.cel (and all its siblings)
// used to fail with "cel requires a positive integer 'width'" because autoWidth
// returned -1 ("items have no width table").
static void testSpriteDecodeItems() {
    std::string mpq = findItemsMpq();
    if (mpq.empty()) {
        printf("  (skipped items decode; no MPQ with items\\armor2.cel)\n");
        printf("sprite_tools (items decode): %s\n", allOk ? "PASS" : "FAIL");
        return;
    }
    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerSpriteTools(reg);
    auto call = [&](const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", "sprite.decode"}, {"arguments", args}}}});
        return resp ? *resp : json::object();
    };

    json autoResp = call(json{{"archive", mpq},
                              {"name", "items\\armor2.cel"},
                              {"width", "auto"},
                              {"palette_name", "gendata\\cut2.pal"}});
    CHECK(!autoResp.contains("error"), "items armor2 width:auto has no JSON-RPC error");
    json dec = autoResp.value("result", json::object());
    CHECK(dec.value("format", std::string()) == "cel", "items armor2 decodes as cel");
    CHECK(dec.value("frame_count", 0) > 0, "items armor2 frame_count > 0");
    if (dec.contains("frames") && dec["frames"].is_array() && !dec["frames"].empty())
        CHECK(dec["frames"][0].value("width", 0) == 96,
              "items armor2 frame width == 96");
    printf("sprite_tools (items decode): %s\n", allOk ? "PASS" : "FAIL");
}

// data\inv\* must be enumerated as its own "inv" family. No MPQ is needed:
// sprite.list only reads the dictionary. Panel backgrounds carry width 320,
// cursor icons carry width 0 (their widths are per-frame, resolved by
// sprite.decode's built-in table when the frontend sends width:"auto").
static void testSpriteListInvFamily() {
    ToolRegistry reg;
    tools::registerSpriteTools(reg);

    std::string dictPath = "tests/_it_inv_dict.json";
    {
        std::ofstream d(dictPath);
        d << "{ \"archives\": [ { \"name\": \"DIABDAT.MPQ\", "
             "\"path\": \"D:/fixtures/DIABDAT.MPQ\", \"recovered\": [ ";
        d << "{ \"name\": \"data\\\\inv\\\\objcurs.cel\", \"size\": 209584 }, ";
        d << "{ \"name\": \"data\\\\inv\\\\inv.cel\", \"size\": 113708 }, ";
        d << "{ \"name\": \"data\\\\inv\\\\inv_rog.cel\", \"size\": 113708 }, ";
        d << "{ \"name\": \"items\\\\armor2.cel\", \"size\": 100 } ";
        d << "] }, { \"name\": \"hellfire.mpq\", "
             "\"path\": \"D:/fixtures/hellfire.mpq\", \"recovered\": [ ";
        d << "{ \"name\": \"data\\\\inv\\\\objcurs2.cel\", \"size\": 57840 } ";
        d << "] } ] }";
    }
    auto resp = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", json{{"name", "sprite.list"},
                        {"arguments", json{{"dict", dictPath}, {"family", "inv"}}}}}});
    CHECK(resp.has_value(), "inv: sprite.list returns a response");
    json lst = resp ? resp->value("result", json::object()) : json::object();
    json sprites = lst.value("sprites", json::array());
    CHECK(sprites.size() == 4, "inv: sprite.list enumerates 4 data\\inv sprites");
    int nCursors = 0, nPanels = 0;
    for (auto& s : sprites) {
        CHECK(s.value("family", "") == "inv", "inv: family == inv");
        CHECK(s.value("type", "") == "cel", "inv: type == cel");
        CHECK(!s.contains("widths"), "inv: no bulky widths[] on catalog entries");
        const std::string cls = s.value("class", "");
        if (cls == "cursors") {
            ++nCursors;
            CHECK(s.value("width", -1) == 0, "inv: cursor entry width == 0 (auto per-frame)");
        } else if (cls == "panels") {
            ++nPanels;
            CHECK(s.value("width", -1) == 320, "inv: panel entry width == 320");
        }
    }
    CHECK(nCursors == 2, "inv: 2 cursor icon entries");
    CHECK(nPanels == 2, "inv: 2 panel background entries");
    std::remove(dictPath.c_str());
    printf("sprite_tools (inv catalog): %s\n", allOk ? "PASS" : "FAIL");
}

// levels\l<d>data\l<d>.cel / nlevels\... are LEVEL tilesets (not frame sprites).
// They must be enumerated as their own "level" family (class L1..L6, tileset
// l1..l6, width 0) so the frontend routes the click to the Level viewer instead
// of sprite.decode (which would fail: "cel requires a width").
static void testSpriteListLevelFamily() {
    ToolRegistry reg;
    tools::registerSpriteTools(reg);

    std::string dictPath = "tests/_it_level_dict.json";
    {
        std::ofstream d(dictPath);
        d << "{ \"archives\": [ { \"name\": \"DIABDAT.MPQ\", "
             "\"path\": \"D:/fixtures/DIABDAT.MPQ\", \"recovered\": [ ";
        d << "{ \"name\": \"levels\\\\l1data\\\\l1.cel\", \"size\": 100 }, ";
        d << "{ \"name\": \"levels\\\\l2data\\\\l2.cel\", \"size\": 100 }, ";
        d << "{ \"name\": \"levels\\\\l3data\\\\l3.cel\", \"size\": 100 }, ";
        d << "{ \"name\": \"levels\\\\l4data\\\\l4.cel\", \"size\": 100 } ";
        d << "] }, { \"name\": \"hellfire.mpq\", "
             "\"path\": \"D:/fixtures/hellfire.mpq\", \"recovered\": [ ";
        d << "{ \"name\": \"nlevels\\\\l5data\\\\l5.cel\", \"size\": 100 }, ";
        d << "{ \"name\": \"nlevels\\\\l6data\\\\l6.cel\", \"size\": 100 } ";
        d << "] } ] }";
    }
    auto resp = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", json{{"name", "sprite.list"},
                        {"arguments", json{{"dict", dictPath}, {"family", "level"}}}}}});
    CHECK(resp.has_value(), "level: sprite.list returns a response");
    json lst = resp ? resp->value("result", json::object()) : json::object();
    json sprites = lst.value("sprites", json::array());
    CHECK(sprites.size() == 6, "level: sprite.list enumerates 6 level tileset CELs");
    int nL = 0;
    for (auto& s : sprites) {
        CHECK(s.value("family", "") == "level", "level: family == level");
        CHECK(s.value("type", "") == "cel", "level: type == cel");
        CHECK(s.value("width", -1) == 0, "level: width == 0 (not a frame sprite)");
        const std::string cls = s.value("class", "");
        const std::string tileset = s.value("tileset", "");
        CHECK(!tileset.empty(), "level: carries a non-empty tileset id");
        // class == "L" + tileset digit; tileset matches TILESETS ids l1..l6
        bool okTileset = (tileset == "l1" || tileset == "l2" || tileset == "l3" ||
                          tileset == "l4" || tileset == "l5" || tileset == "l6");
        CHECK(okTileset, "level: tileset is a known TILESETS id");
        CHECK(cls == ("L" + tileset.substr(1)), "level: class == L<digit>");
        ++nL;
    }
    CHECK(nL == 6, "level: 6 level family entries classified");
    std::remove(dictPath.c_str());
    printf("sprite_tools (level catalog): %s\n", allOk ? "PASS" : "FAIL");
}

// data\inv\objcurs*.cel inventory ICONS have a different width per frame.
// sprite.decode must decode them via width:"auto" (built-in table) or an
// explicit widths[] array, while a single width is still rejected cleanly.
static void testSpriteDecodeInv() {
    const std::string base = "/d/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ";
    const std::string hf = "/d/work/xt/diablo/DevilutionX-Hell Fire/hellfire.mpq";
    const std::string pal = "gendata\\cut2.pal";
    {
        std::ifstream a(engine::Archive::normalizePath(base));
        std::ifstream b(engine::Archive::normalizePath(hf));
        if (!a.good() || !b.good()) {
            printf("  (skipped inv decode; DIABDAT/hellfire MPQ fixture missing)\n");
            printf("sprite_tools (inv decode): %s\n", allOk ? "PASS" : "FAIL");
            return;
        }
    }

    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerSpriteTools(reg);
    auto call = [&](const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", "sprite.decode"}, {"arguments", args}}}});
        return resp ? *resp : json::object();
    };
    const json stack = json::array({base, hf});

    // 1) width:"auto" resolves the built-in 179-value table.
    json autoResp = call(json{{"archive", base}, {"archives", stack},
                              {"name", "data\\inv\\objcurs.cel"},
                              {"width", "auto"}, {"palette_name", pal}});
    CHECK(!autoResp.contains("error"), "inv: objcurs width:auto has no JSON-RPC error");
    json dec = autoResp.value("result", json::object());
    CHECK(dec.value("format", std::string()) == "cel", "inv: objcurs decodes as cel");
    CHECK(dec.value("frame_count", 0) == 179, "inv: objcurs frame_count == 179");
    if (dec.contains("frames") && dec["frames"].is_array() && !dec["frames"].empty()) {
        CHECK(dec["frames"][0].value("width", 0) == 33,
              "inv: objcurs frame 0 width == 33");
        bool has28 = false, has56 = false;
        for (auto& f : dec["frames"]) {
            const int w = f.value("width", 0);
            if (w == 28) has28 = true;
            if (w == 56) has56 = true;
        }
        CHECK(has28 && has56, "inv: objcurs frames have varying widths (28 and 56)");
        CHECK(!dec["frames"][0].value("rgba_base64", std::string()).empty(),
              "inv: objcurs frame has RGBA");
    }

    // 2) Explicit widths[] array (same table) also decodes.
    json ws = json::array();
    for (uint16_t w : decoders::inv_widths::kObjcursWidths) ws.push_back(w);
    json expl = call(json{{"archive", base}, {"archives", stack},
                          {"name", "data\\inv\\objcurs.cel"},
                          {"widths", ws}, {"palette_name", pal}, {"frame", 0}});
    CHECK(!expl.contains("error"), "inv: objcurs explicit widths[] has no error");
    CHECK(expl.value("result", json::object()).value("frame_count", 0) == 179,
          "inv: objcurs explicit widths[] frame_count == 179");

    // 3) A single uniform width is still rejected (clean error, no crash).
    json bad = call(json{{"archive", base}, {"archives", stack},
                         {"name", "data\\inv\\objcurs.cel"},
                         {"width", 28}, {"palette_name", pal}, {"frame", 0}});
    CHECK(bad.is_object() && (bad.contains("result") || bad.contains("error")),
          "inv: objcurs wrong width returns a response (no crash)");
    CHECK(bad.contains("error"), "inv: objcurs single width 28 is rejected");

    // 4) Hellfire's extra icon sheet (61 frames) resolves by its own name.
    json a2 = call(json{{"archive", hf}, {"archives", stack},
                        {"name", "data\\inv\\objcurs2.cel"},
                        {"width", "auto"}, {"palette_name", pal}, {"frame", 0}});
    CHECK(!a2.contains("error"), "inv: objcurs2 width:auto has no error");
    CHECK(a2.value("result", json::object()).value("frame_count", 0) == 61,
          "inv: objcurs2 frame_count == 61");

    // 5) Panel backgrounds are ordinary single-width CELs (320x352).
    json panel = call(json{{"archive", base}, {"archives", stack},
                           {"name", "data\\inv\\inv.cel"},
                           {"width", 320}, {"palette_name", pal}, {"frame", 0}});
    CHECK(!panel.contains("error"), "inv: inv.cel width 320 has no error");
    json pd = panel.value("result", json::object());
    CHECK(pd.value("frame_count", 0) == 1, "inv: inv.cel frame_count == 1");
    if (pd.contains("frames") && pd["frames"].is_array() && !pd["frames"].empty()) {
        CHECK(pd["frames"][0].value("width", 0) == 320, "inv: inv.cel width == 320");
        CHECK(pd["frames"][0].value("height", 0) == 352, "inv: inv.cel height == 352");
    }
    printf("sprite_tools (inv decode): %s\n", allOk ? "PASS" : "FAIL");
}

// spell.list: stage-ordered spell / monster-attack catalog synthesized from the
// DevilutionX data tables. Pure logic (no MPQ); SKIPs if the tables are absent.
static void testSpellListSynthesis() {
    ToolRegistry reg;
    tools::registerSpriteTools(reg);

    auto raw = [&](const std::string& tool, const json& args) -> json {
        auto resp = handleRequest(reg, json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", args}}}});
        CHECK(resp.has_value(), "spell.list: tools/call returns a response");
        return resp ? *resp : json::object();
    };
    auto resultOf = [&](const std::string& tool, const json& args) -> json {
        return raw(tool, args).value("result", json::object());
    };

    auto listing = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    bool registered = false;
    if (listing) {
        json arr = listing->value("result", json::object()).value("tools", json::array());
        for (auto& t : arr)
            if (t.value("name", "") == "spell.list") registered = true;
    }
    CHECK(registered, "tools/list exposes spell.list");

    json r = raw("spell.list", json::object());
    if (r.contains("error")) {
        printf("  (skipped spell.list synthesis; DevilutionX data tables not available)\n");
        return;
    }
    json res = r.value("result", json::object());
    CHECK(res.value("count", 0) >= 36, "spell.list has >= 36 player spells");
    CHECK(res.value("monster_attack_count", 0) > 0, "spell.list has monster attacks");
    json spells = res.value("spells", json::array());
    json attacks = res.value("monster_attacks", json::array());
    json impacts = res.value("impact_catalog", json::array());
    CHECK(!impacts.empty(), "spell.list has impact_catalog");

    auto findSpell = [&](const std::string& name) -> json {
        for (auto& s : spells)
            if (s.value("name", "") == name || s.value("id", "") == name) return s;
        return json::object();
    };
    auto stageOf = [](const json& e, const std::string& id) -> json {
        for (auto& s : e.value("stages", json::array()))
            if (s.value("id", "") == id) return s;
        return json::object();
    };

    // Fireball: 4 stages, 16 numbered files, explicit width, impact override.
    json fb = findSpell("Fireball");
    CHECK(!fb.empty(), "spell.list includes Fireball");
    if (!fb.empty()) {
        CHECK(fb.value("cast_action", std::string()) == "fm", "Fireball cast_action == fm");
        CHECK(fb.value("element", std::string()) == "Fire", "Fireball element == Fire");
        CHECK(fb.value("stages", json::array()).size() == 4, "Fireball has 4 stages");
        CHECK(stageOf(fb, "cast").value("layers", json::array()).size() == 1,
              "Fireball cast has 1 layer");
        json pl = stageOf(fb, "projectile").value("layers", json::array());
        CHECK(pl.size() == 1, "Fireball projectile has 1 layer");
        if (!pl.empty()) {
            json files = pl[0].value("files", json::array());
            CHECK(files.size() == 16, "Fireball has 16 numbered files");
            if (!files.empty())
                CHECK(files[0] == "missiles\\fireba1.cl2", "Fireball first file name");
            CHECK(pl[0].value("width", 0) == 96, "Fireball explicit width == 96");
            CHECK(pl[0].value("frames", 0) == 14, "Fireball frames == 14");
        }
        json il = stageOf(fb, "impact").value("layers", json::array());
        CHECK(!il.empty() && il[0].value("graphic_id", std::string()) == "MagmaBallExplosion",
              "Fireball impact -> MagmaBallExplosion");
    }
    // Chinese name + synthesized description (resolved from Translations/zh_TW.po).
    CHECK(!fb.value("name_zh", std::string()).empty(), "Fireball name_zh present");
    CHECK(fb.value("description", std::string()).find("系法术") != std::string::npos,
          "Fireball zh description synthesized");
    // A clean spell name from the .po (validates the po loader end-to-end).
    json fbt = findSpell("Firebolt");
    CHECK(!fbt.empty() && fbt.value("name_zh", std::string()) == "火栓",
          "Firebolt zh name == 火栓 (from Translations)");

    // Spell-level parameters (from spelldat.tsv) + per-level mana curve.
    CHECK(fb.value("level", std::string()) == "8", "Fireball level (sBookLvl) == 8");
    CHECK(fb.value("mana_multiplier", std::string()) == "1", "Fireball mana_multiplier == 1");
    CHECK(fb.value("min_mana", std::string()) == "10", "Fireball min_mana == 10");
    json mbl = fb.value("mana_by_level", json::array());
    CHECK(mbl.size() == 8, "Fireball mana_by_level has 8 levels (1..bookLevel)");
    if (mbl.size() == 8) {
        // cost = 16 - (lvl-1)*1, floored at minMana=10
        CHECK(mbl[0].value("level", 0) == 1 && mbl[0].value("mana", 0) == 16,
              "Fireball L1 mana == 16");
        CHECK(mbl[7].value("level", 0) == 8 && mbl[7].value("mana", 0) == 10,
              "Fireball L8 mana floored to min_mana == 10");
    }

    // Fire Wall: 2 numbered files, width 128.
    json fw = findSpell("Fire Wall");
    CHECK(!fw.empty(), "spell.list includes Fire Wall");
    if (!fw.empty()) {
        json pl = stageOf(fw, "projectile").value("layers", json::array());
        CHECK(!pl.empty() && pl[0].value("files", json::array()).size() == 2,
              "Fire Wall has 2 numbered files");
        if (!pl.empty())
            CHECK(pl[0].value("width", 0) == 128, "Fire Wall width == 128");
    }

    // visual_by_level: Fire Wall L3 duration_ticks == 640 (10*(3+1)*16)
    if (!fw.empty()) {
        json vbl = fw.value("visual_by_level", json::array());
        CHECK(!vbl.empty(), "Fire Wall has visual_by_level");
        bool ok640 = false;
        for (auto& e : vbl)
            if (e.value("level", 0) == 3 &&
                e.value("params", json::object()).value("duration_ticks", "") == "640")
                ok640 = true;
        CHECK(ok640, "Fire Wall L3 duration_ticks == 640");
    }
    // Identify (utility, no level scaling) -> empty visual_by_level
    json idn = findSpell("Identify");
    CHECK(!idn.empty() && idn.value("visual_by_level", json::array()).empty(),
          "Identify has empty visual_by_level");
    // Chain Lightning -> radius_tiles present (crawl radius min(lvl+3,Max))
    json cl = findSpell("Chain Lightning");
    if (!cl.empty()) {
        bool hasR = false;
        for (auto& e : cl.value("visual_by_level", json::array()))
            if (!e.value("params", json::object()).value("radius_tiles", "").empty()) hasR = true;
        CHECK(hasR, "Chain Lightning exposes radius_tiles");
    }
    // Torchant (monster attack) -> visual_by_level present (monster-level indexed)
    json torV = findSpell("Torchant");
    if (!torV.empty())
        CHECK(!torV.value("visual_by_level", json::array()).empty(),
              "Torchant monster attack has visual_by_level");

    // Stone Curse: TRN debuff applied to the monster.
    json sc = findSpell("Stone Curse");
    CHECK(!sc.empty(), "spell.list includes Stone Curse");
    if (!sc.empty()) {
        bool trn = false;
        for (auto& l : stageOf(sc, "debuff").value("layers", json::array())) {
            if (l.value("kind", "") == "trn") {
                json t = l.value("trn", json::object());
                if (t.value("preset", "") == "stone" && t.value("apply_to", "") == "monster")
                    trn = true;
            }
        }
        CHECK(trn, "Stone Curse has stone TRN debuff on monster");
    }

    // Flash: comma-separated missiles -> 2 projectile layers.
    json fl = findSpell("Flash");
    CHECK(!fl.empty(), "spell.list includes Flash");
    if (!fl.empty()) {
        CHECK(fl.value("missiles", json::array()).size() == 2, "Flash has 2 missiles");
        CHECK(stageOf(fl, "projectile").value("layers", json::array()).size() == 2,
              "Flash has 2 projectile layers");
    }

    // Monster ranged attacks (real GetMissileType switch).
    json tor;
    for (auto& a : attacks)
        if (a.value("monster_ai", "") == "Torchant") tor = a;
    CHECK(!tor.empty(), "spell.list includes Torchant monster attack");
    if (!tor.empty()) {
        bool hasFireball = false;
        for (auto& m : tor.value("missiles", json::array()))
            if (m == "Fireball") hasFireball = true;
        CHECK(hasFireball, "Torchant -> Fireball missile");
        CHECK(!tor.value("monsters", json::array()).empty(), "Torchant has monster names");
    }
    bool hasDefault = false;
    for (auto& a : attacks)
        if (a.value("default_ai", false)) hasDefault = true;
    CHECK(hasDefault, "spell.list marks default AI attacks");

    // Multi-form monster attacks: AIs that pick the missile inside their AI
    // function (bypassing GetMissileType) are split into one entry per form.
    auto findAttack = [&](const std::string& id) -> json {
        for (auto& a : attacks)
            if (a.value("id", std::string()) == id) return a;
        return json::object();
    };
    json adv = findAttack("Counselor.Fireball");
    CHECK(!adv.empty(), "spell.list includes Counselor.Fireball");
    if (!adv.empty()) {
        CHECK(adv.value("monster_ai", std::string()) == "Counselor",
              "Counselor.Fireball monster_ai == Counselor");
        CHECK(adv.value("variant", std::string()) == "Fireball",
              "Counselor.Fireball variant == Fireball");
        json ms = adv.value("missiles", json::array());
        CHECK(ms.size() == 1 && ms[0] == "Fireball",
              "Counselor.Fireball -> Fireball missile");
        bool advMon = false, counselorMon = false;
        for (auto& m : adv.value("monsters", json::array())) {
            if (m == "Advocate") advMon = true;
            if (m == "Counselor") counselorMon = true;
        }
        CHECK(advMon && !counselorMon,
              "Counselor.Fireball only lists intelligence-3 monsters");
        CHECK(adv.value("cast_action", std::string()) == "fm",
              "Counselor.Fireball cast_action == fm");
        CHECK(!adv.value("condition", std::string()).empty(),
              "Counselor.Fireball carries a condition note");
        CHECK(stageOf(adv, "projectile").value("layers", json::array()).size() == 1,
              "Counselor.Fireball has 1 projectile layer");
    }
    json cflash = findAttack("Counselor.Flash");
    CHECK(!cflash.empty(), "spell.list includes Counselor.Flash");
    if (!cflash.empty())
        CHECK(stageOf(cflash, "projectile").value("layers", json::array()).size() == 2,
              "Counselor.Flash has FlashBottom+FlashTop layers");
    json batl = findAttack("Bat.Lightning");
    CHECK(!batl.empty(), "spell.list includes Bat.Lightning");
    if (!batl.empty()) {
        json bm = batl.value("monsters", json::array());
        CHECK(bm.size() == 1 && bm[0] == "Familiar",
              "Bat.Lightning only lists MT_FAMILIAR (Familiar)");
    }
    json mega = findAttack("Mega.Inferno");
    CHECK(!mega.empty(), "spell.list includes Mega.Inferno");
    if (!mega.empty()) {
        json mm = mega.value("missiles", json::array());
        CHECK(!mm.empty() && mm[0] == "InfernoControl",
              "Mega.Inferno -> InfernoControl");
    }
    json hork = findAttack("HorkDemon.HorkSpawn");
    CHECK(!hork.empty(), "spell.list includes HorkDemon.HorkSpawn (unique AI)");
    if (!hork.empty()) {
        json hm = hork.value("monsters", json::array());
        CHECK(hm.size() == 1 && hm[0] == "Hork Demon",
              "HorkSpawn attributed to the Hork Demon unique");
    }
    CHECK(res.value("monster_attack_count", 0) >= 23,
          "spell.list monster_attack_count includes the extra attack forms");

    // Engine-level monster abilities (teleport / acid puddle / invisibility).
    json abilities = res.value("abilities", json::array());
    CHECK(res.value("ability_count", 0) == 3 && abilities.size() == 3,
          "spell.list exposes the 3 monster abilities");
    auto findAbility = [&](const std::string& id) -> json {
        for (auto& a : abilities)
            if (a.value("id", std::string()) == id) return a;
        return json::object();
    };
    json blink = findAbility("Blink");
    CHECK(!blink.empty(), "spell.list includes Blink ability");
    if (!blink.empty()) {
        CHECK(blink.value("ability", false), "Blink marked as an ability");
        CHECK(!blink.value("trigger", std::string()).empty(), "Blink carries a trigger");
        CHECK(!blink.value("source_ref", std::string()).empty(), "Blink carries source_ref");
        bool hasBlink = false;
        for (auto& m : blink.value("monsters", json::array()))
            if (m == "Blink") hasBlink = true;
        CHECK(hasBlink, "Blink ability lists MT_BLINK");
    }
    json acidAb = findAbility("AcidDeathPuddle");
    CHECK(!acidAb.empty(), "spell.list includes AcidDeathPuddle ability");
    if (!acidAb.empty()) {
        json pl = stageOf(acidAb, "projectile").value("layers", json::array());
        CHECK(!pl.empty() && pl[0].value("graphic_id", std::string()) == "AcidPuddle",
              "Acid puddle uses the AcidPuddle sprite");
        bool lavaMaw = false;
        for (auto& m : acidAb.value("monsters", json::array()))
            if (m == "Lava Maw") lavaMaw = true;
        CHECK(lavaMaw, "Acid puddle ability lists Lava Maw");
    }
    json hiddenAb = findAbility("Hidden");
    CHECK(!hiddenAb.empty(), "spell.list includes Hidden ability");
    if (!hiddenAb.empty()) {
        json hm = hiddenAb.value("monsters", json::array());
        bool hasHidden = false, hasIllusion = false;
        for (auto& m : hm) {
            if (m == "Hidden") hasHidden = true;
            if (m == "Illusion Weaver") hasIllusion = true;
        }
        CHECK(hm.size() >= 4 && hasHidden && hasIllusion,
              "Hidden ability lists the Sneak monsters (incl. uniques)");
    }

    // Trap spell effects (wall traps / trapped chests / Hellfire flame traps).
    json traps = res.value("traps", json::array());
    CHECK(res.value("trap_count", 0) == 10 && traps.size() == 10,
          "spell.list exposes the 10 trap spell effects");
    auto findTrap = [&](const std::string& id) -> json {
        for (auto& t : traps)
            if (t.value("id", std::string()) == id) return t;
        return json::object();
    };
    json ta = findTrap("Trap.Arrow");
    CHECK(!ta.empty(), "spell.list includes Trap.Arrow");
    if (!ta.empty()) {
        CHECK(ta.value("trap", false), "Trap.Arrow marked as a trap");
        json pl = stageOf(ta, "projectile").value("layers", json::array());
        CHECK(!pl.empty() && pl[0].value("graphic_id", std::string()) == "Arrow",
              "Trap.Arrow uses the Arrow sprite");
        CHECK(ta.value("objects", json::array()).size() == 2,
              "Trap.Arrow lists both wall trap objects");
        CHECK(!ta.value("trigger", std::string()).empty(), "Trap.Arrow carries a trigger");
        CHECK(!ta.value("source_ref", std::string()).empty(), "Trap.Arrow carries source_ref");
    }
    json tn = findTrap("TrapChest.Nova");
    CHECK(!tn.empty(), "spell.list includes TrapChest.Nova");
    if (!tn.empty()) {
        json pl = stageOf(tn, "projectile").value("layers", json::array());
        CHECK(!pl.empty() && pl[0].value("graphic_id", std::string()) == "Lightning",
              "TrapChest.Nova uses the Lightning sprite");
    }
    json trf = findTrap("TrapChest.RingOfFire");
    CHECK(!trf.empty(), "spell.list includes TrapChest.RingOfFire (Hellfire)");
    if (!trf.empty()) {
        json pl = stageOf(trf, "projectile").value("layers", json::array());
        CHECK(!pl.empty() && pl[0].value("graphic_id", std::string()) == "FireWall" &&
                  pl[0].value("files", json::array()).size() == 2,
              "TrapChest.RingOfFire uses the 2-file FireWall sprite");
    }
    json tsm = findTrap("TrapChest.StealMana");
    CHECK(!tsm.empty(), "spell.list includes TrapChest.StealMana (Hellfire)");
    if (!tsm.empty())
        CHECK(stageOf(tsm, "projectile").value("layers", json::array()).empty(),
              "StealMana has no sprite (invisible missile)");
    json tf = findTrap("Trap.Flame");
    CHECK(!tf.empty(), "spell.list includes Trap.Flame");
    if (!tf.empty()) {
        CHECK(stageOf(tf, "projectile").value("layers", json::array()).empty(),
              "flame trap has no missile sprite (object animation)");
        CHECK(tf.value("objects", json::array()).size() == 2,
              "flame trap lists flame hole + lever objects");
    }
    json trapOnly = resultOf("spell.list", json{ { "source", "trap" } });
    CHECK(trapOnly.value("count", 0) == 10, "spell.list source=trap returns only traps");

    // source / id filters.
    json one = resultOf("spell.list", json{{"source", "player"}, {"id", "Fireball"}});
    CHECK(one.value("count", 0) == 1, "spell.list id/source filter works");

    printf("sprite_tools (spell.list): %s\n", allOk ? "PASS" : "FAIL");
}

// Level map render against a real Diablo 1 / Hellfire MPQ (L1 cathedral/town).
// Exercises the full level pipeline (TIL/MIN/DUN/SOL + level CEL reencode +
// isometric composite) end to end, mirroring the map.build tool. SKIPs cleanly
// if the fixture lacks the L1 level assets.
static void testLevelMap(const std::string& mpq) {
    // The default fixture is the Hellfire bundle (devilutionx.mpq); L1 dungeons
    // live in the base DIABDAT.MPQ beside it, so probe both.
    std::vector<std::string> archives = { mpq };
    {
        size_t i = mpq.find_last_of("/\\");
        std::string dir = (i != std::string::npos) ? mpq.substr(0, i + 1) : "";
        std::string base = dir + "DIABDAT.MPQ";
        if (base != mpq) archives.push_back(base);
    }
    auto bs = [](const std::string& s) {
        std::string r = s;
        for (char& c : r) if (c == '/') c = static_cast<char>(92);
        return r;
    };

    std::string cel = "levels/l1data/l1.cel", til = "levels/l1data/l1.til",
                min = "levels/l1data/l1.min", sol = "levels/l1data/l1.sol",
                pal = "levels/l1data/l1_2.pal";
    std::vector<std::string> duns = {"levels/l1data/rnd2.dun",
                                     "levels/l1data/skngdo.dun",
                                     "levels/l1data/l1.dun"};

    std::string usedMpq, dun;
    std::vector<uint8_t> celB, tilB, minB, solB, dunB, palB;
    for (auto& arch : archives) {
        engine::Archive arc;
        if (!arc.open(arch)) continue;
        if (!arc.extract(bs(cel), celB)) continue;
        if (!arc.extract(bs(til), tilB)) continue;
        if (!arc.extract(bs(min), minB)) continue;
        if (!arc.extract(bs(sol), solB)) continue;
        if (!arc.extract(bs(pal), palB)) continue;
        for (auto& dn : duns) {
            std::vector<uint8_t> t;
            if (arc.extract(bs(dn), t) && !t.empty()) { dun = dn; dunB = t; break; }
        }
        if (!dun.empty()) { usedMpq = arch; break; }
    }
    if (dun.empty()) {
        printf("  (skipped level.map; no L1 dungeon in any fixture)\n");
        printf("level.map: %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

    auto megatiles = decoders::loadTil(tilB);
    auto micros = decoders::loadMin(minB, 10, 10);
    auto d = decoders::loadDun(dunB);
    auto solFlags = decoders::loadSol(solB);
    auto palRgb = decoders::loadPalette(palB);
    CHECK(!megatiles.empty(), "level.map TIL parsed");
    CHECK(!micros.empty(), "level.map MIN parsed");
    CHECK(d.width > 0 && d.height > 0, "level.map DUN parsed");
    CHECK(!palRgb.empty(), "level.map palette parsed");

    int gw = 0, gh = 0;
    std::vector<uint16_t> grid;
    decoders::buildMicroGrid(d, megatiles, grid, gw, gh);
    CHECK(gw > 0 && gh > 0, "level.map micro grid built");

    auto ft = decoders::buildFrameTypes(micros, 10);
    auto re = decoders::reencodeLevelCel(celB, ft);
    CHECK(re.numFrames > 0, "level.map CEL reencoded");

    std::vector<uint8_t> palRgba = decoders::paletteToRGBA(palRgb);
    decoders::MapRenderOptions opts;
    opts.prefillMicro = 0;
    auto map = decoders::renderMap(re, micros, grid, gw, gh, solFlags,
                                  d.transparency, palRgba.data(), 10, opts);
    CHECK(map.width > 0 && map.height > 0, "level.map rendered with positive size");
    CHECK(map.rgba.size() == (size_t)map.width * map.height * 4,
          "level.map rgba sized width*height*4");
    size_t nonT = 0, transparent = 0;
    for (size_t i = 0; i < map.rgba.size() / 4; ++i) {
        if (map.rgba[i * 4 + 3] != 0) nonT++;
        else transparent++;
    }
    CHECK(nonT > 0, "level.map produced visible (opaque) pixels");
    CHECK(transparent == 0, "level.map opaque background: 0 transparent pixels");
    CHECK(map.solid.size() == (size_t)gw * gh, "level.map solid grid sized gw*gh");
    printf("level.map (%s via %s): %s\n", dun.c_str(),
           usedMpq.c_str(), allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// scene.* tools: complete-scene assembly against a real MPQ (Tristram town).
// ---------------------------------------------------------------------------
static void testSceneTools(const std::string& mpq) {
    // Town assets live in DIABDAT.MPQ beside the default devilutionx.mpq.
    std::vector<std::string> archives = { mpq };
    {
        size_t i = mpq.find_last_of("/\\");
        std::string dir = (i != std::string::npos) ? mpq.substr(0, i + 1) : "";
        std::string base = dir + "DIABDAT.MPQ";
        if (base != mpq) archives.push_back(base);
    }
    // Verify the town assets exist before attempting a costly render.
    bool haveTown = false;
    for (auto& arch : archives) {
        engine::Archive a;
        if (!a.open(arch)) continue;
        if (a.verify("levels/towndata/town.cel") &&
            a.verify("levels/towndata/town.til") &&
            a.verify("levels/towndata/town.min") &&
            a.verify("levels/towndata/town.pal") &&
            a.verify("levels/towndata/sector1s.dun")) {
            haveTown = true;
            break;
        }
    }
    if (!haveTown) {
        printf("  (skipped scene.*; town assets not found in any fixture)\n");
        printf("scene.*: %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

    ToolRegistry reg;
    tools::registerSceneTools(reg);
    CHECK(reg.has("scene.list") && reg.has("scene.build") && reg.has("scene.path"),
          "scene.list / scene.build / scene.path registered");

    ToolResult lr = reg.dispatch("scene.list", json::object());
    CHECK(lr.ok, "scene.list dispatch ok");
    json scenes = lr.value.value("scenes", json::array());
    bool hasTristram = false, hasL2 = false, hasL4 = false;
    for (auto& s : scenes) {
        const std::string id = s.value("id", "");
        if (id == "tristram") hasTristram = true;
        if (id == "l2_blind") hasL2 = true;
        if (id == "l4_diab1") hasL4 = true;
    }
    CHECK(hasTristram, "scene.list contains tristram");
    CHECK(hasL2 && hasL4, "scene.list contains L2/L4 presets");

    json args = json{{"id", "tristram"}, {"archive", archives[0]},
                     {"preview_max_width", 400}};
    json stack = json::array();
    for (auto& a : archives) stack.push_back(a);
    args["archives"] = stack;
    ToolResult br = reg.dispatch("scene.build", args);
    CHECK(br.ok, "scene.build dispatch ok");
    if (!br.ok) {
        printf("    scene.build error: %s\n", br.value.dump().c_str());
        printf("scene.*: %s\n", allOk ? "PASS" : "FAIL");
        return;
    }
    const json& v = br.value;
    CHECK(v.value("width", 0) > 0 && v.value("height", 0) > 0,
          "scene.build positive native dims");
    CHECK(v.value("grid_width", 0) == 96 && v.value("grid_height", 0) == 96,
          "scene.build tristram grid 96x96");
    CHECK(v.value("transparent_pixel_count", 1) == 0,
          "scene.build 0 transparent pixels (no black blocks)");
    CHECK(v.value("frame_count", 0) > 0, "scene.build reencoded CEL frames");
    // Preview + overlay PNGs are valid base64 PNGs of IDENTICAL dims.
    const std::string img = v.value("image_base64", std::string());
    const std::string ovl = v.value("overlay_base64", std::string());
    CHECK(img.size() > 100 && ovl.size() > 100,
          "scene.build returns both base64 PNGs");
    CHECK(v.value("preview_width", 0) > 0 && v.value("preview_height", 0) > 0,
          "scene.build preview dims positive");
    const int pw = v.value("preview_width", 0), ph = v.value("preview_height", 0);
    CHECK(pw * ph * 4 > 0, "scene.build preview has pixels");
    // Flag arrays sized gw*gh.
    CHECK(v["meta"]["solid"].size() == (size_t)(96 * 96),
          "scene.build solid array sized gw*gh");
    CHECK(v["meta"]["transparent"].size() == (size_t)(96 * 96),
          "scene.build transparent array sized gw*gh");
    CHECK(v["meta"]["walkable"].size() == (size_t)(96 * 96),
          "scene.build walkable array sized gw*gh");
    CHECK(v.value("walkable_count", 0) > 0,
          "scene.build walkable_count positive");
    CHECK(v.value("special_count", 0) > 0,
          "scene.build emits dSpecial town overlays (trees/arches)");
    CHECK(v["meta"]["special"].size() == (size_t)(96 * 96),
          "scene.build special array sized gw*gh");
    // Decode the base64 and confirm the PNG signature.
    auto pngSigOk = [](const std::string& b64) {
        auto bytes = base64::decode(b64);
        return bytes.size() > 8 && bytes[0] == 137 && bytes[1] == 80 &&
               bytes[2] == 78 && bytes[3] == 71;
    };
    CHECK(pngSigOk(img), "scene.build image_base64 is a PNG");
    CHECK(pngSigOk(ovl), "scene.build overlay_base64 is a PNG");

    // scene.path over the town's SOL grid (centre -> nearby tile).
    json pargs = json{{"id", "tristram"}, {"archive", archives[0]},
                      {"archives", stack},
                      {"from", json{{"x", 48}, {"y", 48}}},
                      {"to", json{{"x", 50}, {"y", 52}}},
                      {"preview_max_width", 400}};
    ToolResult pr = reg.dispatch("scene.path", pargs);
    CHECK(pr.ok, "scene.path dispatch ok");
    if (pr.ok) {
        CHECK(pr.value.value("found", false), "scene.path finds a town path");
        CHECK(pr.value["path"].size() >= 2, "scene.path returns start..destination");
        CHECK(pr.value["path"][0].value("x", -1) == 48 &&
                  pr.value["path"][0].value("y", -1) == 48,
              "scene.path starts at `from`");
        const auto& last = pr.value["path"].back();
        CHECK(last.value("x", -1) == 50 && last.value("y", -1) == 52,
              "scene.path ends at `to`");
        CHECK(pr.value["path"][0].contains("screen_x") &&
                  pr.value["path"][0].contains("preview_x"),
              "scene.path projects native + preview coordinates");
    }

    // Faithful dLight shading on a dungeon preset.
    json largs = json{{"id", "l1_rnd2"}, {"archive", archives[0]},
                      {"archives", stack}, {"preview_max_width", 200},
                      {"lighting", true}};
    ToolResult lbr = reg.dispatch("scene.build", largs);
    CHECK(lbr.ok, "scene.build lighting dispatch ok");
    if (lbr.ok) {
        const json& lv = lbr.value;
        CHECK(lv.value("lighting", false), "scene.build reports lighting enabled");
        CHECK(lv.value("ambient", -1) == 15, "L1 ambient is 15 (dark)");
        CHECK(lv.value("light_lit_count", 0) >= 1,
              "L1 central torch fully lights at least one cell");
        CHECK(lv.value("transparent_pixel_count", 1) == 0,
              "lit dungeon still has 0 transparent pixels");
        const int lgw = lv.value("grid_width", 0), lgh = lv.value("grid_height", 0);
        CHECK(lv["meta"]["light"].size() == (size_t)(lgw * lgh),
              "scene.build light array sized gw*gh");
    }

    // L4 uses 12 microLen / 16 blocks and the hell light style.
    json l4args = json{{"id", "l4_diab1"}, {"archive", archives[0]},
                       {"archives", stack}, {"preview_max_width", 200}};
    ToolResult l4r = reg.dispatch("scene.build", l4args);
    CHECK(l4r.ok, "scene.build L4 dispatch ok");
    if (l4r.ok) {
        CHECK(l4r.value.value("grid_width", 0) == 44 &&
                  l4r.value.value("grid_height", 0) == 44,
              "L4 diab1 grid 44x44");
        CHECK(l4r.value.value("micro_len", 0) == 12, "L4 microLen is 12");
        CHECK(l4r.value.value("transparent_pixel_count", 1) == 0,
              "L4 has 0 transparent pixels");
        CHECK(l4r.value.value("frame_count", 0) > 0, "L4 reencoded CEL frames");
    }
    printf("scene.* (tristram + lighting + L4 + path): %s\n",
           allOk ? "PASS" : "FAIL");
}

static void testLevelGenerate(const std::string& mpq) {
    std::vector<std::string> archives = { mpq };
    {
        size_t i = mpq.find_last_of("/\\");
        std::string dir = (i != std::string::npos) ? mpq.substr(0, i + 1) : "";
        std::string base = dir + "DIABDAT.MPQ";
        if (base != mpq) archives.push_back(base);
    }

    std::string base;
    for (const auto& a : archives) {
        engine::Archive arc;
        if (!arc.open(a)) continue;
        std::vector<uint8_t> tmp;
        if (arc.extract("levels\\l1data\\l1.cel", tmp) || arc.extract("levels/l1data/l1.cel", tmp)) {
            base = a;
            break;
        }
    }
    if (base.empty()) {
        printf("  (skipped level.generate; no DIABDAT.MPQ with L1 assets)\n");
        printf("level.generate: %s\n", allOk ? "PASS" : "FAIL");
        return;
    }

    const char* outDir = "tests/gen_test";
    ToolRegistry reg;
    tools::registerMpqTools(reg);
    tools::registerLevelTools(reg);
    auto call = [&](const std::string& tool, const json& args) -> json {
        ToolResult r = reg.dispatch(tool, args);
        if (!r.ok)
            throw std::runtime_error("tool failed: " + r.value.dump());
        return r.value;
    };

    json first = call("level.generate",
        json{{"archive", base}, {"level", 2}, {"seed", 1383137027}, {"out", outDir}});
    CHECK(first.value("width", 0) == 40, "level.generate width 40");
    CHECK(first.value("height", 0) == 40, "level.generate height 40");
    CHECK(first.value("stairs_up_count", 0) > 0, "level.generate placed stairs up");
    CHECK(first.contains("tiles") && first["tiles"].size() == 40, "level.generate returns 40 hex rows");
    CHECK(first["tiles"][0].get<std::string>().size() == 80, "level.generate hex row width 80");

    const std::string dunPath = first.value("dun_path", std::string());
    CHECK(!dunPath.empty(), "level.generate wrote a dun path");

    std::ifstream f(dunPath, std::ios::binary);
    std::vector<uint8_t> dun { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
    CHECK(dun.size() == 54404, "level.generate DUN is ExportDun-sized (54404 bytes)");
    auto d = decoders::loadDun(dun);
    CHECK(d.width == 40 && d.height == 40, "level.generate DUN parses as 40x40");
    CHECK(d.tileLayer.size() == 1600, "level.generate DUN has 1600 megatiles");
    CHECK(d.hasTransparency, "level.generate DUN transparency layer detected");

    // Determinism through the public tool surface.
    json second = call("level.generate",
        json{{"archive", base}, {"level", 2}, {"seed", 1383137027}, {"out", outDir}});
    CHECK(first["tiles"] == second["tiles"], "level.generate deterministic for same seed");

    // Level 1 has no quests; "none" must also work without set-piece extraction.
    json l1 = call("level.generate",
        json{{"archive", base}, {"level", 1}, {"seed", 2588}, {"quests", "none"}, {"out", outDir}});
    CHECK(l1.value("level", 0) == 1, "level.generate level 1 ok");
    CHECK(l1.value("floor_count", 0) > 0, "level.generate level 1 has floor tiles");

    printf("level.generate (%s): %s\n", base.c_str(), allOk ? "PASS" : "FAIL");
}

static void testLevelInspect(const std::string& mpq) {
    std::vector<std::string> archives = { mpq };
    {
        size_t i = mpq.find_last_of("/\\");
        std::string dir = (i != std::string::npos) ? mpq.substr(0, i + 1) : "";
        std::string base = dir + "DIABDAT.MPQ";
        if (base != mpq) archives.push_back(base);
    }
    bool haveTown = false;
    for (auto& arch : archives) {
        engine::Archive a;
        if (!a.open(arch)) continue;
        if (a.verify("levels/towndata/town.cel") &&
            a.verify("levels/towndata/town.til") &&
            a.verify("levels/towndata/town.min") &&
            a.verify("levels/towndata/town.pal")) {
            haveTown = true;
            break;
        }
    }
    if (!haveTown) {
        printf("  (skipped level.inspect; town assets not found)\n");
        return;
    }

    ToolRegistry reg;
    tools::registerLevelTools(reg);
    CHECK(reg.has("level.inspect"), "level.inspect registered");

    json stack = json::array();
    for (auto& a : archives) stack.push_back(a);
    json args = json{
        {"archive", archives[0]}, {"archives", stack},
        {"cel", "levels/towndata/town.cel"},
        {"til", "levels/towndata/town.til"},
        {"min", "levels/towndata/town.min"},
        {"sol", "levels/towndata/town.sol"},
        {"palette", "levels/towndata/town.pal"},
        {"blocks", 16}, {"micro_len", 16}, {"megatile", 71},
    };
    ToolResult r = reg.dispatch("level.inspect", args);
    CHECK(r.ok, "level.inspect dispatch ok");
    if (!r.ok) {
        printf("    level.inspect error: %s\n", r.value.dump().c_str());
        return;
    }
    const json& v = r.value;
    CHECK(v.value("megatile", 0) == 71, "level.inspect megatile 71");
    CHECK(v["micros"].size() == 4, "level.inspect returns 4 micro pieces");
    const int expected[4] = { 257, 258, 259, 260 };
    for (int i = 0; i < 4 && i < static_cast<int>(v["micros"].size()); ++i)
        CHECK(v["micros"][i].value("id", -1) == expected[i],
              "level.inspect micro id 257..260 in order");
    bool hasFrames = true;
    for (int f = 594; f <= 605; ++f) {
        bool found = false;
        for (const auto& fr : v["frames"])
            if (fr.value("frame", 0) == f) found = true;
        if (!found) hasFrames = false;
    }
    CHECK(hasFrames, "level.inspect decodes town.cel frames 594..605");
    const std::string asmB64 = v["assembly"].value("png_base64", std::string());
    auto asmPng = base64::decode(asmB64);
    CHECK(asmPng.size() > 8 && asmPng[0] == 137 && asmPng[1] == 80 &&
              asmPng[2] == 78 && asmPng[3] == 71,
          "level.inspect assembly is a PNG");
    CHECK(v["assembly"].value("width", 0) > 0 && v["assembly"].value("height", 0) > 0,
          "level.inspect assembly has visible bounds");
    printf("level.inspect (megatile 71): %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// stage 5: ui.* tools — layout catalog + real-MPQ art compositing.
// ---------------------------------------------------------------------------
static void testUiTools(const std::string& mpq) {
    // UI art spans DIABDAT.MPQ (original .pcx/.cel + town.pal) and
    // devilutionx.mpq (ui_art\diablo.pal + .clx). Build the same archive stack
    // the scene tests use, then verify the two key assets before rendering.
    std::vector<std::string> archives = { mpq };
    {
        size_t i = mpq.find_last_of("/\\");
        std::string dir = (i != std::string::npos) ? mpq.substr(0, i + 1) : "";
        for (const char* extra : {"DIABDAT.MPQ", "devilutionx.mpq"}) {
            std::string p = dir + extra;
            if (p != mpq && std::find(archives.begin(), archives.end(), p) == archives.end())
                archives.push_back(p);
        }
    }
    bool haveUi = false;
    for (auto& arch : archives) {
        engine::Archive a;
        if (!a.open(arch)) continue;
        if (a.verify("ui_art\\mainmenu.pcx") && a.verify("levels\\towndata\\town.pal")) {
            haveUi = true; break;
        }
    }
    if (!haveUi) {
        printf("  (skipped ui.*; UI art not found in any fixture)\n");
        printf("ui.*: %s\n", allOk ? "PASS" : "FAIL");
        return;
    }
    json stack = json::array();
    for (auto& a : archives) stack.push_back(a);

    ToolRegistry reg;
    tools::registerUiTools(reg);
    CHECK(reg.has("ui.list") && reg.has("ui.layout") && reg.has("ui.build"),
          "ui.list / ui.layout / ui.build registered");

    auto pngSigOk = [](const std::string& b64) {
        auto bytes = base64::decode(b64);
        return bytes.size() > 8 && bytes[0] == 137 && bytes[1] == 80 &&
               bytes[2] == 78 && bytes[3] == 71;
    };

    // ui.list (no MPQ needed, but exercised through the same registry).
    ToolResult lr = reg.dispatch("ui.list", json::object());
    CHECK(lr.ok, "ui.list dispatch ok");
    if (lr.ok) {
        CHECK(lr.value.value("count", 0) >= 18, "ui.list count >= 18");
        CHECK(lr.value["canvas_default"].value("width", 0) == 640 &&
              lr.value["canvas_default"].value("height", 0) == 480,
              "ui.list default canvas 640x480");
        CHECK(lr.value["categories"].is_array() && !lr.value["categories"].empty(),
              "ui.list categories array");
    }
    ToolResult lh = reg.dispatch("ui.list", json{{"category", "hud"}});
    CHECK(lh.ok, "ui.list{hud} dispatch ok");
    if (lh.ok) {
        CHECK(lh.value.value("count", 0) == 1, "ui.list{hud} returns only control_panel");
        if (lh.value["screens"].size() == 1)
            CHECK(lh.value["screens"][0].value("id", "") == "control_panel",
                  "ui.list{hud} id is control_panel");
    }

    // ui.layout: full structured layout, bounds, art refs, needs_width.
    ToolResult lay = reg.dispatch("ui.layout", json{{"id", "control_panel"}});
    CHECK(lay.ok, "ui.layout dispatch ok");
    if (lay.ok) {
        const json& v = lay.value;
        CHECK(v["elements"].is_array() && v["elements"].size() >= 10,
              "ui.layout control_panel has >=10 elements");
        CHECK(v["panels"].is_array() && !v["panels"].empty(), "ui.layout panels array");
        CHECK(v["art_refs"].is_array() && !v["art_refs"].empty(), "ui.layout art_refs array");
        CHECK(v["needs_width"].is_array(), "ui.layout needs_width array");
        CHECK(v.value("overlay_box_count", 0) > 0, "ui.layout overlay_box_count > 0");
        bool allInBounds = true;
        for (const auto& e : v["elements"]) {
            const auto r = e["rect"];
            int x = r[0].get<int>(), y = r[1].get<int>(), w = r[2].get<int>(), h = r[3].get<int>();
            if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= 640 || y >= 480) allInBounds = false;
        }
        CHECK(allInBounds, "ui.layout every element rect anchored in 640x480");
    }
    ToolResult bad = reg.dispatch("ui.layout", json{{"id", "bogus_screen"}});
    CHECK(!bad.ok, "ui.layout{bogus} returns error (dispatch catches throw)");

    // ui.build(mainmenu): PCX embedded palette, 0 transparent pixels, valid PNG.
    json bargs = json{{"id", "mainmenu"}, {"archive", archives[0]},
                      {"archives", stack}, {"out", "tests/ui"}};
    ToolResult br = reg.dispatch("ui.build", bargs);
    CHECK(br.ok, "ui.build(mainmenu) dispatch ok");
    if (!br.ok) {
        printf("    ui.build error: %s\n", br.value.dump().c_str());
        printf("ui.*: %s\n", allOk ? "PASS" : "FAIL");
        return;
    }
    const json& mm = br.value;
    CHECK(mm.value("width", 0) == 640 && mm.value("height", 0) == 480,
          "ui.build(mainmenu) 640x480");
    CHECK(pngSigOk(mm.value("image_base64", std::string())),
          "ui.build(mainmenu) image_base64 is a PNG");
    CHECK(pngSigOk(mm.value("overlay_base64", std::string())),
          "ui.build(mainmenu) overlay_base64 is a PNG");
    CHECK(mm.value("transparent_pixel_count", 1) == 0 &&
          mm.value("background_opaque", false),
          "ui.build(mainmenu) opaque background / 0 transparent pixels");
    CHECK(mm.value("overlay_box_count", 0) == mm.value("element_count", -1),
          "ui.build overlay_box_count == element_count (no panels in mainmenu)");
    bool mmArtOk = false;
    for (const auto& a : mm["art"])
        if (a.value("name", "") == "ui_art\\mainmenu.pcx" &&
            a.value("status", "") == "ok" && a.value("width", 0) == 640 &&
            a.value("height", 0) == 480) mmArtOk = true;
    CHECK(mmArtOk, "ui.build(mainmenu) mainmenu.pcx decoded 640x480");
    bool statusValid = true;
    for (const auto& e : mm["elements"]) {
        const std::string s = e.value("decode_status", "");
        if (s != "ok" && s != "text" && s != "missing" &&
            s != "width_unknown" && s != "decode_error") statusValid = false;
    }
    CHECK(statusValid, "ui.build every element has a valid decode_status");
    {
        std::ifstream f("tests/ui/mainmenu.png");
        CHECK(f.good(), "ui.build writes tests/ui/mainmenu.png");
    }
    {
        std::ifstream f("tests/ui/mainmenu.json");
        CHECK(f.good(), "ui.build writes tests/ui/mainmenu.json sidecar");
    }

    // ui.build(control_panel): cross-archive palette (town.pal in DIABDAT,
    // diablo.pal in devilutionx.mpq) + CEL width from catalog + 2 orb frames.
    json cargs = json{{"id", "control_panel"}, {"archive", archives[0]},
                      {"archives", stack}, {"out", "tests/ui"}};
    ToolResult cr = reg.dispatch("ui.build", cargs);
    CHECK(cr.ok, "ui.build(control_panel) dispatch ok");
    if (cr.ok) {
        const json& v = cr.value;
        CHECK(pngSigOk(v.value("image_base64", std::string())),
              "ui.build(control_panel) image PNG");
        CHECK(v.value("transparent_pixel_count", 1) == 0,
              "ui.build(control_panel) 0 transparent pixels");
        // palette bank: town.pal must be ok and come from a real archive
        CHECK(v["palettes"].is_array() && !v["palettes"].empty(),
              "ui.build(control_panel) palette bank non-empty");
        bool townOk = false, panel8Ok = false;
        int bulbsOk = 0;
        for (const auto& p : v["palettes"])
            if (p.value("name", "") == "levels\\towndata\\town.pal" &&
                p.value("status", "") == "ok") townOk = true;
        for (const auto& a : v["art"]) {
            if (a.value("name", "") == "ctrlpan\\panel8.cel" &&
                a.value("status", "") == "ok" && a.value("width", 0) == 640) panel8Ok = true;
            if (a.value("name", "") == "ctrlpan\\p8bulbs.cel" &&
                a.value("status", "") == "ok" && a.value("width", 0) == 88 &&
                a.value("height", 0) == 88) ++bulbsOk;
        }
        CHECK(townOk, "ui.build(control_panel) town.pal resolved (cross-archive)");
        CHECK(panel8Ok, "ui.build(control_panel) panel8.cel width=640 from catalog");
        CHECK(bulbsOk >= 1, "ui.build(control_panel) p8bulbs.cel decoded 88x88");
        // spelicon.cel's width was recovered (56 px, SPLICONLENGTH), so the
        // spell icon now decodes cleanly instead of being skipped as
        // width_unknown. Its palette is the cross-archive town.pal above.
        bool speliconDecoded = false;
        for (const auto& e : v["elements"])
            if (e.value("id", "") == "cp.spell_icon" &&
                e.value("decode_status", "") == "ok") speliconDecoded = true;
        CHECK(speliconDecoded, "ui.build spell icon CEL decodes (width recovered)");
    }

    // Missing-palette fallback: give only a stack WITHOUT the palette files.
    // We cannot easily remove diablo.pal, but we CAN pass an explicit wrong
    // palette name -> all CEL/CLX decode_error, tool still ok (skip_missing).
    json wargs = json{{"id", "control_panel"}, {"archive", archives[0]},
                      {"archives", stack}, {"out", "tests/ui"},
                      {"palette", "__no_such_palette__.pal"}};
    ToolResult wr = reg.dispatch("ui.build", wargs);
    CHECK(wr.ok, "ui.build with missing palette still ok (skip_missing_art)");
    if (wr.ok)
        CHECK(wr.value.value("art_ok_count", 0) == 0,
              "ui.build missing palette -> no art decoded, no crash");

    printf("ui.*: %s\n", allOk ? "PASS" : "FAIL");
}

int main() {
    printf("=== DiabloHFConverter integration tests (engine + mpq_tools + sprite_tools + scene) ===\n");
    testNormalize();
    testSpriteList();
    testSpriteListSynthesis();
    testSpriteListInvFamily();
    testSpriteListLevelFamily();

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
    testSpriteDecodeTrn(mpq);
    testSpriteDecodeMonster();
    testSpriteDecodeCrossArchive();
    testSpriteDecodeRejectsTruncatedCl2();
    testSpriteListCatalogFixes();
    testSpriteDecodeCow();
    testSpriteDecodeItems();
    testSpriteDecodeInv();
    testSpellListSynthesis();
    testLevelMap(mpq);
    testSceneTools(mpq);
    testLevelGenerate(mpq);
    testLevelInspect(mpq);
    testUiTools(mpq);
    printf("=== %s ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
    return allOk ? 0 : 1;
}
