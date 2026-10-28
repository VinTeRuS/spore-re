#include "spore/refpack.h"
#include <cstddef>

namespace Spore {

bool DecompressRefPack(std::span<const uint8_t> compressed, std::vector<uint8_t>& out) {
    if (compressed.size() < 5) {
        return false;
    }

    uint8_t b0 = compressed[0];
    uint8_t b1 = compressed[1];
    
    // Check signature: 0x10fb or similar flag combinations
    if (b1 != 0xFB) {
        return false;
    }

    bool is_large = (b0 & 0x80) != 0;
    bool has_extra = (b0 & 0x01) != 0;
    size_t pos = 2;

    uint32_t decomp_size = 0;
    if (is_large) {
        if (pos + 4 > compressed.size()) return false;
        decomp_size = (static_cast<uint32_t>(compressed[pos]) << 24) |
                      (static_cast<uint32_t>(compressed[pos + 1]) << 16) |
                      (static_cast<uint32_t>(compressed[pos + 2]) << 8) |
                      static_cast<uint32_t>(compressed[pos + 3]);
        pos += 4;
    } else {
        if (pos + 3 > compressed.size()) return false;
        decomp_size = (static_cast<uint32_t>(compressed[pos]) << 16) |
                      (static_cast<uint32_t>(compressed[pos + 1]) << 8) |
                      static_cast<uint32_t>(compressed[pos + 2]);
        pos += 3;
    }

    if (has_extra) {
        pos += (is_large ? 4 : 3);
        if (pos > compressed.size()) return false;
    }

    out.clear();
    out.reserve(decomp_size);

    while (pos < compressed.size() && out.size() < decomp_size) {
        uint8_t prefix = compressed[pos++];

        // 111M MMMM (0xE0..0xFF)
        if ((prefix & 0xE0) == 0xE0) {
            uint8_t m = prefix & 0x1F;
            if (m <= 0x1B) {
                size_t ids = 4 * (m + 1);
                if (pos + ids > compressed.size()) return false;
                out.insert(out.end(), compressed.begin() + pos, compressed.begin() + pos + ids);
                pos += ids;
            } else {
                size_t ids = m - 0x1C;
                if (pos + ids > compressed.size()) return false;
                out.insert(out.end(), compressed.begin() + pos, compressed.begin() + pos + ids);
                pos += ids;
                break; // Stop flag
            }
        }
        // 0TTC CCMM (0x00..0x7F)
        else if ((prefix & 0x80) == 0) {
            if (pos >= compressed.size()) return false;
            uint8_t b2 = compressed[pos++];
            size_t ids = prefix & 3;
            size_t copy_len = ((prefix >> 2) & 7) + 3;
            size_t offset = (((prefix >> 5) & 3) << 8) + b2 + 1;

            if (ids > 0) {
                if (pos + ids > compressed.size()) return false;
                out.insert(out.end(), compressed.begin() + pos, compressed.begin() + pos + ids);
                pos += ids;
            }

            if (offset > out.size()) return false; // Match before start of buffer
            for (size_t i = 0; i < copy_len; ++i) {
                out.push_back(out[out.size() - offset]);
            }
        }
        // 10CC CCCC (0x80..0xBF)
        else if ((prefix & 0xC0) == 0x80) {
            if (pos + 1 >= compressed.size()) return false;
            uint8_t b2 = compressed[pos++];
            uint8_t b3 = compressed[pos++];
            size_t ids = (b2 >> 6) & 3;
            size_t copy_len = (prefix & 0x3F) + 4;
            size_t offset = ((b2 & 0x3F) << 8) + b3 + 1;

            if (ids > 0) {
                if (pos + ids > compressed.size()) return false;
                out.insert(out.end(), compressed.begin() + pos, compressed.begin() + pos + ids);
                pos += ids;
            }

            if (offset > out.size()) return false;
            for (size_t i = 0; i < copy_len; ++i) {
                out.push_back(out[out.size() - offset]);
            }
        }
        // 110R CCMM (0xC0..0xDF)
        else if ((prefix & 0xE0) == 0xC0) {
            if (pos + 2 >= compressed.size()) return false;
            uint8_t b2 = compressed[pos++];
            uint8_t b3 = compressed[pos++];
            uint8_t b4 = compressed[pos++];
            size_t ids = prefix & 3;
            size_t copy_len = (((prefix >> 2) & 3) << 8) + b4 + 5;
            size_t offset = (((prefix >> 4) & 1) << 16) + (static_cast<size_t>(b2) << 8) + b3 + 1;

            if (ids > 0) {
                if (pos + ids > compressed.size()) return false;
                out.insert(out.end(), compressed.begin() + pos, compressed.begin() + pos + ids);
                pos += ids;
            }

            if (offset > out.size()) return false;
            for (size_t i = 0; i < copy_len; ++i) {
                out.push_back(out[out.size() - offset]);
            }
        }
    }

    return true;
}

} // namespace Spore
