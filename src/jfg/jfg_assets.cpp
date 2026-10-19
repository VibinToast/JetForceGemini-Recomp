// Jet Force Gemini port: asset mods - mods that replace game data (textures, models, levels, music...) instead of code.
//
// The game reads all of its data through one table (src/pi.c in the decomp): 71 "assets", each a byte range of the
// asset area at ROM 0xB1880, whose start offsets are listed at ROM 0xB1750. piInit copies that table to RAM at boot;
// piRomLoad, piRomLoadCompressed and piRomLoadSection read an asset (or part of one) through it. An asset's size is
// the distance to the next asset's start, so the assets form one unbroken chain and none can grow in place.
//
// A mod with a jfg_assets.json at the top of its .nrm replaces data:
//   {
//     "assets":  [ { "asset": 34, "file": "assets/asset34.bin" } ],
//     "entries": [ { "table": 1, "data": 0, "entry": 291, "file": "assets/tex3d_291.bin" } ]
//   }
// "assets" replaces a whole asset. "entries" replaces one entry of a set whose table asset is a list of big-endian
// offsets into its data asset, ended by -1 (the 2D and 3D texture sets are tables 3 and 1 over data 2 and 0; see
// tools/textures.py in the decomp for the entry format). The files are the game's own formats, and they go in the
// .nrm through RecompModTool's additional_files. Mods lower in the mod list are applied later, so they win.
//
// At game start, before the game's first instruction, the port copies the whole asset area with the replacements in
// it to the end of the ROM and points the table there. The game never sees the difference: it reads the table at
// boot, sizes come from the table, and all data reads go through it. The original area stays where it is (unused).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

// US ROM layout (ver/splat/jfg.us.yaml): the asset table (a count, then count + 1 start offsets relative to the
// asset area, the last one being its end) and the asset area right after it.
constexpr uint32_t ROM_ASSET_TABLE = 0xB1750;
constexpr uint32_t ROM_ASSET_AREA = 0xB1880;
constexpr uint32_t ROM_LIMIT = 0x0FC00000; // the cartridge window of the PI bus (0x10000000-0x1FBFFFFF)
constexpr char CONTENT_FILE[] = "jfg_assets.json";

struct Replacement {
    size_t order;        // the mod's place in the mod list
    std::string mod_id;
    std::string file;
    int asset = -1;      // whole-asset replacement, or the data asset of an entry
    int table = -1;      // entries only
    int entry = -1;      // entries only
    std::vector<uint8_t> data;
};

std::vector<Replacement> g_pending; // filled by enabled mods (load_mods), used once by jfg_apply_asset_mods

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3];
}
void put_be32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = (uint8_t)(v >> 24); b[o + 1] = (uint8_t)(v >> 16); b[o + 2] = (uint8_t)(v >> 8); b[o + 3] = (uint8_t)v;
}
void pad16(std::vector<uint8_t>& b) { b.resize((b.size() + 15) & ~size_t(15)); }

int json_int(const nlohmann::json& e, const char* key) {
    if (!e.contains(key) || !e[key].is_number_integer()) return -1;
    const long long v = e[key].get<long long>();
    return v < 0 || v > 0xFFFFFF ? -1 : (int)v;
}

// ModContentType::on_enabled for jfg_assets.json: runs inside load_mods, once per enabled mod, at every game start.
void on_asset_mod_enabled(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    const std::string& id = mod.manifest.mod_id;
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(CONTENT_FILE, exists);
    if (!exists) return;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log("assets: " + id + ": jfg_assets.json isn't valid JSON - none of its assets are used");
        return;
    }
    const size_t order = context.get_mod_order_index(id);
    auto add = [&](const nlohmann::json& e, Replacement r) {
        if (!e.contains("file") || !e["file"].is_string()) {
            log("assets: " + id + ": an entry without \"file\": " + e.dump());
            return;
        }
        r.order = order;
        r.mod_id = id;
        r.file = e["file"].get<std::string>();
        bool ok = false;
        const std::vector<char> bytes = mod.manifest.file_handle->read_file(r.file, ok);
        if (!ok) {
            log("assets: " + id + ": " + r.file + " isn't in the mod (add it to additional_files in mod.toml)");
            return;
        }
        r.data.assign(bytes.begin(), bytes.end());
        g_pending.push_back(std::move(r));
    };
    for (const nlohmann::json& e : j.value("assets", nlohmann::json::array())) {
        Replacement r;
        r.asset = json_int(e, "asset");
        if (r.asset < 0) { log("assets: " + id + ": \"asset\" missing or invalid: " + e.dump()); continue; }
        add(e, std::move(r));
    }
    for (const nlohmann::json& e : j.value("entries", nlohmann::json::array())) {
        Replacement r;
        r.table = json_int(e, "table");
        r.asset = json_int(e, "data");
        r.entry = json_int(e, "entry");
        if (r.table < 0 || r.asset < 0 || r.entry < 0) {
            log("assets: " + id + ": \"table\", \"data\" or \"entry\" missing or invalid: " + e.dump());
            continue;
        }
        add(e, std::move(r));
    }
}

