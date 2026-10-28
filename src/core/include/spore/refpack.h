#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Spore {

// Decompresses EA RefPack/QFS stream.
// Returns true on success, false if the stream was malformed.
bool DecompressRefPack(std::span<const uint8_t> compressed, std::vector<uint8_t>& out);

} // namespace Spore
