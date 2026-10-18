// Jet Force Gemini port: HD texture packs (the choice, F6) and texture dumping, for the renderer
// (src/renderer/renderer.cpp).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

std::atomic<int> g_texture_pack_count{ -1 }; // textures listed in bin/texture_pack/rt64.json, -1 = pack not in use
// Texture packs: folders in bin/texture_packs/<name> (and the older single bin/texture_pack). The choice is kept in
// bin/jfg_texture_pack.txt; the renderer picks it up through jfg_texture_pack_current().
std::mutex g_pack_mutex;
std::string g_pack_folder;          // relative to the app folder, "" = none
std::atomic<int> g_pack_generation{ 0 };
std::chrono::steady_clock::time_point g_pack_toast; // when F6 last switched packs (on-screen note)
constexpr const char* PACK_CHOICE_FILE = "jfg_texture_pack.txt";

// The available packs: "texture_pack" if that folder exists, then every folder and .rtz file in texture_packs, sorted.
std::vector<std::string> list_texture_packs() {
    std::vector<std::string> packs;
    std::error_code ec;
    if (std::filesystem::is_directory("texture_pack", ec)) packs.emplace_back("texture_pack");
    std::vector<std::string> named;
    for (std::filesystem::directory_iterator it("texture_packs", ec), end; !ec && it != end; it.increment(ec)) {
        // A pack is a folder, or a packed pack: a .rtz file (a zip with rt64.json at its root).
        bool is_pack = it->is_directory();
        if (!is_pack && it->is_regular_file()) {
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            is_pack = ext == ".rtz";
        }
        if (is_pack) { const auto n8 = it->path().filename().u8string(); named.emplace_back("texture_packs/" + std::string(n8.begin(), n8.end())); }
    }
    std::sort(named.begin(), named.end());
    packs.insert(packs.end(), named.begin(), named.end());
    return packs;
}

std::string pack_display_name(const std::string& folder) {
    if (folder.empty()) return "Off (original textures)";
    std::string name = folder.rfind("texture_packs/", 0) == 0 ? folder.substr(14) : folder;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".rtz") == 0) name.resize(name.size() - 4);
    return name;
}

// Chooses a pack (or "" for none), saves the choice and makes the renderer (re)load it.
void set_texture_pack(const std::string& folder) {
    {
        std::lock_guard lock(g_pack_mutex);
        g_pack_folder = folder;
    }
    g_pack_generation++;
    if (std::FILE* f = std::fopen(PACK_CHOICE_FILE, "w")) { std::fprintf(f, "%s\n", folder.c_str()); std::fclose(f); }
}

void load_texture_pack_choice() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::string folder;
    if (std::FILE* f = std::fopen(PACK_CHOICE_FILE, "r")) {
        char line[512] = {};
        if (std::fgets(line, sizeof(line), f)) folder = line;
        std::fclose(f);
        while (!folder.empty() && (folder.back() == '\n' || folder.back() == '\r')) folder.pop_back();
    }
    else {
        std::error_code ec;
        if (std::filesystem::is_directory("texture_pack", ec)) folder = "texture_pack"; // as before packs had names
    }
    std::lock_guard lock(g_pack_mutex);
    g_pack_folder = folder;
    g_pack_generation++;
}

// F6: the next pack in the list, then Off, then the first again.
void next_texture_pack() {
    const std::vector<std::string> packs = list_texture_packs();
    std::string current;
    { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
    std::string next;
    if (current.empty()) next = packs.empty() ? "" : packs.front();
    else {
        auto it = std::find(packs.begin(), packs.end(), current);
        next = (it == packs.end() || it + 1 == packs.end()) ? "" : *(it + 1);
    }
    set_texture_pack(next);
    g_pack_toast = std::chrono::steady_clock::now();
}

// Read by the renderer thread (src/renderer/renderer.cpp) for the Textures section of the Options tab.
extern "C" int jfg_texture_dump_active(void) { static const bool env = std::getenv("JFG_DEBUG_TEXTURE_DUMP") != nullptr; return (g.texture_dump || env) ? 1 : 0; }
extern "C" void jfg_texture_pack_set_count(int count) { g_texture_pack_count = count; }

// Texture dumping, "One folder per level": the sub-folder of texture_dump to write into ("" = texture_dump itself).
// Worked out on the game thread (jfg_debug_frame, where the level names are loaded) and read by the renderer thread.
std::mutex g_dump_folder_mutex;
std::string g_dump_folder;
void update_dump_scene();
extern "C" void jfg_texture_dump_folder(char* buf, int size) {
    std::lock_guard lock(g_dump_folder_mutex);
    std::snprintf(buf, (size_t)size, "%s", g_dump_folder.c_str());
}
void update_dump_folder() {
    std::string folder;
    if (g.dump_by_level && g_rdram != nullptr) {
        load_level_names();
        const int level = (int16_t)rd16(ADDR_currentLevel);
        char num[16];
        std::snprintf(num, sizeof(num), "%03d", level);
        folder = num;
        if (level >= 0 && level < (int)g_level_names.size()) {
            std::string name;
            for (char c : g_level_names[level]) name += (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (!name.empty()) folder += " " + name;
        }
    }
    {
        std::lock_guard lock(g_dump_folder_mutex);
        g_dump_folder = folder;
    }
    update_dump_scene();
}

// Texture dumping: the scene tag RT64 stamps on each line of draw_trace.txt (Toasty_Upscaler's RT64_INTEGRATION.md,
// "Scene tags"), from the game's own level number and name: "<number>_<name>" in a level, "front_<level>" for the title
// and menus (the front end draws over a backdrop level), "pause_menu", "loading" while no level is set, "boot" before
// the game is up. RT64 switches to a new tag only after it has held for a few frames.
std::mutex g_dump_scene_mutex;
std::string g_dump_scene = "boot";
extern "C" void jfg_texture_dump_scene(char* buf, int size) {
    std::lock_guard lock(g_dump_scene_mutex);
    std::snprintf(buf, (size_t)size, "%s", g_dump_scene.c_str());
}
void update_dump_scene() {
    if (g_rdram == nullptr || !jfg_texture_dump_active()) return;
    load_level_names();
    const int level = (int16_t)rd16(ADDR_currentLevel);
    std::string tag;
    if (level < 0) tag = "loading";
    else {
        char num[16];
        std::snprintf(num, sizeof(num), "%03d", level);
        tag = num;
        if (level < (int)g_level_names.size()) {
            std::string name;
            for (char c : g_level_names[level]) {
                const char l = (char)std::tolower((unsigned char)c);
                const char out = (std::isalnum((unsigned char)l) || l == '-' || l == '.') ? l : '_';
                if (out != '_' || (!name.empty() && name.back() != '_')) name += out;
            }
            while (!name.empty() && name.back() == '_') name.pop_back();
            if (!name.empty()) tag += "_" + name;
        }
    }
    const int mode = (int)rd32(ADDR_mainGameMode);
    if (mode == 0 && rd8(ADDR_pauseMode) != 0) tag = "pause_menu";
    else if (mode == 1) tag = "front_" + tag;
    else if (mode == 6) tag = "boot";
    else if (mode != 0) tag = "mode" + std::to_string(mode) + "_" + tag;
    if (tag.size() > 48) tag.resize(48);
    std::lock_guard lock(g_dump_scene_mutex);
    g_dump_scene = tag;
}
extern "C" int jfg_texture_pack_current(char* buf, int size) {
    std::lock_guard lock(g_pack_mutex);
    std::snprintf(buf, (size_t)size, "%s", g_pack_folder.c_str());
    return g_pack_generation.load();
}

} // namespace jfg
