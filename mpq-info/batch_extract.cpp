// mpq-info/batch_extract.cpp
//
// Batch-extract every filename verified by recover_phase0 (recorded in
// mpq-info/dict/recovered.json) from its owning MPQ into
// out/extracted/<archive>/<in-archive-path>.
//
// The recovered.json already pairs each recovered name with the exact MPQ path,
// so this is a straight "open archive -> extractToFile per verified name" loop.
// Because every name in recovered.json was previously validated by StormLib's
// hash lookup, the extraction should succeed for all of them; any failure is
// reported and counted.
//
// Build (mirrors recover_phase0.cpp):
//   g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 \
//       mpq-info/batch_extract.cpp src/engine/archive.cpp \
//       3rdparty/StormLib/build/libStormLib.a \
//       -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
//       -o out/batch_extract.exe
//
// Run:  out/batch_extract.exe [recovered.json] [out_root]
//   defaults: mpq-info/dict/recovered.json  and  out/extracted

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>

#include "engine/archive.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

// Strip a trailing ".mpq"/".MPQ" extension from an archive base name so the
// output folder is e.g. "DIABDAT" rather than "DIABDAT.MPQ".
static std::string stripMpqExt(const std::string& s) {
    auto dot = s.find_last_of('.');
    auto sep = s.find_last_of("/\\");
    if (dot == std::string::npos) return s;
    if (sep != std::string::npos && dot < sep) return s;  // dot belongs to a dir
    std::string ext = s.substr(dot + 1);
    if (ext == "mpq" || ext == "MPQ") return s.substr(0, dot);
    return s;
}

int main(int argc, char** argv) {
    const std::string jsonPath = (argc > 1) ? argv[1] : "mpq-info/dict/recovered.json";
    const std::string outRoot  = (argc > 2) ? argv[2] : "out/extracted";

    std::ifstream in(jsonPath);
    if (!in) {
        fprintf(stderr, "cannot open %s\n", jsonPath.c_str());
        return 1;
    }
    nlohmann::json doc;
    in >> doc;

    long total = 0, ok = 0, fail = 0, openFail = 0;

    for (auto& arch : doc.at("archives")) {
        const std::string name = arch.value("name", "");
        const std::string path = arch.value("path", "");
        if (path.empty()) continue;

        engine::Archive a;
        if (!a.open(path)) {
            fprintf(stderr, "[SKIP] cannot open archive %s (%s)\n",
                    name.c_str(), path.c_str());
            openFail++;
            continue;
        }

        const fs::path archDir = fs::path(outRoot) / stripMpqExt(name);
        fs::create_directories(archDir);

        long archOk = 0, archFail = 0;
        for (auto& e : arch.at("recovered")) {
            const std::string fname = e.value("name", "");
            if (fname.empty()) continue;
            const fs::path outFile = archDir / fname;
            fs::create_directories(outFile.parent_path());
            total++;
            if (a.extractToFile(fname, outFile.string())) {
                ok++;
                archOk++;
            } else {
                fail++;
                archFail++;
                fprintf(stderr, "[FAIL] %s :: %s\n", name.c_str(), fname.c_str());
            }
        }
        printf("%-14s %7ld ok / %ld fail\n", name.c_str(), archOk, archFail);
    }

    printf("\nTOTAL: %ld attempted, %ld extracted OK, %ld failed, %ld archives unopened\n",
           total, ok, fail, openFail);
    return (fail == 0 && openFail == 0) ? 0 : 2;
}
