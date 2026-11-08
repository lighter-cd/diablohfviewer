// src/engine/archive.h
//
// Thin C++ RAII wrapper around StormLib (the canonical, Diablo-1-capable MPQ
// reader; DevilutionX's mpqfs is a fork of it). This is the single place that
// touches StormLib's C API. All higher layers (tools, decoders, MCP) go through
// this interface and only ever see raw bytes + metadata.
//
// StormLib does the hard parts for us: hash-table lookup, per-file decryption
// key (HashString(name,3)+offset), PKWARE implode (0x200) and Blizzard
// multi-compression (0x100) with per-sector type bytes, and sector-offset
// tables. We deliberately do NOT re-implement any of that.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "StormLib.h"   // HANDLE, SFile* APIs used by the move-assignment impl

namespace engine {

struct FileEntry {
    std::string name;   // real name, or "FileNNNNNNN.xxx" placeholder when the
                        // archive has no (listfile)
    uint64_t    size   = 0;
    bool        exists = false;
};

class Archive {
public:
    Archive() = default;
    ~Archive();

    // Normalize a filesystem path for Win32 (folds the POSIX-ish "/d/foo" a
    // shell user might type into "D:/foo"; passes Windows paths through
    // unchanged). Used so the server accepts paths arriving over stdin JSON,
    // where MSYS argv translation does not apply.
    static std::string normalizePath(const std::string& in);

    // Normalize an IN-ARCHIVE name for StormLib: MPQ internal paths are
    // backslash-separated ("levels\\l1data\\l1.cel"), but callers often pass
    // forward slashes ("levels/l1data/l1.cel"). SFileOpenFileEx only resolves
    // the backslash form, so fold '/' -> '\\' before every lookup. Applied
    // inside extract/verify/querySize/extractToFile; names already using '\\'
    // pass through unchanged.
    static std::string normalizeArchiveName(const std::string& in);

    // Non-copyable (owns an HANDLE); movable.
    Archive(const Archive&)            = delete;
    Archive& operator=(const Archive&) = delete;
    Archive(Archive&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Archive& operator=(Archive&& o) noexcept {
        if (this != &o) {
            if (h_ != nullptr) SFileCloseArchive((HANDLE)h_);
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }

    // Open an MPQ. Returns false (and leaves the object closed) on failure.
    bool open(const std::string& path);

    bool isOpen() const { return h_ != nullptr; }

    // Enumerate files. If the archive has no (listfile), StormLib synthesizes
    // "FileNNNNNNN.xxx" placeholder names from the block/hash table, so this
    // always returns the full file population.
    std::vector<FileEntry> list() const;

    // Extract a named file into `out` as raw decrypted+decompressed bytes.
    // Returns false if the name does not resolve or the read fails.
    bool extract(const std::string& name, std::vector<uint8_t>& out) const;

    // Get the uncompressed size of a named file without reading its body.
    // Returns false if the name does not resolve.
    bool querySize(const std::string& name, uint64_t& sizeOut) const;

    // True if `name` resolves to a real file in the archive (hash lookup).
    bool verify(const std::string& name) const;

    // Extract a named file and write it to `outPath` on disk (convenience;
    // uses StormLib's direct file extraction, which is the most robust path).
    bool extractToFile(const std::string& name, const std::string& outPath) const;

private:
    void* h_ = nullptr;   // StormLib HANDLE (void* to avoid pulling windows.h
                          // semantics into every translation unit)
};

} // namespace engine
