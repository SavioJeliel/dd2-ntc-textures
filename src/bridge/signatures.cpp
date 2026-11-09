#include <windows.h>

#include <cstring>
#include <vector>

#include "signatures.hpp"

namespace dd2sig {
namespace {

struct Pattern {
    std::vector<unsigned char> bytes;
    std::vector<bool> fixed;
};

Pattern parse(const char* text) {
    Pattern pattern;
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?') {
            pattern.bytes.push_back(0);
            pattern.fixed.push_back(false);
            p += p[1] == '?' ? 2 : 1;
            continue;
        }
        auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        pattern.bytes.push_back(static_cast<unsigned char>(hex(p[0]) * 16 + hex(p[1])));
        pattern.fixed.push_back(true);
        p += 2;
    }
    return pattern;
}

}  // namespace

unsigned char* find(const char* text, int offset, unsigned* matches) {
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    unsigned char* begin = nullptr;
    std::size_t size = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 6) == 0) {
            begin = image + section->VirtualAddress;
            size = section->Misc.VirtualSize;
        }
    }
    if (matches) *matches = 0;
    const Pattern pattern = parse(text);
    if (!begin || pattern.bytes.empty() || size < pattern.bytes.size()) return nullptr;

    // Âncora: a maior sequência de bytes fixos, procurada com memchr + memcmp.
    std::size_t anchor = 0, anchor_len = 0;
    for (std::size_t i = 0; i < pattern.bytes.size();) {
        std::size_t j = i;
        while (j < pattern.bytes.size() && pattern.fixed[j]) ++j;
        if (j - i > anchor_len) anchor = i, anchor_len = j - i;
        i = j + 1;
    }
    unsigned count = 0;
    unsigned char* found = nullptr;
    const unsigned char* last = begin + size - pattern.bytes.size();
    for (const unsigned char* p = begin + anchor; p <= last + anchor;) {
        p = static_cast<const unsigned char*>(std::memchr(p, pattern.bytes[anchor], (last + anchor) - p + 1));
        if (!p) break;
        const unsigned char* start = p - anchor;
        if (std::memcmp(p, pattern.bytes.data() + anchor, anchor_len) == 0) {
            bool ok = true;
            for (std::size_t k = 0; k < pattern.bytes.size() && ok; ++k) {
                ok = !pattern.fixed[k] || start[k] == pattern.bytes[k];
            }
            if (ok) {
                ++count;
                found = const_cast<unsigned char*>(start);
            }
        }
        ++p;
    }
    if (matches) *matches = count;
    return count == 1 ? found + offset : nullptr;
}

}  // namespace dd2sig