// Replaces entries of one table/data set; returns how many. -1 (and nothing changed) when the table isn't that format.
int replace_entries(std::vector<uint8_t>& table, std::vector<uint8_t>& data, int table_asset, int data_asset,
                     const std::map<int, const Replacement*>& reps) {
    std::vector<int32_t> offs;
    size_t end_index = SIZE_MAX;
    for (size_t o = 0; o + 4 <= table.size(); o += 4) {
        offs.push_back((int32_t)be32(table, o));
        if (offs.back() == -1) { end_index = offs.size() - 1; break; }
    }
    char where[64];
    std::snprintf(where, sizeof where, "table %d / data %d", table_asset, data_asset);
    if (end_index == SIZE_MAX || end_index < 1) {
        log(std::string("assets: ") + where + " isn't a list of offsets ended by -1 - its entries are skipped");
        return -1;
    }
    const size_t count = end_index - 1; // the last offset before -1 is the end of the last entry
    for (size_t k = 0; k < count; k++) {
        if (offs[k] < 0 || offs[k] > offs[k + 1] || (size_t)offs[k + 1] > data.size()) {
            log(std::string("assets: ") + where + " has offsets that don't fit its data - its entries are skipped");
            return -1;
        }
    }
    int applied = 0;
    for (const auto& [entry, r] : reps) {
        if ((size_t)entry < count) {
            applied++;
        } else {
            char line[160];
            std::snprintf(line, sizeof line, "assets: %s: %s has %zu entries, there's no entry %d (%s)", r->mod_id.c_str(),
                          where, count, entry, r->file.c_str());
            log(line);
        }
    }
    std::vector<uint8_t> out;
    out.reserve(data.size());
    for (size_t k = 0; k < count; k++) {
        put_be32(table, 4 * k, (uint32_t)out.size());
        auto it = reps.find((int)k);
        if (it != reps.end()) {
            out.insert(out.end(), it->second->data.begin(), it->second->data.end());
            pad16(out);
        } else {
            out.insert(out.end(), data.begin() + offs[k], data.begin() + offs[k + 1]);
        }
    }
    put_be32(table, 4 * count, (uint32_t)out.size());
    out.insert(out.end(), data.begin() + offs[count], data.end()); // whatever followed the last entry
    pad16(out);
    data = std::move(out);
    return applied;
}

} // namespace

