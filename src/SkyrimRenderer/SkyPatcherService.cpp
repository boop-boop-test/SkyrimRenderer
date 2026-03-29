#include "SkyrimRenderer/SkyPatcherService.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace SkyrimRenderer {

SkyPatcherService& SkyPatcherService::GetInstance() {
    static SkyPatcherService instance;
    return instance;
}

void SkyPatcherService::Load() {
    std::lock_guard lock(mutex_);
    SKSE::log::info("[SkyPatcherService] Load() called");
    overrides_.clear();
    loaded_ = true;
    ScanAndParseAll();
}

RE::TESNPC* SkyPatcherService::Resolve(RE::TESNPC* npc) {
    if (!npc) return npc;

    std::lock_guard lock(mutex_);
    if (!loaded_) return npc;

    auto it = overrides_.find(npc->GetFormID());
    if (it == overrides_.end()) return npc;

    auto* donor = RE::TESForm::LookupByID<RE::TESNPC>(it->second.donorFormID);
    if (!donor) {
        SKSE::log::warn("[SkyPatcherService] Donor NPC 0x{:X} not found for override of '{}'",
                        it->second.donorFormID, npc->GetName());
        return npc;
    }

    SKSE::log::info("[SkyPatcherService] Resolved '{}' (0x{:X}) -> '{}' (0x{:X})",
                    npc->GetFullName(), npc->GetFormID(),
                    donor->GetFullName(), donor->GetFormID());
    return donor;
}

void SkyPatcherService::ScanAndParseAll() {
    namespace fs = std::filesystem;

    fs::path npcDir = "Data/SKSE/Plugins/SkyPatcher/npc";

    std::error_code ec;
    if (!fs::exists(npcDir, ec)) {
        SKSE::log::info("[SkyPatcherService] SkyPatcher npc directory not found — SkyPatcher may not be installed");
        return;
    }

    // Collect all INI files with their depth, then sort for priority ordering
    struct FileEntry {
        fs::path path;
        int depth = 0;
    };
    std::vector<FileEntry> files;

    for (auto& entry : fs::recursive_directory_iterator(npcDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(::tolower(c));
        if (ext != ".ini") continue;

        // Check conditional INI files: "SomeMod.esp.ini" only loads if that plugin is active
        auto filename = entry.path().filename().string();
        // Check if filename matches pattern: *.esp.ini, *.esm.ini, *.esl.ini
        auto stem = entry.path().stem().string(); // e.g., "SomeMod.esp" from "SomeMod.esp.ini"
        if (stem.ends_with(".esp") || stem.ends_with(".esm") || stem.ends_with(".esl")) {
            auto* dhand = RE::TESDataHandler::GetSingleton();
            if (!dhand || !dhand->LookupModByName(stem)) {
                continue; // Plugin not loaded, skip this conditional INI
            }
        }

        // Calculate depth relative to npcDir
        auto relPath = fs::relative(entry.path(), npcDir, ec);
        int depth = 0;
        if (!ec) {
            for (auto& component : relPath.parent_path()) {
                (void)component;
                ++depth;
            }
        }

        files.push_back({entry.path(), depth});
    }

    // Sort by depth (ascending) then filename (ascending) so later entries overwrite earlier ones.
    // This means deeper files win, and at same depth, later alphanumeric wins.
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.path.filename().string() < b.path.filename().string();
    });

    uint32_t fileCount = 0;
    for (auto& [filePath, depth] : files) {
        ParseFile(filePath, depth);
        ++fileCount;
    }

    SKSE::log::info("[SkyPatcherService] Parsed {} INI files, {} visual style overrides loaded",
                    fileCount, overrides_.size());
}

void SkyPatcherService::ParseFile(const std::filesystem::path& filePath, int depth) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        SKSE::log::warn("[SkyPatcherService] Failed to open: {}", filePath.string());
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim leading whitespace
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments
        if (line[0] == ';') continue;

        ParseLine(line, depth);
    }
}

