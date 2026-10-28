#include "spore/vfs.h"
#include "spore/hashing.h"
#include <algorithm>
#include <unordered_set>

namespace Spore {

VirtualFileSystem::VirtualFileSystem() = default;
VirtualFileSystem::~VirtualFileSystem() = default;

bool VirtualFileSystem::MountPackage(const std::filesystem::path& packagePath) {
    auto archive = std::make_unique<DBPFArchive>();
    if (!archive->Open(packagePath)) {
        return false;
    }
    // Prepend or append? If we search in reverse order, newest additions take precedence.
    mArchives.push_back(std::move(archive));
    mNameIndexBuilt = false;
    return true;
}

size_t VirtualFileSystem::MountDataDirectory(const std::filesystem::path& dataDir) {
    if (!std::filesystem::exists(dataDir) || !std::filesystem::is_directory(dataDir)) {
        return 0;
    }

    std::vector<std::filesystem::path> packageFiles;
    for (const auto& entry : std::filesystem::directory_iterator(dataDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".package") {
            packageFiles.push_back(entry.path());
        }
    }

    // Sort to give patches and expansions higher priority
    // Priority: Spore_Audio*, Spore_Content, Spore_Graphics, Spore_Game, Spore_Pack_*, PatchData*
    auto GetPriority = [](const std::filesystem::path& p) -> int {
        std::string filename = p.filename().string();
        if (filename.find("PatchData") != std::string::npos) return 100;
        if (filename.find("Spore_Pack") != std::string::npos) return 80;
        if (filename.find("Spore_Game") != std::string::npos) return 50;
        if (filename.find("Spore_Graphics") != std::string::npos) return 40;
        if (filename.find("Spore_Content") != std::string::npos) return 30;
        if (filename.find("Spore_Audio") != std::string::npos) return 20;
        return 10;
    };

    std::sort(packageFiles.begin(), packageFiles.end(), [&](const auto& a, const auto& b) {
        return GetPriority(a) < GetPriority(b);
    });

    size_t mounted = 0;
    for (const auto& p : packageFiles) {
        if (MountPackage(p)) {
            mounted++;
        }
    }

    return mounted;
}

bool VirtualFileSystem::HasResource(const ResourceKey& key) const {
    // Search in reverse order (highest priority first)
    for (auto it = mArchives.rbegin(); it != mArchives.rend(); ++it) {
        if ((*it)->HasResource(key)) {
            return true;
        }
    }
    return false;
}

bool VirtualFileSystem::ReadResource(const ResourceKey& key, std::vector<uint8_t>& out) const {
    for (auto it = mArchives.rbegin(); it != mArchives.rend(); ++it) {
        if ((*it)->HasResource(key)) {
            return (*it)->ReadResource(key, out);
        }
    }
    return false;
}

std::vector<ResourceKey> VirtualFileSystem::FindAllByType(uint32_t type) const {
    std::unordered_set<ResourceKey> seen;
    std::vector<ResourceKey> result;

    for (auto it = mArchives.rbegin(); it != mArchives.rend(); ++it) {
        for (const auto& [key, entry] : (*it)->GetEntries()) {
            if (key.type == type && seen.insert(key).second) {
                result.push_back(key);
            }
        }
    }

    return result;
}

void VirtualFileSystem::EnsureNameIndex() const {
    if (mNameIndexBuilt) return;
    mByNameIndex.clear();
    // Iterate lowest-priority first so that when we later read in reverse the
    // highest-priority key ends up in front.
    for (const auto& archive : mArchives) {
        for (const auto& [key, entry] : archive->GetEntries()) {
            mByNameIndex[(uint64_t)key.instance].push_back(key);
        }
    }
    mNameIndexBuilt = true;
}

std::vector<ResourceKey> VirtualFileSystem::FindAllByName(const std::string& name) const {
    EnsureNameIndex();
    std::vector<ResourceKey> result;
    auto it = mByNameIndex.find((uint64_t)HashName(name));
    if (it == mByNameIndex.end()) return result;
    // High-priority packages were inserted last -> reverse for priority order.
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (std::find(result.begin(), result.end(), *rit) == result.end()) {
            result.push_back(*rit);
        }
    }
    return result;
}

bool VirtualFileSystem::FindByName(uint32_t type, const std::string& name, ResourceKey& outKey) const {
    EnsureNameIndex();
    auto it = mByNameIndex.find((uint64_t)HashName(name));
    if (it == mByNameIndex.end()) return false;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (type == 0 || rit->type == type) {
            outKey = *rit;
            return true;
        }
    }
    return false;
}

bool VirtualFileSystem::ReadByName(uint32_t type, const std::string& name, std::vector<uint8_t>& out) const {
    ResourceKey key;
    if (!FindByName(type, name, key)) return false;
    return ReadResource(key, out);
}

} // namespace Spore
