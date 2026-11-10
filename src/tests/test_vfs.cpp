#include <cstdlib>
#include "spore/vfs.h"
#include "spore/refpack.h"
#include "spore/hashing.h"
#include <iostream>
#include <cassert>
#include <cstring>

void TestNameHashing() {
    // Resource instance ids are the lowercased name hashed with FNV-1 (32-bit).
    // Values cross-checked against the shipped property database.
    assert(Spore::HashName("amoeba_cell") == 0xdae8ce17u);
    assert(Spore::HashName("Amoeba_Cell") == 0xdae8ce17u);
    assert(Spore::HashName("genericmeat_01") == 0xbfc7022du);
    std::cout << "[PASS] Name hashing (FNV-1 32-bit, lowercased).\n";
}

void TestRefPackInvalid() {
    std::vector<uint8_t> out;
    uint8_t garbage[] = { 0x00, 0x01, 0x02 };
    bool res = Spore::DecompressRefPack(garbage, out);
    assert(!res);
    std::cout << "[PASS] RefPack invalid stream check.\n";
}

void TestRealSporeData() {
    const char* env = std::getenv("SPORE_DATA_DIR");
    if (!env) {
        std::cout << "[SKIP] set SPORE_DATA_DIR to the game's Data folder\n";
        return;
    }
    const std::filesystem::path sporeDataDir = env;
    if (!std::filesystem::exists(sporeDataDir)) {
        std::cout << "[SKIP] Spore Data directory not found at " << sporeDataDir << "\n";
        return;
    }

    Spore::VirtualFileSystem vfs;
    size_t mounted = vfs.MountDataDirectory(sporeDataDir);
    std::cout << "[INFO] Mounted " << mounted << " packages from " << sporeDataDir << "\n";
    assert(mounted > 0);

    // 1. Check textures (RenderWare 4 Texture)
    auto textures = vfs.FindAllByType(Spore::ResourceTypes::Texture);
    std::cout << "[INFO] Found " << textures.size() << " RenderWare 4 textures.\n";
    assert(!textures.empty());

    // Read first texture and check magic "\x89RW4w32"
    std::vector<uint8_t> texData;
    bool readTex = vfs.ReadResource(textures[0], texData);
    assert(readTex);
    assert(texData.size() >= 8);

    const uint8_t expectedRw4Magic[] = { 0x89, 'R', 'W', '4', 'w', '3', '2', 0x00 };
    bool rw4Match = (std::memcmp(texData.data(), expectedRw4Magic, sizeof(expectedRw4Magic)) == 0);
    std::cout << "[INFO] Texture 0 size: " << texData.size() << " bytes, RW4 magic match: " << (rw4Match ? "YES" : "NO") << "\n";
    assert(rw4Match);

    // 2. Check UI layouts (SPUI)
    auto spui = vfs.FindAllByType(Spore::ResourceTypes::Spui);
    std::cout << "[INFO] Found " << spui.size() << " SPUI layouts.\n";
    assert(!spui.empty());

    std::vector<uint8_t> spuiData;
    bool readSpui = vfs.ReadResource(spui[0], spuiData);
    assert(readSpui);
    std::cout << "[INFO] SPUI layout 0 size: " << spuiData.size() << " bytes.\n";

    // 3. Check property files (PROP)
    auto props = vfs.FindAllByType(Spore::ResourceTypes::Prop);
    std::cout << "[INFO] Found " << props.size() << " Property files.\n";
    assert(!props.empty());

    std::vector<uint8_t> propData;
    bool readProp = vfs.ReadResource(props[0], propData);
    assert(readProp);
    std::cout << "[INFO] Property file 0 size: " << propData.size() << " bytes.\n";

    // 4. Resolve original assets by name (FNV-1 hash of the lowercased name).
    Spore::ResourceKey amoeba;
    bool foundByName = vfs.FindByName(Spore::ResourceTypes::Texture, "amoeba_cell", amoeba);
    assert(foundByName);
    assert(static_cast<uint32_t>(amoeba.instance) == 0xdae8ce17u);
    std::vector<uint8_t> amoebaData;
    bool readByName = vfs.ReadByName(Spore::ResourceTypes::Texture, "amoeba_cell", amoebaData);
    assert(readByName);
    assert(amoebaData.size() >= 8);
    assert(std::memcmp(amoebaData.data(), expectedRw4Magic, sizeof(expectedRw4Magic)) == 0);
    std::cout << "[INFO] Resolved original texture 'amoeba_cell' by name, "
              << amoebaData.size() << " bytes.\n";

    std::cout << "[PASS] All real Spore package VFS tests passed successfully!\n";
}

int main() {
    std::cout << "=== Spore Native VFS & RefPack Tests ===\n";
    TestRefPackInvalid();
    TestNameHashing();
    TestRealSporeData();
    std::cout << "=== All tests completed successfully! ===\n";
    return 0;
}