// Called from jfg_register_mod_exports (src/main.cpp, before the runtime starts): mods with a jfg_assets.json carry
// asset replacements. They can't be switched while the game runs (the game reads its data as it goes).
extern "C" void jfg_register_asset_mods(void) {
    recomp::mods::ModContentType type{
        .content_filename = CONTENT_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_asset_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(type);
}

// Called from the game's entry point (src/main.cpp), after the runtime has loaded mods and before the game runs.
extern "C" void jfg_apply_asset_mods(void) {
    std::vector<Replacement> reps = std::move(g_pending);
    g_pending.clear();
    if (reps.empty()) return;
    std::stable_sort(reps.begin(), reps.end(), [](const Replacement& a, const Replacement& b) { return a.order < b.order; });

    const std::span<const uint8_t> rom = recomp::get_rom();
    if (rom.size() < ROM_ASSET_AREA) { log("assets: the ROM is too small - asset mods are off"); return; }
    const uint32_t count = be32(rom, ROM_ASSET_TABLE);
    if (ROM_ASSET_TABLE + 4 + 4 * (count + 1) > ROM_ASSET_AREA) { log("assets: unexpected asset table - asset mods are off"); return; }
    std::vector<std::vector<uint8_t>> assets(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t start = (uint64_t)ROM_ASSET_AREA + be32(rom, ROM_ASSET_TABLE + 4 + 4 * i);
        const uint64_t end = (uint64_t)ROM_ASSET_AREA + be32(rom, ROM_ASSET_TABLE + 8 + 4 * i);
        if (end < start || end > rom.size()) { log("assets: unexpected asset table - asset mods are off"); return; }
        assets[i].assign(rom.begin() + start, rom.begin() + end);
    }

    // Whole assets, in mod order: a later mod's replacement wins.
    std::vector<std::string> owner(count);
    int whole = 0, entries = 0;
    for (const Replacement& r : reps) {
        if (r.entry >= 0) continue;
        if ((uint32_t)r.asset >= count) {
            log("assets: " + r.mod_id + ": there's no asset " + std::to_string(r.asset) + " (" + r.file + ")");
            continue;
        }
        if (!owner[r.asset].empty()) {
            log("assets: asset " + std::to_string(r.asset) + " is replaced by " + owner[r.asset] + " and " + r.mod_id +
                "; " + r.mod_id + " wins (lower in the mod list)");
        }
        owner[r.asset] = r.mod_id;
        assets[r.asset] = r.data;
        pad16(assets[r.asset]);
        whole++;
    }

    // Entries, grouped by set; within a set a later mod's entry wins.
    std::map<int, std::pair<int, std::map<int, const Replacement*>>> sets; // data asset -> table asset, entries
    for (const Replacement& r : reps) {
        if (r.entry < 0) continue;
        if ((uint32_t)r.asset >= count || (uint32_t)r.table >= count || r.asset == r.table) {
            log("assets: " + r.mod_id + ": no such table/data pair (" + r.file + ")");
            continue;
        }
        auto [it, fresh] = sets.try_emplace(r.asset, r.table, std::map<int, const Replacement*>{});
        if (it->second.first != r.table) {
            log("assets: " + r.mod_id + ": data asset " + std::to_string(r.asset) + " was given another table by an "
                "earlier mod; skipped " + r.file);
            continue;
        }
        const Replacement*& slot = it->second.second[r.entry];
        if (slot != nullptr) {
            log("assets: entry " + std::to_string(r.entry) + " of data asset " + std::to_string(r.asset) + " is replaced by " +
                slot->mod_id + " and " + r.mod_id + "; " + r.mod_id + " wins (lower in the mod list)");
        }
        slot = &r;
    }
    for (auto& [data_asset, set] : sets) {
        entries += std::max(0, replace_entries(assets[set.first], assets[data_asset], set.first, data_asset, set.second));
    }
    if (whole == 0 && entries == 0) return;

    // Lay the assets out again after the end of the ROM and point the table at them.
    std::vector<uint8_t> out(rom.begin(), rom.end());
    out.resize((out.size() + 0xFFFF) & ~size_t(0xFFFF));
    const size_t area = out.size();
    for (uint32_t i = 0; i < count; i++) {
        put_be32(out, ROM_ASSET_TABLE + 4 + 4 * i, (uint32_t)(out.size() - ROM_ASSET_AREA));
        out.insert(out.end(), assets[i].begin(), assets[i].end());
    }
    put_be32(out, ROM_ASSET_TABLE + 4 + 4 * count, (uint32_t)(out.size() - ROM_ASSET_AREA));
    if (out.size() > ROM_LIMIT) {
        log("assets: the replaced assets make the ROM larger than 252 MB - asset mods are off");
        return;
    }
    char line[200];
    std::snprintf(line, sizeof line, "assets: %d whole assets and %d entries replaced; the asset area is now at ROM "
                  "0x%08zX (ROM %.1f MB)", whole, entries, area, out.size() / 1048576.0);
    log(line);
    recomp::set_rom_contents(std::move(out));
}
