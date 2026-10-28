#pragma once

#include "spore/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace Spore {

struct DBPFEntry {
    ResourceKey key;
    uint32_t offset = 0;
    uint32_t diskSize = 0;
    uint32_t memSize = 0;
    bool isCompressed = false;
};

class DBPFArchive {
public:
    DBPFArchive();
    ~DBPFArchive();

    DBPFArchive(const DBPFArchive&) = delete;
    DBPFArchive& operator=(const DBPFArchive&) = delete;

    DBPFArchive(DBPFArchive&&) noexcept;
    DBPFArchive& operator=(DBPFArchive&&) noexcept;

    // Opens and parses index table of a .package file.
    bool Open(const std::filesystem::path& packagePath);

    // Checks if key exists.
    bool HasResource(const ResourceKey& key) const;

    // Retrieves entry metadata.
    const DBPFEntry* GetEntry(const ResourceKey& key) const;

    // Reads raw chunk data (as stored on disk).
    bool ReadRaw(const ResourceKey& key, std::vector<uint8_t>& out) const;

    // Reads and decompresses resource (if compressed with RefPack).
    bool ReadResource(const ResourceKey& key, std::vector<uint8_t>& out) const;

    // Get all entries.
    const std::unordered_map<ResourceKey, DBPFEntry>& GetEntries() const { return mEntries; }

    // Path to the package file.
    const std::filesystem::path& GetPath() const { return mPath; }

    // Total count of resources.
    size_t GetEntryCount() const { return mEntries.size(); }

private:
    std::filesystem::path mPath;
    mutable std::ifstream mFile;
    mutable std::mutex mFileMutex;
    std::unordered_map<ResourceKey, DBPFEntry> mEntries;
};

} // namespace Spore
