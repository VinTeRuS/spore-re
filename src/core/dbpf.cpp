#include "spore/dbpf.h"
#include "spore/refpack.h"
#include <cstring>
#include <iostream>

namespace Spore {

DBPFArchive::DBPFArchive() = default;
DBPFArchive::~DBPFArchive() = default;

DBPFArchive::DBPFArchive(DBPFArchive&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mFileMutex);
    mPath = std::move(other.mPath);
    mFile = std::move(other.mFile);
    mEntries = std::move(other.mEntries);
}

DBPFArchive& DBPFArchive::operator=(DBPFArchive&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mFileMutex, other.mFileMutex);
        mPath = std::move(other.mPath);
        mFile = std::move(other.mFile);
        mEntries = std::move(other.mEntries);
    }
    return *this;
}

bool DBPFArchive::Open(const std::filesystem::path& packagePath) {
    std::lock_guard<std::mutex> lock(mFileMutex);
    mEntries.clear();
    mPath = packagePath;

    mFile.open(packagePath, std::ios::binary);
    if (!mFile.is_open()) {
        return false;
    }

    uint8_t header[96];
    if (!mFile.read(reinterpret_cast<char*>(header), sizeof(header))) {
        mFile.close();
        return false;
    }

    // Verify magic 'DBPF'
    if (std::memcmp(header, "DBPF", 4) != 0) {
        mFile.close();
        return false;
    }

    auto ReadU32 = [](const uint8_t* ptr) -> uint32_t {
        return static_cast<uint32_t>(ptr[0]) |
               (static_cast<uint32_t>(ptr[1]) << 8) |
               (static_cast<uint32_t>(ptr[2]) << 16) |
               (static_cast<uint32_t>(ptr[3]) << 24);
    };

    uint32_t major = ReadU32(header + 0x04);
    uint32_t entryCount = ReadU32(header + 0x24);
    uint32_t indexOffset = ReadU32(header + 0x40);

    if (major != 2 && major != 3) {
        mFile.close();
        return false;
    }

    if (!mFile.seekg(indexOffset, std::ios::beg)) {
        mFile.close();
        return false;
    }

    uint8_t indexTypeBuf[4];
    if (!mFile.read(reinterpret_cast<char*>(indexTypeBuf), 4)) {
        mFile.close();
        return false;
    }
    uint32_t indexType = ReadU32(indexTypeBuf);

    // Standard 8 DBPF fields:
    // 0: ResourceType
    // 1: ResourceGroup
    // 2: InstanceHi
    // 3: InstanceLo
    // 4: ChunkOffset
    // 5: FileSize
    // 6: MemSize
    // 7: Compressed
    uint32_t globalValues[8] = { 0 };
    for (int bit = 0; bit < 8; ++bit) {
        if (indexType & (1 << bit)) {
            uint8_t buf[4];
            if (!mFile.read(reinterpret_cast<char*>(buf), 4)) {
                mFile.close();
                return false;
            }
            globalValues[bit] = ReadU32(buf);
        }
    }

    mEntries.reserve(entryCount);

    for (uint32_t i = 0; i < entryCount; ++i) {
        uint32_t currentValues[8];
        for (int bit = 0; bit < 8; ++bit) {
            if (indexType & (1 << bit)) {
                currentValues[bit] = globalValues[bit];
            } else {
                uint8_t buf[4];
                if (!mFile.read(reinterpret_cast<char*>(buf), 4)) {
                    mEntries.clear();
                    mFile.close();
                    return false;
                }
                currentValues[bit] = ReadU32(buf);
            }
        }

        DBPFEntry entry;
        entry.key.type = currentValues[0];
        entry.key.group = currentValues[1];
        entry.key.instance = (static_cast<uint64_t>(currentValues[2]) << 32) | currentValues[3];
        entry.offset = currentValues[4];
        entry.diskSize = currentValues[5] & 0x7FFFFFFF;
        entry.memSize = currentValues[6];
        entry.isCompressed = (currentValues[7] & 0xFFFF) != 0;

        mEntries[entry.key] = entry;
    }

    return true;
}

bool DBPFArchive::HasResource(const ResourceKey& key) const {
    return mEntries.find(key) != mEntries.end();
}

const DBPFEntry* DBPFArchive::GetEntry(const ResourceKey& key) const {
    auto it = mEntries.find(key);
    if (it != mEntries.end()) {
        return &it->second;
    }
    return nullptr;
}

bool DBPFArchive::ReadRaw(const ResourceKey& key, std::vector<uint8_t>& out) const {
    const DBPFEntry* entry = GetEntry(key);
    if (!entry) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mFileMutex);
    if (!mFile.is_open()) {
        return false;
    }

    if (!mFile.seekg(entry->offset, std::ios::beg)) {
        return false;
    }

    out.resize(entry->diskSize);
    if (!mFile.read(reinterpret_cast<char*>(out.data()), entry->diskSize)) {
        out.clear();
        return false;
    }

    return true;
}

bool DBPFArchive::ReadResource(const ResourceKey& key, std::vector<uint8_t>& out) const {
    const DBPFEntry* entry = GetEntry(key);
    if (!entry) {
        return false;
    }

    std::vector<uint8_t> raw;
    if (!ReadRaw(key, raw)) {
        return false;
    }

    if (entry->isCompressed) {
        return DecompressRefPack(raw, out);
    } else {
        out = std::move(raw);
        return true;
    }
}

} // namespace Spore
