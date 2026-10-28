#pragma once

#include <cstdint>
#include <string_view>

namespace Spore {

// Spore resource instance ids are the lowercased name hashed with FNV-1 (32-bit
// multiply-then-xor). Verified against the shipped property database, e.g.
//   HashName("amoeba_cell")  == 0xdae8ce17
//   HashName("genericmeat_01") == 0xbfc7022d
// See docs/clean-room/specs/02-rw4-and-naming.md for the evidence.
inline uint32_t HashName(std::string_view name) {
    uint32_t h = 0x811c9dc5u;              // FNV offset basis
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
        h = (h * 0x01000193u) ^ c;         // FNV-1: multiply, then XOR
    }
    return h;
}

} // namespace Spore
