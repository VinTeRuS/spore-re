#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace Spore {

struct ResourceKey {
    uint32_t type = 0;
    uint32_t group = 0;
    uint64_t instance = 0;

    constexpr bool operator==(const ResourceKey& other) const = default;
};

// Common Spore Resource Types
namespace ResourceTypes {
    constexpr uint32_t Prop     = 0x00b1b104; // Property list (.prop)
    constexpr uint32_t Texture  = 0x2f4e681b; // RenderWare 4 Texture
    constexpr uint32_t Model    = 0x2f7d0004; // RenderWare 4 Model / Mesh
    constexpr uint32_t Spui     = 0x01c135da; // Spore UI Layout
    constexpr uint32_t Effect   = 0x250fe9a2; // VFX / Shaders
    constexpr uint32_t Audio    = 0x02393756; // Audio stream / bank
}

} // namespace Spore

template <>
struct std::hash<Spore::ResourceKey> {
    std::size_t operator()(const Spore::ResourceKey& k) const noexcept {
        std::size_t h1 = std::hash<uint32_t>{}(k.type);
        std::size_t h2 = std::hash<uint32_t>{}(k.group);
        std::size_t h3 = std::hash<uint64_t>{}(k.instance);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};
