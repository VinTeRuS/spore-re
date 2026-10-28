#pragma once

#include "spore/dbpf.h"
#include <vector>
#include <memory>
#include <filesystem>

namespace Spore {

class VirtualFileSystem {
public:
    VirtualFileSystem();
    ~VirtualFileSystem();

    // Mounts an individual package. Later mounts take precedence over earlier ones.
    bool MountPackage(const std::filesystem::path& packagePath);

    // Scans a directory for *.package files and mounts them in standard Spore priority:
    // Core -> Graphics -> Audio -> Content -> Packs -> Patches.
    size_t MountDataDirectory(const std::filesystem::path& dataDir);

    // Checks whether the specified resource exists in any mounted package.
    bool HasResource(const ResourceKey& key) const;

    // Reads and decompresses a resource from the highest-priority mounted package containing it.
    bool ReadResource(const ResourceKey& key, std::vector<uint8_t>& out) const;

    // Finds all unique resource keys of a given type across all mounted packages.
    std::vector<ResourceKey> FindAllByType(uint32_t type) const;

    // Resolves a resource by its Spore name (lowercased FNV-1 hash of the name).
    // Pass type = 0 to match any resource type. Highest-priority package wins.
    bool FindByName(uint32_t type, const std::string& name, ResourceKey& outKey) const;

    // Convenience wrapper: resolve by name and read+decompress the resource.
    bool ReadByName(uint32_t type, const std::string& name, std::vector<uint8_t>& out) const;

    // Returns every resource whose name hash matches (any type), highest priority first.
    std::vector<ResourceKey> FindAllByName(const std::string& name) const;

    // Returns total count of mounted packages.
    size_t GetMountedPackageCount() const { return mArchives.size(); }

private:
    void EnsureNameIndex() const;
    std::vector<std::unique_ptr<DBPFArchive>> mArchives;
    mutable std::unordered_map<uint64_t, std::vector<ResourceKey>> mByNameIndex;
    mutable bool mNameIndexBuilt = false;
};

} // namespace Spore
