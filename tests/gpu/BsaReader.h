// Minimal reader for Skyrim SE archives (BSA version 105), for the offline
// GPU material tools: list files, extract one. LZ4-frame compressed files are
// decoded here (no library); Oblivion / LE archives (104, zlib) are refused.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace BsaReader {

struct Entry {
    std::string path;  // lowercase, backslashes: textures\actors\...\x.dds
    uint64_t offset = 0;
    uint32_t size = 0;
    bool compressed = false;
};

struct Archive {
    std::ifstream file;
    uint32_t flags = 0;
    std::vector<Entry> entries;
    std::string error;
};

inline std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p;
}

template <class T>
bool Get(std::ifstream& f, T& v) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

inline bool Open(Archive& a, const std::string& path) {
    a.file.open(path, std::ios::binary);
    if (!a.file) return a.error = "cannot open " + path, false;
    char magic[4];
    uint32_t version, folderOffset, folderCount, fileCount, folderNames, fileNames;
    uint16_t fileFlags, pad;
    a.file.read(magic, 4);
    if (!Get(a.file, version) || std::memcmp(magic, "BSA\0", 4) != 0) return a.error = "not a BSA", false;
    if (version != 105) return a.error = "BSA version " + std::to_string(version) + " (only SE 105)", false;
    Get(a.file, folderOffset);
    Get(a.file, a.flags);
    Get(a.file, folderCount);
    Get(a.file, fileCount);
    Get(a.file, folderNames);
    Get(a.file, fileNames);
    Get(a.file, fileFlags);
    Get(a.file, pad);
    if (!(a.flags & 0x1) || !(a.flags & 0x2)) return a.error = "archive without folder / file names", false;

    std::vector<uint32_t> counts(folderCount);
    for (uint32_t i = 0; i < folderCount; ++i) {
        uint64_t hash, offset;
        uint32_t count, pad32;
        Get(a.file, hash);
        Get(a.file, count);
        Get(a.file, pad32);
        Get(a.file, offset);
        counts[i] = count;
    }
    const bool compressedDefault = (a.flags & 0x4) != 0;
    std::vector<std::string> folders;
    for (uint32_t i = 0; i < folderCount; ++i) {
        uint8_t len;
        Get(a.file, len);
        std::string name(len, '\0');
        a.file.read(name.data(), len);
        if (!name.empty() && name.back() == '\0') name.pop_back();
        for (uint32_t k = 0; k < counts[i]; ++k) {
            uint64_t hash;
            uint32_t size, offset;
            Get(a.file, hash);
            Get(a.file, size);
            Get(a.file, offset);
            Entry e;
            e.path = Normalize(name) + "\\";
            e.offset = offset;
            e.compressed = compressedDefault != ((size & 0x40000000) != 0);
            e.size = size & 0x3FFFFFFF;
            a.entries.push_back(std::move(e));
        }
    }
    std::vector<char> names(fileNames);
    a.file.read(names.data(), fileNames);
    size_t at = 0;
    for (auto& e : a.entries) {
        const char* n = names.data() + at;
        const size_t len = strnlen(n, names.size() - at);
        e.path += Normalize(std::string(n, len));
        at += len + 1;
    }
    if (!a.file) return a.error = "truncated archive", false;
    return true;
}

// LZ4 block decoder; matches may reach back into earlier blocks (linked
// blocks), so it decodes into the whole output.
inline bool Lz4Block(const uint8_t* ip, const uint8_t* iend, std::vector<uint8_t>& out) {
    while (ip < iend) {
        const uint8_t token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (size_t(iend - ip) < lit) return false;
        out.insert(out.end(), ip, ip + lit);
        ip += lit;
        if (ip >= iend) break;  // last sequence: literals only
        if (iend - ip < 2) return false;
        const size_t offset = ip[0] | (ip[1] << 8);
        ip += 2;
        size_t match = token & 15;
        if (match == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (offset == 0 || offset > out.size()) return false;
        const size_t from = out.size() - offset;
        for (size_t k = 0; k < match; ++k) out.push_back(out[from + k]);
    }
    return true;
}

inline bool Lz4Frame(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint32_t expected) {
    const uint8_t* p = in.data();
    const uint8_t* end = p + in.size();
    auto u32 = [&](const uint8_t* q) { return uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24; };
    if (in.size() < 7 || u32(p) != 0x184D2204) return false;
    const uint8_t flg = p[4];
    p += 6;                       // magic, FLG, BD
    if (flg & 0x08) p += 8;       // content size
    if (flg & 0x01) p += 4;       // dictionary id
    p += 1;                       // header checksum
    out.clear();
    out.reserve(expected);
    while (p + 4 <= end) {
        const uint32_t block = u32(p);
        p += 4;
        if (block == 0) break;    // end mark
        const uint32_t size = block & 0x7FFFFFFF;
        if (size_t(end - p) < size) return false;
        if (block & 0x80000000) {
            out.insert(out.end(), p, p + size);
        } else if (!Lz4Block(p, p + size, out)) {
            return false;
        }
        p += size;
        if (flg & 0x10) p += 4;   // block checksum
    }
    return out.size() == expected;
}

inline bool Extract(Archive& a, const std::string& inner, std::vector<uint8_t>& out) {
    const auto want = Normalize(inner);
    auto it = std::find_if(a.entries.begin(), a.entries.end(), [&](const Entry& e) { return e.path == want; });
    if (it == a.entries.end()) return a.error = "not in the archive: " + want, false;
    a.file.clear();
    a.file.seekg(static_cast<std::streamoff>(it->offset));
    uint32_t size = it->size;
    if (a.flags & 0x100) {  // file names embedded before the data
        uint8_t len;
        Get(a.file, len);
        a.file.seekg(len, std::ios::cur);
        size -= 1 + len;
    }
    if (!it->compressed) {
        out.resize(size);
        a.file.read(reinterpret_cast<char*>(out.data()), size);
        return static_cast<bool>(a.file) || (a.error = "read failed", false);
    }
    uint32_t original;
    Get(a.file, original);
    std::vector<uint8_t> packed(size - 4);
    a.file.read(reinterpret_cast<char*>(packed.data()), packed.size());
    if (!a.file) return a.error = "read failed", false;
    if (!Lz4Frame(packed, out, original)) return a.error = "LZ4 decode failed for " + want, false;
    return true;
}

}  // namespace BsaReader
