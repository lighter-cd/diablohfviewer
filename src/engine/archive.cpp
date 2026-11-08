// src/engine/archive.cpp
//
// StormLib C-API implementation of the engine::Archive wrapper.

#include "engine/archive.h"

#include <cstdio>
#include <string>
#include <windows.h>

#include "StormLib.h"

namespace engine {

// Normalize a filesystem path so StormLib/Win32 CreateFile accepts it. The
// server receives paths over stdin (JSON), so the MSYS argv POSIX->Windows
// translation that native CLIs enjoy does NOT happen here. We therefore accept
// both "D:\foo\bar", "D:/foo/bar" and the POSIX-ish "/d/foo/bar" (as typed by
// a user in a bash shell) and fold the last form into a proper Windows path.
std::string Archive::normalizePath(const std::string& in) {
    if (in.size() >= 3 && in[0] == '/' && in[2] == '/' &&
        std::isalpha((unsigned char)in[1])) {
        // "/d/foo" -> "D:/foo"
        std::string out;
        out.reserve(in.size() + 1);
        out.push_back((char)std::toupper((unsigned char)in[1]));
        out.push_back(':');
        out += in.substr(2);
        return out;
    }
    return in;
}

std::string Archive::normalizeArchiveName(const std::string& in) {
    std::string out = in;
    for (char& c : out)
        if (c == '/') c = '\\';
    return out;
}

Archive::~Archive() {
    if (h_ != nullptr) {
        SFileCloseArchive((HANDLE)h_);
        h_ = nullptr;
    }
}

bool Archive::open(const std::string& path) {
    if (h_ != nullptr) {
        SFileCloseArchive((HANDLE)h_);
        h_ = nullptr;
    }
    HANDLE h = nullptr;
    std::string norm = normalizePath(path);
    // Flags: 0 = open existing, base provider. StormLib picks the right
    // provider for Diablo-1 v0 MPQs automatically.
    if (!SFileOpenArchive(norm.c_str(), 0, 0, &h)) {
        DWORD le = GetLastError();
        fprintf(stderr, "[engine] SFileOpenArchive FAILED path='%s' GetLastError=%lu\n",
                norm.c_str(), (unsigned long)le);
        return false;
    }
    h_ = h;
    return true;
}

std::vector<FileEntry> Archive::list() const {
    std::vector<FileEntry> entries;
    if (h_ == nullptr)
        return entries;

    SFILE_FIND_DATA fd{};
    HANDLE hFind = SFileFindFirstFile((HANDLE)h_, "*", &fd, nullptr);
    if (hFind == nullptr)
        return entries;   // nothing enumerable (shouldn't happen for v0 MPQs)

    do {
        FileEntry e;
        e.name   = fd.cFileName;
        e.size   = (uint64_t)fd.dwFileSize;
        e.exists = true;
        entries.push_back(std::move(e));
    } while (SFileFindNextFile(hFind, &fd));

    SFileFindClose(hFind);
    return entries;
}

bool Archive::extract(const std::string& name, std::vector<uint8_t>& out) const {
    out.clear();
    if (h_ == nullptr)
        return false;

    HANDLE hf = nullptr;
    std::string norm = normalizeArchiveName(name);
    if (!SFileOpenFileEx((HANDLE)h_, norm.c_str(), 0, &hf))
        return false;

    DWORD sizeHi = 0;
    DWORD sizeLo = SFileGetFileSize(hf, &sizeHi);
    uint64_t total = ((uint64_t)sizeHi << 32) | (uint64_t)sizeLo;

    bool ok = false;
    if (total > 0 && total < (uint64_t)512 * 1024 * 1024) {
        out.resize((size_t)total);
        DWORD read = 0;
        ok = SFileReadFile(hf, out.data(), (DWORD)total, &read, nullptr) != 0;
        if (ok && (uint64_t)read != total)
            out.resize((size_t)read);
        if (!ok)
            out.clear();
    }
    SFileCloseFile(hf);
    return ok;
}

bool Archive::extractToFile(const std::string& name, const std::string& outPath) const {
    if (h_ == nullptr)
        return false;
    // SFileExtractFile decrypts + decompresses and writes the raw bytes.
    std::string norm = normalizeArchiveName(name);
    return SFileExtractFile((HANDLE)h_, norm.c_str(), outPath.c_str(), 0) != 0;
}

bool Archive::verify(const std::string& name) const {
    if (h_ == nullptr)
        return false;
    HANDLE hf = nullptr;
    std::string norm = normalizeArchiveName(name);
    bool ok = SFileOpenFileEx((HANDLE)h_, norm.c_str(), 0, &hf) != 0;
    if (ok)
        SFileCloseFile(hf);
    return ok;
}

bool Archive::querySize(const std::string& name, uint64_t& sizeOut) const {
    sizeOut = 0;
    if (h_ == nullptr)
        return false;
    HANDLE hf = nullptr;
    std::string norm = normalizeArchiveName(name);
    if (!SFileOpenFileEx((HANDLE)h_, norm.c_str(), 0, &hf))
        return false;
    DWORD sizeHi = 0;
    DWORD sizeLo = SFileGetFileSize(hf, &sizeHi);
    sizeOut = ((uint64_t)sizeHi << 32) | (uint64_t)sizeLo;
    SFileCloseFile(hf);
    return true;
}

} // namespace engine