void SkyPatcherService::ParseLine(const std::string& line, int depth) {
    // SkyPatcher lines are colon-delimited key=value pairs, e.g.:
    // filterByNpcs=AdrianneAvenicci:copyVisualStyle=Bijin_AdrianneAvenicci
    // filterByNpcs=Skyrim.esm|246EE7:copyVisualStyle=Bijin AIO.esp|C6DC

    // Split on ':'
    std::vector<std::string> parts;
    std::istringstream stream(line);
    std::string part;
    while (std::getline(stream, part, ':')) {
        parts.push_back(part);
    }

    // Find filterByNpcs and copyVisualStyle directives
    std::string filterValue;
    std::string copyVisualValue;

    for (auto& p : parts) {
        auto eqPos = p.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = p.substr(0, eqPos);
        std::string value = p.substr(eqPos + 1);

        // Trim whitespace from key
        auto keyStart = key.find_first_not_of(" \t");
        auto keyEnd = key.find_last_not_of(" \t");
        if (keyStart != std::string::npos)
            key = key.substr(keyStart, keyEnd - keyStart + 1);

        // Case-insensitive key comparison
        std::string keyLower = key;
        for (auto& c : keyLower) c = static_cast<char>(::tolower(c));

        if (keyLower == "filterbynpcs") {
            filterValue = value;
        } else if (keyLower == "copyvisualstyle") {
            copyVisualValue = value;
        }
    }

    // Need both directives on the same line
    if (filterValue.empty() || copyVisualValue.empty()) return;

    // Resolve the donor NPC
    RE::TESNPC* donor = ResolveNPC(copyVisualValue);
    if (!donor) {
        SKSE::log::warn("[SkyPatcherService] Could not resolve donor NPC: '{}'", copyVisualValue);
        return;
    }

    // Split filterByNpcs on ',' for multiple targets
    std::istringstream targetStream(filterValue);
    std::string target;
    while (std::getline(targetStream, target, ',')) {
        // Trim whitespace
        auto tStart = target.find_first_not_of(" \t");
        auto tEnd = target.find_last_not_of(" \t");
        if (tStart == std::string::npos) continue;
        target = target.substr(tStart, tEnd - tStart + 1);

        RE::TESNPC* targetNPC = ResolveNPC(target);
        if (!targetNPC) {
            SKSE::log::warn("[SkyPatcherService] Could not resolve target NPC: '{}'", target);
            continue;
        }

        RE::FormID targetID = targetNPC->GetFormID();

        // Insert or update based on priority (deeper nesting wins, later file at same depth wins)
        auto existing = overrides_.find(targetID);
        if (existing == overrides_.end() || depth >= existing->second.priority) {
            overrides_[targetID] = {donor->GetFormID(), depth};
        }
    }
}

RE::TESNPC* SkyPatcherService::ResolveNPC(const std::string& identifier) {
    if (identifier.empty()) return nullptr;

    // Check if it's a FormID format: "Plugin.esp|HexID"
    if (identifier.find('|') != std::string::npos) {
        auto* form = ParseSkyPatcherFormString(identifier);
        return form ? form->As<RE::TESNPC>() : nullptr;
    }

    // Try editor ID lookup
    auto* form = RE::TESForm::LookupByEditorID(identifier);
    if (form) {
        return form->As<RE::TESNPC>();
    }

    SKSE::log::warn("[SkyPatcherService] Could not resolve NPC by editor ID: '{}'", identifier);
    return nullptr;
}

RE::TESForm* SkyPatcherService::ParseSkyPatcherFormString(const std::string& formStr) {
    // SkyPatcher format: "Plugin.esp|HexLocalID" (note: reversed from "0xID|Plugin.esp" convention)
    auto pipePos = formStr.find('|');
    if (pipePos == std::string::npos) return nullptr;

    std::string pluginName = formStr.substr(0, pipePos);
    std::string hexIdStr = formStr.substr(pipePos + 1);

    // Trim whitespace
    auto trimStr = [](std::string& s) {
        auto start = s.find_first_not_of(" \t");
        auto end = s.find_last_not_of(" \t");
        if (start == std::string::npos) { s.clear(); return; }
        s = s.substr(start, end - start + 1);
    };
    trimStr(pluginName);
    trimStr(hexIdStr);

    if (pluginName.empty() || hexIdStr.empty()) return nullptr;

    // Parse the hex local ID
    uint32_t localId = 0;
    try {
        localId = std::stoul(hexIdStr, nullptr, 16);
    } catch (const std::exception&) {
        SKSE::log::warn("[SkyPatcherService] Failed to parse hex FormID: '{}'", hexIdStr);
        return nullptr;
    }

    // Look up the plugin
    auto* dhand = RE::TESDataHandler::GetSingleton();
    if (!dhand) return nullptr;

    const RE::TESFile* modInfo = dhand->LookupModByName(pluginName);
    if (!modInfo) {
        SKSE::log::warn("[SkyPatcherService] Plugin not found: '{}'", pluginName);
        return nullptr;
    }

    // Construct runtime FormID (same logic as Forms::ParseFormString)
    uint32_t formId = 0;
    if (modInfo->IsLight()) {
        uint16_t smallFileIndex = modInfo->GetSmallFileCompileIndex();
        formId = 0xFE000000 | (static_cast<uint32_t>(smallFileIndex) << 12) | (localId & 0xFFF);
    } else {
        uint8_t compileIndex = modInfo->GetCompileIndex();
        formId = (static_cast<uint32_t>(compileIndex) << 24) | localId;
    }

    return formId == 0 ? nullptr : RE::TESForm::LookupByID(formId);
}

} // namespace SkyrimRenderer
