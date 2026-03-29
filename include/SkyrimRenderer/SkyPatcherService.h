#pragma once

/// @file SkyPatcherService.h
/// @brief Parses SkyPatcher INI files to resolve NPC visual style overrides.
///
/// When SkyPatcher is installed with copyVisualStyle directives, NPCs may have
/// their appearance (head, face, hair) replaced by a donor NPC form. This service
/// parses the INI files and provides a lookup so the renderer can use the correct
/// appearance data for preview rendering.

#include <mutex>
#include <string>
#include <unordered_map>
#include <filesystem>

#include <RE/T/TESNPC.h>

namespace SkyrimRenderer {

/// Parses SkyPatcher NPC INI files and caches visual style override mappings.
/// Thread-safe singleton. Call Load() on game load to parse and resolve all overrides.
class SkyPatcherService {
public:
    static SkyPatcherService& GetInstance();

    /// Parse SkyPatcher INI files and resolve all NPC overrides.
    /// Call on game data loaded (kDataLoaded) and on save load.
    void Load();

    /// Resolve the visual NPC for rendering — returns the SkyPatcher donor
    /// if one exists, otherwise returns the original NPC unchanged.
    RE::TESNPC* Resolve(RE::TESNPC* npc);

private:
    SkyPatcherService() = default;

    void ScanAndParseAll();
    void ParseFile(const std::filesystem::path& filePath, int depth);
    void ParseLine(const std::string& line, int depth);

    /// Resolve an NPC identifier (editor ID or Plugin.esp|HexID format).
    RE::TESNPC* ResolveNPC(const std::string& identifier);

    /// Parse SkyPatcher-format FormID string: "Plugin.esp|HexLocalID"
    RE::TESForm* ParseSkyPatcherFormString(const std::string& formStr);

    struct OverrideEntry {
        RE::FormID donorFormID = 0;
        int priority = 0;  // deeper nesting = higher priority wins
    };

    /// target NPC FormID -> donor override entry
    std::unordered_map<RE::FormID, OverrideEntry> overrides_;

    mutable std::mutex mutex_;
    bool loaded_ = false;
};

} // namespace SkyrimRenderer
