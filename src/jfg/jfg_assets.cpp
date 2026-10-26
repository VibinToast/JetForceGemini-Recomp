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
//     "entries": [ { "table": 1, "data": 0, "entry": 291, "file": "assets/tex3d_291.bin" },
//                  { "table": 30, "data": 31, "append": "gemini", "expect": 412, "file": "levels/gemini.bin" } ]
//   }
// "assets" replaces a whole asset. "entries" replaces one entry of a set whose table asset is a list of big-endian
// offsets into its data asset, ended by -1 (the 2D and 3D texture sets are tables 3 and 1 over data 2 and 0; see
// tools/textures.py in the decomp for the entry format). The files are the game's own formats, and they go in the
// .nrm through RecompModTool's additional_files. When two mods replace the same asset or entry, the one higher in the
// mod list wins, as with texture packs.
// "copyOf": N in place of "file" makes the entry from entry N of the same set in the player's own data (with "set":
// [{ "at", "hex" }] byte edits and fixups on top; into the unpacked bytes when "packed"), so a mod that only changes a
// copy of game data carries none of it (a reskinned enemy: its definition and model, pointing at the mod's textures).
// "objtypes": [{ "append": "name", "def": "objdef name", "like": stock object id, "expect": N }] adds object ids (asset
// 0x30, the object index) for appended definitions; the squad code treats each like its "like" id (BaddyTypeToIndex).
// "animRows": [{ "model": "model name", "from": stock model id }] gives an appended model the animation lists and joint
// slots of a stock one (assets 0x28/0x29 and 0x2C/0x2D are indexed by model number).
// "append" in place of "entry" adds a new entry after the set's last one (the -1 end stays). Its number is kept
// between game starts in bin/mod_ids.json (by mod and name); a new one gets "expect" (the number the mod was made
// for) when that is free, else the lowest free number, mods from the top of the mod list down. jfg_mods.log says which
// number each name got, and a number other than "expect" is logged.
// "regions": [{ "append": "name", "name": "Zephyra", "world": "Zephyra Expanse", "expect": 31 }] adds regions (a level
// header's +0x102: the Tribal / stats region; the game uses 1-30 and its save keeps records for 40), numbered 31-39
// like appended entries (key "regions"). A level in one gets the region's stats screen when an exit (+0x1A bit 0, as
// most of the game's) leads into another region, the area title with "name" and "world" when it is entered, and the
// game's continues; the Tribal table (object table 0x34) is lengthened so the mods' regions have their own Tribal
// counts ("tribals", default 0: rescues there stay out of the game's own totals and its "all Tribals" flag). A level
// takes the number with a fixup { "at": 258, "type": "u8", "ref": "name" } (jfg_regions.cpp shows the names).
// "fixups" on an entry (or on a patch's object-list operation) write numbers the mod can't know into its bytes once
// they are given out: { "at": 0x54, "type": "s16", "ref": "track_field" } ("ref": a name of the mod's own, or
// "mod:name" of another mod's; types u8, u16, s16, u32, and "lohi" for an exit's target level). With "packed": true the
// entry is a packed model or track, and the fixups go into its unpacked bytes.
// "patches" changes entries of the player's own data instead of replacing them, so a mod carries only its changes and
// several mods can change one entry (a stock level's header or object list) together:
//     "patches": [ { "table": 30, "data": 31, "entry": 21, "set": [ { "at": 96, "hex": "ff5000" } ] },
//                  { "table": 28, "data": 29, "entry": 424, "objlist": [
//                      { "op": "set", "index": 12, "id": 120, "at": 4, "hex": "00960000ff6a" },
//                      { "op": "delete", "index": 40, "id": 233 },
//                      { "op": "append", "hex": "00a9000e..." } ] } ]
// "set" writes bytes at an offset of the entry. "objlist" edits an object list (a 16-byte header with the entries'
// length at +0, then entries of u16 object id, u8 size, ...): "set" writes inside entry "index", "delete" removes it,
// "append" adds an entry at the end. Indices are the list's own before the patch; "id" must be the object id at that
// index, or the operation is skipped and logged (the mod was made for other data). Patches apply after replacements,
// mods from the top of the mod list down, each mod's in file order.
// "rotateY": 90 / 180 / 270 on a packed model entry (a "copyOf" one, say) turns the model about Y as it is made: a prop
// whose list entry has no facing (the plain 10-byte entry) can face another way. rotate_model says what it turns.
// "baddies" patches the squad code's animation table (overlay 3 data, not an asset) by field: jfg_baddies.cpp.
//
// At game start, before the game's first instruction, the port copies the whole asset area with the replacements in
// it to the end of the ROM and points the table there. The game never sees the difference: it reads the table at
// boot, sizes come from the table, and all data reads go through it. The original area stays where it is (unused).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& data); // jfg_deflate.cpp
int asset_number(const std::string& ref);
bool mod_region(int region, std::string* name, std::string* world);
bool appended_entry(int data_asset, int entry, std::string* owner);
void read_baddy_patches(const nlohmann::json& j, const std::string& id, size_t order); // jfg_baddies.cpp
void activate_baddy_patches();
int baddy_like(int object_id);
std::string texture_owner(int tex_id);
bool mod_textures();
}

namespace {

// US ROM layout (ver/splat/jfg.us.yaml): the asset table (a count, then count + 1 start offsets relative to the
// asset area, the last one being its end) and the asset area right after it.
constexpr uint32_t ROM_ASSET_TABLE = 0xB1750;
constexpr uint32_t ROM_ASSET_AREA = 0xB1880;
constexpr uint32_t ROM_LIMIT = 0x0FC00000; // the cartridge window of the PI bus (0x10000000-0x1FBFFFFF)
constexpr char CONTENT_FILE[] = "jfg_assets.json";
constexpr uint32_t OBJINDEX = 0x30;                   // the object index: s16 definition number per object id
constexpr uint32_t ANIM_LISTS = 0x28, ANIM_IDS = 0x29;   // u16 byte offsets per model into u16 animation ids
constexpr uint32_t ANIM_SLOTS = 0x2C, ANIM_SLOT_DATA = 0x2D; // s32 offsets per model into the joint-slot bytes
constexpr int MODEL_DATA = 0x27;                            // the models (table 0x26)

// A number a mod's data needs but can't know: another appended entry's ("ref": "name" for the mod's own, "mod:name"
// for another mod's), written at "at" when the numbers are given out. Types: "u8", "u16", "s16", "u32" (big endian,
// the number plus "add"), "lohi" (an exit's target level: low byte at "at", high byte at "at" + 1, 0xFF below 256).
struct Fixup {
    int at = 0;
    std::string type;
    std::string ref;  // "mod:name", made full when read
    int add = 0;
};

struct Replacement {
    size_t order;        // the mod's place in the mod list
    std::string mod_id;
    std::string file;
    int asset = -1;      // whole-asset replacement, or the data asset of an entry
    int table = -1;      // entries only
    int entry = -1;      // entries only
    std::string append;  // appended entries: the mod's name for it (entry stays -1)
    int expect = -1;     // appended entries: the number the mod expects, -1 any
    std::vector<uint8_t> data;
    std::vector<Fixup> fixups;
    bool packed = false; // fixups go into the unpacked bytes of a packed (model or track) entry
    int copy_of = -1;    // "copyOf": the entry of the same set the bytes are copied from (the player's own data)
    int rotate_y = 0;    // "rotateY" on a packed model: quarter turns about Y (1, 2, 3) given to the copy (rotate_model)
    std::vector<std::pair<int, std::vector<uint8_t>>> sets; // byte edits, before the fixups
};

struct ObjType {             // "objtypes": a new object id for an appended definition
    size_t order;
    std::string mod_id, append, def; // def: "mod:name" of the appended definition
    int like = -1;           // the stock object id the squad code treats it as
    int expect = -1;
};
struct AnimRows {            // "animRows": an appended model gets a stock model's animation rows
    std::string mod_id, model; // model: "mod:name"
    int from = -1;
};
struct Region {              // "regions": a Tribal / stats region of a mod's own
    size_t order;
    std::string mod_id, append, name, world;
    int expect = -1;
    int tribals = 0; // the Tribals to rescue in its levels ("tribals"): the HUD's count and the stats screen's total
};
std::vector<ObjType> g_objtypes;
std::vector<AnimRows> g_animrows;
std::vector<Region> g_regions;
std::map<int, std::pair<std::string, std::string>> g_region_names; // region number -> name, world
std::map<int, int> g_baddy_like; // object id -> the stock id the squad code treats it as (BaddyTypeToIndex wrapper)
// Entries a mod replaced, patched or appended (data asset, entry) -> mod id, and whole assets a mod replaced: what
// texture dumping keeps apart from the game's own textures (jfg::texture_owner).
std::map<std::pair<int, int>, std::string> g_entry_owner;
std::map<int, size_t> g_stock_entries; // by data asset: the player's own entries in the sets mods append to
std::map<int, std::string> g_asset_owner;

struct ObjOp {
    enum Kind { Set, Delete, Append } kind;
    int index = -1;  // Set, Delete: the entry's index in the list as it was
    int id = -1;     // Set, Delete: the object id the entry must have
    int at = 0;      // Set: offset inside the entry
    std::vector<uint8_t> bytes;
    std::vector<Fixup> fixups; // offsets into bytes
};

struct Patch {
    size_t order;
    std::string mod_id;
    int table = -1, asset = -1, entry = -1;
    std::vector<std::pair<int, std::vector<uint8_t>>> sets; // (offset, bytes) in the entry
    std::vector<ObjOp> objlist;
};

std::vector<Replacement> g_pending; // filled by enabled mods (load_mods), used once by jfg_apply_asset_mods
std::vector<Patch> g_patches;       // the same, for "patches"

// The content linker: appended entries keep their numbers from one game start to the next (bin/mod_ids.json), so a
// save that names a mod's level or world still finds it when other mods come and go. A remembered number is the
// entry's own; new ones take the number the mod expects if it's free, else the lowest free one. Numbers of mods that
// are not loaded stay reserved (their slots are left empty), until the file is deleted.
constexpr char IDS_FILE[] = "mod_ids.json";
nlohmann::json g_ids = nlohmann::json::object();  // "table/data" -> { "mod:name": number }
std::map<std::string, int> g_numbers;             // "mod:name" -> number, this start's
bool g_ids_changed = false;

constexpr size_t OBJLIST_HEADER = 0x10;
constexpr size_t OBJLIST_BUFFER = 0x3000; // objLoadObjList's buffer, header included

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

// "hex" of a patch operation; false when missing or not an even run of hex digits.
bool json_hex(const nlohmann::json& e, std::vector<uint8_t>& out) {
    if (!e.contains("hex") || !e["hex"].is_string()) return false;
    const std::string& s = e["hex"].get_ref<const std::string&>();
    if (s.empty() || s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)(hi << 4 | lo));
    }
    return true;
}

std::vector<Fixup> read_fixups(const nlohmann::json& e, const std::string& mod_id, bool& ok) {
    std::vector<Fixup> out;
    for (const nlohmann::json& f : e.value("fixups", nlohmann::json::array())) {
        Fixup x;
        x.at = json_int(f, "at");
        x.type = f.value("type", "");
        x.ref = f.value("ref", "");
        x.add = f.contains("add") && f["add"].is_number_integer() ? f["add"].get<int>() : 0;
        if (x.at < 0 || x.ref.empty() || (x.type != "u8" && x.type != "u16" && x.type != "s16" && x.type != "u32" &&
                                           x.type != "lohi")) {
            ok = false;
            return out;
        }
        if (x.ref.find(':') == std::string::npos) x.ref = mod_id + ":" + x.ref;
        out.push_back(std::move(x));
    }
    return out;
}

// Writes the fixups' numbers into bytes; false when one names an entry no mod added (logged).
bool apply_fixups(std::vector<uint8_t>& bytes, const std::vector<Fixup>& fixups, const std::string& mod_id,
                  const std::string& what) {
    bool all = true;
    for (const Fixup& f : fixups) {
        auto it = g_numbers.find(f.ref);
        const size_t width = f.type == "u8" ? 1 : f.type == "u32" ? 4 : 2;
        if (it == g_numbers.end() || (size_t)f.at + width > bytes.size()) {
            log("assets: " + mod_id + ": " + what + " names \"" + f.ref + "\", which " +
                (it == g_numbers.end() ? "no loaded mod adds (a missing dependency?)" : "lies past the data") +
                " - left as it is");
            all = false;
            continue;
        }
        const int n = it->second + f.add;
        if (f.type == "lohi") {
            bytes[f.at] = (uint8_t)(n & 0xFF);
            bytes[f.at + 1] = n < 0x100 ? 0xFF : (uint8_t)(n >> 8);
        } else {
            for (size_t k = 0; k < width; k++) bytes[f.at + k] = (uint8_t)(n >> (8 * (width - 1 - k)));
        }
    }
    return all;
}

// A packed model or track entry (u32 little endian unpacked size, the byte 9, raw deflate) <-> its bytes.
bool unpack_entry(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    if (raw.size() < 5) return false;
    const size_t size = raw[0] | raw[1] << 8 | raw[2] << 16 | (size_t)raw[3] << 24;
    out.assign(size, 0);
    const size_t got = tinfl_decompress_mem_to_mem(out.data(), size, raw.data() + 5, raw.size() - 5, 0);
    return got == size;
}
std::vector<uint8_t> pack_entry(const std::vector<uint8_t>& data) {
    const std::vector<uint8_t> packed = jfg::deflate_raw(data);
    if (packed.empty()) return {};
    std::vector<uint8_t> out = { (uint8_t)data.size(), (uint8_t)(data.size() >> 8), (uint8_t)(data.size() >> 16),
                                 (uint8_t)(data.size() >> 24), 9 };
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

// An entry's bytes with its fixups written in (into the unpacked bytes for a packed entry).
void apply_sets(std::vector<uint8_t>& bytes, const Replacement& r) {
    for (const auto& [at, value] : r.sets) {
        if (at < 0 || (size_t)at + value.size() > bytes.size()) {
            log("assets: " + r.mod_id + ": " + r.file + ": a byte edit at " + std::to_string(at) + " is past the entry");
            continue;
        }
        std::copy(value.begin(), value.end(), bytes.begin() + at);
    }
}

// A model (unpacked) turned by `quarters` quarter turns about Y, counter-clockwise seen from above as the game's
// facings go: the vertices (+0x1C, +0x12 of them, 10 bytes: s16 x y z), the vertex normals (+0x68: 6-byte entries,
// s8 x y z first, running to the next section) and the bounding box (+0x3C: s16 min x y z, max x y z). Hit spheres
// sit on joints and have no position of their own. A turn keeps the triangles' winding. False (unchanged) when the
// model isn't one this knows: posed points (+0x34) or a skeleton (+0x4F joints) would need turning too.
bool rotate_model(std::vector<uint8_t>& m, int quarters) {
    auto s16 = [&](size_t o) { return (int16_t)(m[o] << 8 | m[o + 1]); };
    auto put16 = [&](size_t o, int v) { m[o] = (uint8_t)(v >> 8); m[o + 1] = (uint8_t)v; };
    auto turn = [&](int& x, int& z) {
        for (int q = 0; q < quarters; q++) { const int t = x; x = z; z = -t; }
    };
    if (m.size() < 0x80 || m[0x2E] != 0 || m[0x4F] != 0) return false;
    const size_t verts = be32(m, 0x1C), count = (size_t)s16(0x12);
    if (verts + 10 * count > m.size()) return false;
    for (size_t i = 0; i < count; i++) {
        const size_t o = verts + 10 * i;
        int x = s16(o), z = s16(o + 4);
        turn(x, z);
        put16(o, x);
        put16(o + 4, z);
    }
    // The normals table has no count: it ends where the next section starts.
    const size_t normals = be32(m, 0x68);
    if (normals != 0) {
        size_t end = m.size();
        for (const size_t ptr : { 0x18, 0x1C, 0x20, 0x24, 0x30, 0x34, 0x38, 0x54, 0x6C, 0x70, 0x80 }) {
            const size_t at = be32(m, ptr);
            if (at > normals && at < end) end = at;
        }
        for (size_t o = normals; o + 6 <= end; o += 6) {
            int x = (int8_t)m[o], z = (int8_t)m[o + 2];
            turn(x, z);
            m[o] = (uint8_t)std::clamp(x, -127, 127);
            m[o + 2] = (uint8_t)std::clamp(z, -127, 127);
        }
    }
    // The bounding box: turn both corners and take the new minimum and maximum of each axis.
    int x0 = s16(0x3C), z0 = s16(0x40), x1 = s16(0x42), z1 = s16(0x46);
    turn(x0, z0);
    turn(x1, z1);
    put16(0x3C, std::min(x0, x1));
    put16(0x40, std::min(z0, z1));
    put16(0x42, std::max(x0, x1));
    put16(0x46, std::max(z0, z1));
    return true;
}

std::vector<uint8_t> linked(const Replacement& r) {
    if (r.fixups.empty() && r.sets.empty() && r.rotate_y == 0) return r.data;
    const std::string what = r.file;
    if (!r.packed) {
        std::vector<uint8_t> out = r.data;
        apply_sets(out, r);
        apply_fixups(out, r.fixups, r.mod_id, what);
        return out;
    }
    std::vector<uint8_t> plain;
    if (!unpack_entry(r.data, plain)) {
        log("assets: " + r.mod_id + ": " + what + " says it is packed but doesn't unpack - its fixups are skipped");
        return r.data;
    }
    if (r.rotate_y != 0 && !rotate_model(plain, r.rotate_y)) {
        log("assets: " + r.mod_id + ": " + what + " can't be turned (posed points or joints) - left as it is");
    }
    apply_sets(plain, r);
    apply_fixups(plain, r.fixups, r.mod_id, what);
    std::vector<uint8_t> packed = pack_entry(plain);
    if (packed.empty()) {
        log("assets: " + r.mod_id + ": " + what + " couldn't be packed again - its fixups are skipped");
        return r.data;
    }
    return packed;
}

void load_ids() {
    g_ids = nlohmann::json::object();
    g_numbers.clear();
    g_ids_changed = false;
    if (std::FILE* f = std::fopen(IDS_FILE, "rb")) {
        std::string text;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object()) g_ids = std::move(j);
        else log("assets: " + std::string(IDS_FILE) + " isn't valid JSON - mods' numbers are given out afresh");
    }
}

void save_ids() {
    if (!g_ids_changed) return;
    if (std::FILE* f = std::fopen(IDS_FILE, "wb")) {
        const std::string text = g_ids.dump(1) + "\n";
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

// The numbers of one set's appended entries (count = its stock entries), remembered ones first.
std::vector<int> assign_numbers(const std::string& key, size_t count, const std::vector<const Replacement*>& appends) {
    nlohmann::json& known = g_ids[key];
    if (!known.is_object()) known = nlohmann::json::object();
    std::map<int, std::string> taken; // number -> ref, every remembered one (loaded or not) is reserved
    for (auto& [ref, n] : known.items()) {
        if (n.is_number_integer() && n.get<int>() >= (int)count) taken[n.get<int>()] = ref;
    }
    std::vector<int> out(appends.size(), -1);
    for (size_t k = 0; k < appends.size(); k++) {
        const std::string ref = appends[k]->mod_id + ":" + appends[k]->append;
        if (known.contains(ref) && known[ref].is_number_integer() && known[ref].get<int>() >= (int)count) {
            out[k] = known[ref].get<int>();
        }
    }
    for (size_t k = 0; k < appends.size(); k++) {
        if (out[k] >= 0) continue;
        const std::string ref = appends[k]->mod_id + ":" + appends[k]->append;
        int n = appends[k]->expect;
        if (n < (int)count || taken.count(n)) {
            n = (int)count;
            while (taken.count(n)) n++;
        }
        out[k] = n;
        taken[n] = ref;
        known[ref] = n;
        g_ids_changed = true;
    }
    for (size_t k = 0; k < appends.size(); k++) g_numbers[appends[k]->mod_id + ":" + appends[k]->append] = out[k];
    return out;
}
std::vector<int> assign_numbers(int table_asset, int data_asset, size_t count,
                                const std::vector<const Replacement*>& appends) {
    return assign_numbers(std::to_string(table_asset) + "/" + std::to_string(data_asset), count, appends);
}

// The object tables (objGetTable): asset 0x19 is s32 word offsets into asset 0x18, ended by -1. Table 0x34 is the
// Tribals of each region (s8, 32 of them). Lengthens it to SAVE_REGIONS (the later tables move up) and gives the mods'
// regions their own Tribal counts (0 unless a mod's "regions" entry says "tribals"). False when the tables aren't what
// the game's code expects.
constexpr uint32_t OBJ_TABLES = 0x18, OBJ_TABLE_INDEX = 0x19;
constexpr int TRIBAL_TABLE = 0x34, STOCK_REGIONS = 31, SAVE_REGIONS = 40;
bool give_tribal_room(std::vector<std::vector<uint8_t>>& assets, const std::map<int, int>& regions) {
    std::vector<uint8_t>& index = assets[OBJ_TABLE_INDEX];
    std::vector<uint8_t>& data = assets[OBJ_TABLES];
    if (index.size() < 4 * (size_t)(TRIBAL_TABLE + 2)) return false;
    const int32_t a = (int32_t)be32(index, 4 * TRIBAL_TABLE);
    int32_t b = (int32_t)be32(index, 4 * (TRIBAL_TABLE + 1));
    if (b == -1) b = (int32_t)(data.size() / 4);
    if (a < 0 || b < a || 4 * (size_t)b > data.size()) return false;
    const size_t have = 4 * (size_t)(b - a), want = (SAVE_REGIONS + 3) & ~3;
    if (have < want) {
        data.insert(data.begin() + 4 * (size_t)b, want - have, 0);
        for (size_t o = 4 * (TRIBAL_TABLE + 1); o + 4 <= index.size(); o += 4) {
            const int32_t w = (int32_t)be32(index, o);
            if (w == -1) break;
            put_be32(index, o, (uint32_t)(w + (int32_t)((want - have) / 4)));
        }
    }
    for (auto [r, count] : regions) data[4 * (size_t)a + r] = (uint8_t)count;
    return true;
}

// Model m (appended, with no "animRows") gets an empty animation list, so the game draws it static. The game counts a
// model's animations as (lists[m + 1] - lists[m]) / 2: past the stock rows the table is short or padded with zeros, so
// m's two entries could hold anything (a non-zero count on a model with no joints crashes its draw in gen_anim_data).
// The table is lengthened to m + 1, and every entry after the stock models' end (entry `stock_models`) that would go
// back carries the one before it. Runs after give_anim_rows, whose rows are in order and start where the rows before
// them end, so m's two entries come out equal.
void give_empty_anim_row(std::vector<std::vector<uint8_t>>& assets, int m, size_t stock_models) {
    std::vector<uint8_t>& lists = assets[ANIM_LISTS];
    auto u16 = [&](size_t k) { return (uint32_t)(lists[2 * k] << 8 | lists[2 * k + 1]); };
    if (lists.size() < 2 * (size_t)(m + 2)) lists.resize(2 * (size_t)(m + 2), 0);
    for (size_t k = stock_models + 1; k <= (size_t)m + 1; k++) {
        if (u16(k) < u16(k - 1)) {
            lists[2 * k] = lists[2 * k - 2];
            lists[2 * k + 1] = lists[2 * k - 1];
        }
    }
}

// Model m gets model `from`'s animation list and joint slots, appended at the end of the id list and slot data. The
// rows of the models between the last stock one and m (numbers kept for mods not loaded) stay empty. Returns false
// when the tables aren't what the game's code expects.
bool give_anim_rows(std::vector<std::vector<uint8_t>>& assets, int m, int from) {
    std::vector<uint8_t>& lists = assets[ANIM_LISTS];
    std::vector<uint8_t>& ids = assets[ANIM_IDS];
    std::vector<uint8_t>& slots = assets[ANIM_SLOTS];
    std::vector<uint8_t>& slot_data = assets[ANIM_SLOT_DATA];
    auto u16 = [](const std::vector<uint8_t>& b, size_t o) { return (uint32_t)(b[o] << 8 | b[o + 1]); };
    if (lists.size() < 2 * (size_t)(from + 2) || slots.size() < 4 * (size_t)(from + 2)) return false;
    // animation ids: [lists[from], lists[from + 1]) in bytes of `ids`
    const uint32_t a0 = u16(lists, 2 * from), a1 = u16(lists, 2 * from + 2);
    const int32_t s0 = (int32_t)be32(slots, 4 * from), s1 = (int32_t)be32(slots, 4 * from + 4);
    if (a1 < a0 || a1 > ids.size() || s0 < 0 || s1 < s0 || (size_t)s1 > slot_data.size()) return false;
    // Where the stock rows end: the largest offset in each table (the end of the last model's row).
    uint32_t ids_end = 0;
    for (size_t o = 0; o + 2 <= lists.size(); o += 2) ids_end = std::max(ids_end, u16(lists, o));
    int32_t slot_end = 0;
    size_t slot_words = 0;
    for (size_t o = 0; o + 4 <= slots.size(); o += 4, slot_words++) {
        const int32_t v = (int32_t)be32(slots, o);
        if (v == -1) break;
        slot_end = std::max(slot_end, v);
    }
    const std::vector<uint8_t> copy_ids(ids.begin() + a0, ids.begin() + a1);
    const std::vector<uint8_t> copy_slots(slot_data.begin() + s0, slot_data.begin() + s1);
    if (ids_end + copy_ids.size() > 0xFFFF) return false; // u16 offsets
    ids.resize(std::max<size_t>(ids.size(), ids_end));
    ids.insert(ids.begin() + ids_end, copy_ids.begin(), copy_ids.end());
    slot_data.resize(std::max<size_t>(slot_data.size(), (size_t)slot_end));
    slot_data.insert(slot_data.begin() + slot_end, copy_slots.begin(), copy_slots.end());
    // Grow the offset tables to model m + 1; rows up to m start at the old end (empty), m's ends after its copy.
    const size_t old_lists = lists.size() / 2;
    if (lists.size() < 2 * (size_t)(m + 2)) lists.resize(2 * (size_t)(m + 2), 0);
    // Past the stock rows the table may hold padding (zeros): every row up to m that would start before the previous
    // one starts at the end instead (an empty row).
    for (size_t k = 1; k <= (size_t)m; k++) {
        if (k >= old_lists || u16(lists, 2 * k) < u16(lists, 2 * k - 2)) {
            lists[2 * k] = (uint8_t)(ids_end >> 8);
            lists[2 * k + 1] = (uint8_t)ids_end;
        }
    }
    lists[2 * m] = (uint8_t)(ids_end >> 8);
    lists[2 * m + 1] = (uint8_t)ids_end;
    const uint32_t ids_new_end = ids_end + (uint32_t)copy_ids.size();
    lists[2 * m + 2] = (uint8_t)(ids_new_end >> 8);
    lists[2 * m + 3] = (uint8_t)ids_new_end;
    std::vector<uint8_t> new_slots;
    int32_t prev = 0;
    for (size_t k = 0; k <= (size_t)m + 1; k++) {
        int32_t v;
        if (k < slot_words && k <= (size_t)m) v = std::min((int32_t)be32(slots, 4 * k), slot_end);
        else v = slot_end;
        if (v < prev) v = slot_end;  // padding past the stock rows (the US table has zeros, no -1): an empty row
        if (k == (size_t)m) v = slot_end;
        if (k == (size_t)m + 1) v = slot_end + (int32_t)copy_slots.size();
        prev = v;
        new_slots.resize(new_slots.size() + 4);
        put_be32(new_slots, new_slots.size() - 4, (uint32_t)v);
    }
    new_slots.resize(new_slots.size() + 4);
    put_be32(new_slots, new_slots.size() - 4, 0xFFFFFFFFu);
    if (4 * (slot_words + 1) < slots.size()) {  // words after the -1 stay after it
        new_slots.insert(new_slots.end(), slots.begin() + 4 * (slot_words + 1), slots.end());
    }
    slots = std::move(new_slots);
    pad16(lists);
    pad16(ids);
    pad16(slots);
    pad16(slot_data);
    return true;
}

// The "patches" of one mod's jfg_assets.json.
void read_patches(const nlohmann::json& j, const std::string& id, size_t order) {
    for (const nlohmann::json& e : j.value("patches", nlohmann::json::array())) {
        Patch p;
        p.order = order;
        p.mod_id = id;
        p.table = json_int(e, "table");
        p.asset = json_int(e, "data");
        p.entry = json_int(e, "entry");
        if (p.table < 0 || p.asset < 0 || p.entry < 0) {
            log("assets: " + id + ": a patch needs \"table\", \"data\" and \"entry\": " + e.dump());
            continue;
        }
        bool ok = true;
        for (const nlohmann::json& s : e.value("set", nlohmann::json::array())) {
            std::vector<uint8_t> bytes;
            const int at = json_int(s, "at");
            if (at < 0 || !json_hex(s, bytes)) { ok = false; break; }
            p.sets.emplace_back(at, std::move(bytes));
        }
        for (const nlohmann::json& s : e.value("objlist", nlohmann::json::array())) {
            ObjOp op;
            const std::string kind = s.value("op", "");
            op.kind = kind == "set" ? ObjOp::Set : kind == "delete" ? ObjOp::Delete : ObjOp::Append;
            if (kind != "set" && kind != "delete" && kind != "append") { ok = false; break; }
            if (op.kind != ObjOp::Append) {
                op.index = json_int(s, "index");
                op.id = json_int(s, "id");
                if (op.index < 0 || op.id < 0) { ok = false; break; }
            }
            if (op.kind == ObjOp::Set) {
                op.at = json_int(s, "at");
                if (op.at < 0) { ok = false; break; }
            }
            if (op.kind != ObjOp::Delete && !json_hex(s, op.bytes)) { ok = false; break; }
            op.fixups = read_fixups(s, id, ok);
            if (!ok) break;
            p.objlist.push_back(std::move(op));
        }
        if (!ok) {
            log("assets: " + id + ": a patch operation is malformed - the whole patch is skipped: " + e.dump());
            continue;
        }
        g_patches.push_back(std::move(p));
    }
}

// Applies one patch to an entry's bytes; false (and the entry unchanged) when the patch doesn't fit it.
bool apply_patch(std::vector<uint8_t>& entry, const Patch& p, const char* where) {
    char line[256];
    std::vector<uint8_t> out = entry;
    for (const auto& [at, bytes] : p.sets) {
        if ((size_t)at + bytes.size() > out.size()) {
            std::snprintf(line, sizeof line, "assets: %s: patch of %s entry %d writes past its end (%zu bytes) - skipped",
                          p.mod_id.c_str(), where, p.entry, out.size());
            log(line);
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), out.begin() + at);
    }
    if (!p.objlist.empty()) {
        if (out.size() < OBJLIST_HEADER) { log("assets: " + p.mod_id + ": patch of a list that is too short - skipped"); return false; }
        const size_t length = be32(out, 0);
        if (OBJLIST_HEADER + length > out.size()) { log("assets: " + p.mod_id + ": patch of a list with a bad length - skipped"); return false; }
        std::vector<std::vector<uint8_t>> items;
        for (size_t o = OBJLIST_HEADER; o + 4 <= OBJLIST_HEADER + length;) {
            const size_t size = out[o + 2];
            if (size < 10 || o + size > OBJLIST_HEADER + length) { log("assets: " + p.mod_id + ": patch of a broken list - skipped"); return false; }
            items.emplace_back(out.begin() + o, out.begin() + o + size);
            o += size;
        }
        std::vector<bool> deleted(items.size());
        std::vector<std::vector<uint8_t>> appended;
        for (const ObjOp& op_in : p.objlist) {
            ObjOp op = op_in;
            apply_fixups(op.bytes, op.fixups, p.mod_id, std::string(where) + " patch");
            if (op.kind == ObjOp::Append) {
                if (op.bytes.size() < 10 || op.bytes.size() > 255 || op.bytes[2] != op.bytes.size()) {
                    log("assets: " + p.mod_id + ": an appended object's size byte doesn't match its length - skipped");
                    continue;
                }
                appended.push_back(op.bytes);
                continue;
            }
            const bool found = (size_t)op.index < items.size();
            const int id = found ? (items[op.index][0] << 8 | items[op.index][1]) : -1;
            if (!found || id != op.id || deleted[op.index]) {
                std::snprintf(line, sizeof line, "assets: %s: %s entry %d: object %d is %s, not id %d - operation "
                              "skipped (the mod was made for other data)", p.mod_id.c_str(), where, p.entry, op.index,
                              found ? (deleted[op.index] ? "deleted" : std::to_string(id).c_str()) : "missing", op.id);
                log(line);
                continue;
            }
            if (op.kind == ObjOp::Delete) {
                deleted[op.index] = true;
            } else if ((size_t)op.at + op.bytes.size() > items[op.index].size() || op.at < 3) {
                std::snprintf(line, sizeof line, "assets: %s: %s entry %d: a set on object %d is outside it (or over its "
                              "id and size) - skipped", p.mod_id.c_str(), where, p.entry, op.index);
                log(line);
            } else {
                std::copy(op.bytes.begin(), op.bytes.end(), items[op.index].begin() + op.at);
            }
        }
        std::vector<uint8_t> list(OBJLIST_HEADER);
        std::copy(out.begin() + 4, out.begin() + OBJLIST_HEADER, list.begin() + 4); // the header's other 12 bytes
        for (size_t k = 0; k < items.size(); k++) {
            if (!deleted[k]) list.insert(list.end(), items[k].begin(), items[k].end());
        }
        for (const auto& a : appended) list.insert(list.end(), a.begin(), a.end());
        put_be32(list, 0, (uint32_t)(list.size() - OBJLIST_HEADER));
        pad16(list);
        if (list.size() > OBJLIST_BUFFER) {
            std::snprintf(line, sizeof line, "assets: %s: patch of %s entry %d makes the list 0x%zX bytes, past the game's "
                          "0x%zX - skipped", p.mod_id.c_str(), where, p.entry, list.size(), OBJLIST_BUFFER);
            log(line);
            return false;
        }
        out = std::move(list);
    }
    entry = std::move(out);
    return true;
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
        if (r.copy_of >= 0) {  // the bytes come from the player's own data when the assets are laid out
            r.order = order;
            r.mod_id = id;
            r.file = "copy of entry " + std::to_string(r.copy_of);
            g_pending.push_back(std::move(r));
            return;
        }
        if (!e.contains("file") || !e["file"].is_string()) {
            log("assets: " + id + ": an entry without \"file\": " + e.dump());
            return;
        }
        r.order = order;
        r.mod_id = id;
        r.file = e["file"].get<std::string>();
        bool ok = false;
        std::vector<char> bytes = mod.manifest.file_handle->read_file(r.file, ok);
        // RecompModTool packs additional_files by name only (its folders dropped), so "tex3d/0291.bin" in a mod with
        // code is "0291.bin"; a mod zipped by hand keeps the folders. Either works.
        const size_t slash = r.file.find_last_of("/\\");
        if (!ok && slash != std::string::npos) {
            bytes = mod.manifest.file_handle->read_file(r.file.substr(slash + 1), ok);
        }
        if (!ok) {
            log("assets: " + id + ": " + r.file + " isn't in the mod (add it to additional_files in mod.toml)");
            return;
        }
        r.data.assign(bytes.begin(), bytes.end());
        g_pending.push_back(std::move(r));
    };
    read_patches(j, id, order);
    jfg::read_baddy_patches(j, id, order);
    for (const nlohmann::json& e : j.value("objtypes", nlohmann::json::array())) {
        ObjType t{ order, id, e.value("append", ""), e.value("def", ""), json_int(e, "like"), json_int(e, "expect") };
        if (t.append.empty() || t.def.empty()) {
            log("assets: " + id + ": an object type needs \"append\" and \"def\": " + e.dump());
            continue;
        }
        if (t.def.find(':') == std::string::npos) t.def = id + ":" + t.def;
        g_objtypes.push_back(std::move(t));
    }
    for (const nlohmann::json& e : j.value("regions", nlohmann::json::array())) {
        Region r{ order, id, e.value("append", ""), e.value("name", ""), e.value("world", ""), json_int(e, "expect") };
        r.tribals = std::clamp(e.value("tribals", 0), 0, 127);
        if (r.append.empty()) {
            log("assets: " + id + ": a region needs \"append\": " + e.dump());
            continue;
        }
        g_regions.push_back(std::move(r));
    }
    for (const nlohmann::json& e : j.value("animRows", nlohmann::json::array())) {
        AnimRows a{ id, e.value("model", ""), json_int(e, "from") };
        if (a.model.empty() || a.from < 0) {
            log("assets: " + id + ": animation rows need \"model\" and \"from\": " + e.dump());
            continue;
        }
        if (a.model.find(':') == std::string::npos) a.model = id + ":" + a.model;
        g_animrows.push_back(std::move(a));
    }
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
        if (e.contains("append")) {
            if (!e["append"].is_string() || e["append"].get<std::string>().empty() || e.contains("entry")) {
                log("assets: " + id + ": \"append\" must be a name, and without \"entry\": " + e.dump());
                continue;
            }
            r.append = e["append"].get<std::string>();
            r.expect = json_int(e, "expect");
        } else {
            r.entry = json_int(e, "entry");
        }
        if (r.table < 0 || r.asset < 0 || (r.entry < 0 && r.append.empty())) {
            log("assets: " + id + ": \"table\", \"data\" or \"entry\" missing or invalid: " + e.dump());
            continue;
        }
        bool ok = true;
        r.fixups = read_fixups(e, id, ok);
        r.packed = e.value("packed", false);
        r.copy_of = json_int(e, "copyOf");
        if (const int deg = json_int(e, "rotateY"); deg > 0) {
            if (deg % 90 != 0 || deg >= 360 || !r.packed) {
                log("assets: " + id + ": \"rotateY\" takes 90, 180 or 270 on a packed model - left as it is: " + e.dump());
            } else {
                r.rotate_y = deg / 90;
            }
        }
        for (const nlohmann::json& s : e.value("set", nlohmann::json::array())) {
            std::vector<uint8_t> bytes;
            const int at = json_int(s, "at");
            if (at < 0 || !json_hex(s, bytes)) { ok = false; break; }
            r.sets.emplace_back(at, std::move(bytes));
        }
        if (!ok) {
            log("assets: " + id + ": a fixup is malformed - the entry is skipped: " + e.dump());
            continue;
        }
        add(e, std::move(r));
    }
}

// Replaces entries of one table/data set and appends new ones after its last entry; returns how many entries were
// replaced or added. -1 (and nothing changed) when the table isn't that format.
int replace_entries(std::vector<uint8_t>& table, std::vector<uint8_t>& data, int table_asset, int data_asset,
                     const std::map<int, const Replacement*>& reps, const std::vector<const Replacement*>& appends,
                     const std::vector<int>& numbers, const std::map<int, std::vector<const Patch*>>& patches) {
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
    // The new table: an offset per entry (old and appended), the end offset, -1, then whatever words followed the -1.
    // Appended entries sit at their numbers; numbers reserved for mods that aren't loaded stay as empty entries.
    size_t total = count;
    for (int n : numbers) total = std::max(total, (size_t)n + 1);
    std::vector<uint8_t> new_table(4 * (total + 2));
    new_table.insert(new_table.end(), table.begin() + 4 * (end_index + 1), table.end());
    put_be32(new_table, 4 * (total + 1), 0xFFFFFFFFu);
    std::vector<uint8_t> out;
    out.reserve(data.size());
    for (const auto& [entry, list] : patches) {
        if ((size_t)entry >= count) {
            char line[160];
            std::snprintf(line, sizeof line, "assets: %s: %s has %zu entries, there's no entry %d to patch",
                          list.front()->mod_id.c_str(), where, count, entry);
            log(line);
        }
    }
    for (size_t k = 0; k < count; k++) {
        put_be32(new_table, 4 * k, (uint32_t)out.size());
        auto it = reps.find((int)k);
        auto pt = patches.find((int)k);
        if (pt != patches.end()) {
            std::vector<uint8_t> entry = it != reps.end() ? linked(*it->second)
                : std::vector<uint8_t>(data.begin() + offs[k], data.begin() + offs[k + 1]);
            for (const Patch* p : pt->second) {
                if (apply_patch(entry, *p, where)) {
                    applied++;
                    char line[200];
                    std::snprintf(line, sizeof line, "assets: %s: patched entry %zu of %s", p->mod_id.c_str(), k, where);
                    log(line);
                }
            }
            out.insert(out.end(), entry.begin(), entry.end());
            pad16(out);
        } else if (it != reps.end()) {
            const std::vector<uint8_t> bytes = linked(*it->second);
            out.insert(out.end(), bytes.begin(), bytes.end());
            pad16(out);
        } else {
            out.insert(out.end(), data.begin() + offs[k], data.begin() + offs[k + 1]);
        }
    }
    if (!appends.empty()) pad16(out);
    std::map<int, const Replacement*> by_number;
    for (size_t k = 0; k < appends.size(); k++) by_number[numbers[k]] = appends[k];
    for (size_t n = count; n < total; n++) {
        put_be32(new_table, 4 * n, (uint32_t)out.size());
        auto found = by_number.find((int)n);
        if (found == by_number.end()) continue; // reserved for a mod that isn't loaded: an empty entry
        const Replacement* r = found->second;
        const int number = (int)n;
        const std::vector<uint8_t> bytes = linked(*r);
        out.insert(out.end(), bytes.begin(), bytes.end());
        pad16(out);
        char line[256];
        std::snprintf(line, sizeof line, "assets: %s: \"%s\" is entry %d of %s (%zu bytes)", r->mod_id.c_str(),
                      r->append.c_str(), number, where, r->data.size());
        log(line);
        if (r->expect >= 0 && r->expect != number) {
            std::snprintf(line, sizeof line, "assets: %s: \"%s\" was made for entry %d, but got %d - anything that "
                          "names it by number points elsewhere", r->mod_id.c_str(), r->append.c_str(), r->expect, number);
            log(line);
        }
    }
    put_be32(new_table, 4 * total, (uint32_t)out.size());
    out.insert(out.end(), data.begin() + offs[count], data.end()); // whatever followed the last entry
    pad16(out);
    table = std::move(new_table);
    data = std::move(out);
    return applied + (int)appends.size();
}

} // namespace

// The stock object id a mod's object type stands in for in the squad code, or -1 (the BaddyTypeToIndex wrapper).
int jfg::baddy_like(int object_id) {
    auto it = g_baddy_like.find(object_id);
    return it == g_baddy_like.end() ? -1 : it->second;
}

// The mod a texture id comes from (texLoadTexture's id: bit 15 = texture table 1 over data asset 0, else table 3 over
// data asset 2), or "" for the game's own.
std::string jfg::texture_owner(int tex_id) {
    tex_id &= 0xFFFF;
    const int data_asset = tex_id & 0x8000 ? 0 : 2;
    auto it = g_entry_owner.find({ data_asset, tex_id & 0x7FFF });
    if (it != g_entry_owner.end()) return it->second;
    auto whole = g_asset_owner.find(data_asset);
    return whole == g_asset_owner.end() ? std::string() : whole->second;
}
bool jfg::mod_textures() { return !g_entry_owner.empty() || g_asset_owner.count(0) || g_asset_owner.count(2); }

// A mod region's name and world text (jfg_regions.cpp); false when `region` isn't one.
bool jfg::mod_region(int region, std::string* name, std::string* world) {
    auto it = g_region_names.find(region);
    if (it == g_region_names.end()) return false;
    if (name) *name = it->second.first;
    if (world) *world = it->second.second;
    return true;
}

// True when `entry` of a set was appended by a mod (not one of the player's own data), with that mod's id.
bool jfg::appended_entry(int data_asset, int entry, std::string* owner) {
    auto stock = g_stock_entries.find(data_asset);
    if (stock == g_stock_entries.end() || entry < (int)stock->second) return false;
    auto it = g_entry_owner.find({ data_asset, entry });
    if (it == g_entry_owner.end()) return false;
    if (owner) *owner = it->second;
    return true;
}

// The number an appended entry got this start ("mod_id:append name"), or -1 (jfg_launch: --start-level by name).
int jfg::asset_number(const std::string& ref) {
    auto it = g_numbers.find(ref);
    return it == g_numbers.end() ? -1 : it->second;
}

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
    jfg::activate_baddy_patches();
    std::vector<Replacement> reps = std::move(g_pending);
    g_pending.clear();
    std::vector<Patch> patches = std::move(g_patches);
    g_patches.clear();
    std::vector<Region> regions = std::move(g_regions);
    g_regions.clear();
    if (reps.empty() && patches.empty()) return;
    std::stable_sort(reps.begin(), reps.end(), [](const Replacement& a, const Replacement& b) { return a.order < b.order; });
    std::stable_sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) { return a.order < b.order; });

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

    // Whole assets, in mod order (top of the list first): the first mod to replace an asset keeps it.
    std::vector<std::string> owner(count);
    int whole = 0, entries = 0;
    for (const Replacement& r : reps) {
        if (r.entry >= 0 || !r.append.empty()) continue; // entries below
        if ((uint32_t)r.asset >= count) {
            log("assets: " + r.mod_id + ": there's no asset " + std::to_string(r.asset) + " (" + r.file + ")");
            continue;
        }
        if (!owner[r.asset].empty()) {
            log("assets: asset " + std::to_string(r.asset) + " is replaced by " + owner[r.asset] + " and " + r.mod_id +
                "; " + owner[r.asset] + " wins (higher in the mod list)");
            continue;
        }
        owner[r.asset] = r.mod_id;
        assets[r.asset] = r.data;
        pad16(assets[r.asset]);
        whole++;
    }

    // Entries, grouped by set; within a set the first mod's entry (highest in the list) is kept. Appended entries keep
    // mod order, then file order (reps is sorted stably by mod order).
    struct Set {
        int table;
        std::map<int, const Replacement*> entries;
        std::vector<const Replacement*> appends;
        std::map<int, std::vector<const Patch*>> patches;
    };
    std::map<int, Set> sets; // by data asset
    for (const Replacement& r : reps) {
        if (r.entry < 0 && r.append.empty()) continue;
        if ((uint32_t)r.asset >= count || (uint32_t)r.table >= count || r.asset == r.table) {
            log("assets: " + r.mod_id + ": no such table/data pair (" + r.file + ")");
            continue;
        }
        auto [it, fresh] = sets.try_emplace(r.asset, Set{ r.table, {}, {}, {} });
        if (it->second.table != r.table) {
            log("assets: " + r.mod_id + ": data asset " + std::to_string(r.asset) + " was given another table by a mod "
                "higher in the mod list; skipped " + r.file);
            continue;
        }
        if (!r.append.empty()) {
            std::vector<const Replacement*>& appends = it->second.appends;
            if (std::any_of(appends.begin(), appends.end(), [&](const Replacement* a) {
                    return a->mod_id == r.mod_id && a->append == r.append; })) {
                log("assets: " + r.mod_id + ": \"" + r.append + "\" is appended to data asset " + std::to_string(r.asset) +
                    " twice; skipped " + r.file);
                continue;
            }
            appends.push_back(&r);
            continue;
        }
        const Replacement*& slot = it->second.entries[r.entry];
        if (slot != nullptr) {
            log("assets: entry " + std::to_string(r.entry) + " of data asset " + std::to_string(r.asset) + " is replaced by " +
                slot->mod_id + " and " + r.mod_id + "; " + slot->mod_id + " wins (higher in the mod list)");
            continue;
        }
        slot = &r;
    }
    for (const Patch& p : patches) {
        if ((uint32_t)p.asset >= count || (uint32_t)p.table >= count || p.asset == p.table) {
            log("assets: " + p.mod_id + ": a patch names no such table/data pair");
            continue;
        }
        auto [it, fresh] = sets.try_emplace(p.asset, Set{ p.table, {}, {}, {} });
        if (it->second.table != p.table) {
            log("assets: " + p.mod_id + ": a patch gives data asset " + std::to_string(p.asset) + " another table; skipped");
            continue;
        }
        it->second.patches[p.entry].push_back(&p);
    }
    // Entries copied from the player's own data ("copyOf").
    for (Replacement& r : reps) {
        if (r.copy_of < 0) continue;
        const std::vector<uint8_t>& table = assets[r.table];
        const std::vector<uint8_t>& data = assets[r.asset];
        const size_t at = 4 * (size_t)r.copy_of;
        if (at + 8 > table.size()) {
            log("assets: " + r.mod_id + ": copyOf " + std::to_string(r.copy_of) + ": no such entry in table " +
                std::to_string(r.table));
            continue;
        }
        const int32_t a = (int32_t)be32(table, at), b = (int32_t)be32(table, at + 4);
        if (a < 0 || b < a || (size_t)b > data.size()) {
            log("assets: " + r.mod_id + ": copyOf " + std::to_string(r.copy_of) + " has offsets that don't fit");
            continue;
        }
        r.data.assign(data.begin() + a, data.begin() + b);
    }
    // Numbers first, for every set, so a fixup can name an entry of any set (a level's track, a track's textures).
    load_ids();
    std::map<int, std::vector<int>> numbers;
    for (auto& [data_asset, set] : sets) {
        const std::vector<uint8_t>& table = assets[set.table];
        size_t stock = 0;
        for (size_t o = 0; o + 4 <= table.size(); o += 4) {
            if ((int32_t)be32(table, o) == -1) { stock = o / 4 >= 1 ? o / 4 - 1 : 0; break; }
        }
        numbers[data_asset] = assign_numbers(set.table, data_asset, stock, set.appends);
        g_stock_entries[data_asset] = stock;
    }
    // New object ids: numbered like appended entries (key "48/48", the object index asset), before any fixup names one.
    std::vector<ObjType> objtypes = std::move(g_objtypes);
    g_objtypes.clear();
    std::stable_sort(objtypes.begin(), objtypes.end(), [](const ObjType& a, const ObjType& b) { return a.order < b.order; });
    std::vector<Replacement> type_reps(objtypes.size());
    std::vector<const Replacement*> type_ptrs;
    for (size_t k = 0; k < objtypes.size(); k++) {
        type_reps[k].mod_id = objtypes[k].mod_id;
        type_reps[k].append = objtypes[k].append;
        type_reps[k].expect = objtypes[k].expect;
        type_ptrs.push_back(&type_reps[k]);
    }
    std::vector<int> type_numbers;
    if (!type_ptrs.empty() && OBJINDEX < count) {
        type_numbers = assign_numbers(OBJINDEX, OBJINDEX, assets[OBJINDEX].size() / 2, type_ptrs);
    }
    // Regions: numbered from 31 like appended entries (key "regions"). Past the save's 40 they are refused, and the
    // levels that name one keep the region their headers have.
    std::stable_sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.order < b.order; });
    std::vector<Replacement> region_reps(regions.size());
    std::vector<const Replacement*> region_ptrs;
    for (size_t k = 0; k < regions.size(); k++) {
        region_reps[k].mod_id = regions[k].mod_id;
        region_reps[k].append = regions[k].append;
        region_reps[k].expect = regions[k].expect;
        region_ptrs.push_back(&region_reps[k]);
    }
    std::vector<int> region_numbers;
    if (!region_ptrs.empty()) region_numbers = assign_numbers("regions", STOCK_REGIONS, region_ptrs);
    std::map<int, int> given_regions; // region number -> its Tribals
    for (size_t k = 0; k < region_numbers.size(); k++) {
        const Region& r = regions[k];
        if (region_numbers[k] >= SAVE_REGIONS) {
            log("assets: " + r.mod_id + ": region \"" + r.append + "\" would be region " + std::to_string(region_numbers[k]) +
                ", past the save's " + std::to_string(SAVE_REGIONS) + " - its levels keep the region their headers have");
            g_numbers.erase(r.mod_id + ":" + r.append);
            continue;
        }
        given_regions[region_numbers[k]] = r.tribals;
        g_region_names[region_numbers[k]] = { r.name.empty() ? r.append : r.name, r.world };
        log("assets: " + r.mod_id + ": region \"" + r.append + "\" is region " + std::to_string(region_numbers[k]) +
            (r.tribals ? ", " + std::to_string(r.tribals) + " Tribals" : ""));
    }
    save_ids();
    if (!given_regions.empty()) {
        if (give_tribal_room(assets, given_regions)) entries++;
        else log("assets: the object tables aren't what the game expects - the mods' regions have no Tribal counts");
    }
    for (auto& [data_asset, set] : sets) {
        entries += std::max(0, replace_entries(assets[set.table], assets[data_asset], set.table, data_asset, set.entries,
                                               set.appends, numbers[data_asset], set.patches));
        for (const auto& [entry, r] : set.entries) g_entry_owner[{ data_asset, entry }] = r->mod_id;
        for (const auto& [entry, list] : set.patches) g_entry_owner.try_emplace({ data_asset, entry }, list.front()->mod_id);
        for (size_t k = 0; k < set.appends.size(); k++) g_entry_owner[{ data_asset, numbers[data_asset][k] }] = set.appends[k]->mod_id;
    }
    for (uint32_t i = 0; i < count; i++) if (!owner[i].empty()) g_asset_owner[(int)i] = owner[i];
    // The object index for the new ids (their definitions' numbers are known now).
    for (size_t k = 0; k < type_numbers.size(); k++) {
        const ObjType& t = objtypes[k];
        const int def = jfg::asset_number(t.def);
        const int n = type_numbers[k];
        if (def < 0) {
            log("assets: " + t.mod_id + ": object type \"" + t.append + "\" names definition " + t.def +
                ", which no loaded mod appends - skipped");
            continue;
        }
        std::vector<uint8_t>& index = assets[OBJINDEX];
        if (index.size() < 2 * (size_t)(n + 1)) index.resize(2 * (size_t)(n + 1), 0xFF); // -1: no definition
        index[2 * n] = (uint8_t)(def >> 8);
        index[2 * n + 1] = (uint8_t)def;
        if (t.like >= 0) g_baddy_like[n] = t.like;
        log("assets: " + t.mod_id + ": object type \"" + t.append + "\" is object id " + std::to_string(n) +
            " (definition " + std::to_string(def) + (t.like >= 0 ? ", squads treat it as " + std::to_string(t.like) : "") + ")");
        entries++;
    }
    // Animation rows for appended models.
    std::vector<AnimRows> animrows = std::move(g_animrows);
    g_animrows.clear();
    std::set<int> with_rows;
    for (const AnimRows& a : animrows) {
        const int m = jfg::asset_number(a.model);
        if (m < 0) {
            log("assets: " + a.mod_id + ": animation rows for " + a.model + ", which no loaded mod appends - skipped");
            continue;
        }
        if (give_anim_rows(assets, m, a.from)) {
            log("assets: " + a.mod_id + ": model " + std::to_string(m) + " has the animation rows of model " +
                std::to_string(a.from));
            with_rows.insert(m);
            entries++;
        } else {
            log("assets: " + a.mod_id + ": model " + std::to_string(a.from) + "'s animation rows couldn't be copied");
        }
    }
    // Appended models with no rows of their own get an empty row (a static model: props made from a copied model).
    if (auto models = numbers.find(MODEL_DATA); models != numbers.end()) {
        for (const int m : models->second) {
            if (m < 0 || with_rows.count(m)) continue;
            give_empty_anim_row(assets, m, g_stock_entries[MODEL_DATA]);
            entries++;
        }
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
    std::snprintf(line, sizeof line, "assets: %d whole assets and %d entries replaced or added; the asset area is now at ROM "
                  "0x%08zX (ROM %.1f MB)", whole, entries, area, out.size() / 1048576.0);
    log(line);
    recomp::set_rom_contents(std::move(out));
}

// --- runtime assets (jfg_assets.h) ------------------------------------------------------------------------------------
#include "recomp.h"
#include "jfg_assets.h"
#include "jfg_game.h"
#include "jfg_hooks.h"

namespace {
struct RuntimeAsset {
    std::string owner;
    std::vector<uint8_t> data;
};
RuntimeAsset g_runtime[jfg::assets::COUNT];
constexpr uint32_t COLOUR_TAG_GREY = 0x7F7F7FFF; // piRomLoad's mmAlloc colour (src/memory.h)

const RuntimeAsset* runtime(uint32_t asset) {
    return asset < (uint32_t)jfg::assets::COUNT && !g_runtime[asset].owner.empty() ? &g_runtime[asset] : nullptr;
}

void copy_to_rdram(uint8_t* rdram, uint32_t address, std::span<const uint8_t> data) {
    for (size_t i = 0; i < data.size(); i++) MEM_B((int32_t)i, (gpr)(int32_t)address) = (int8_t)data[i];
}
} // namespace

namespace jfg::assets {

std::span<const uint8_t> read_rom(int asset) {
    const std::span<const uint8_t> rom = recomp::get_rom();
    if (rom.size() < ROM_ASSET_AREA) return {};
    const uint32_t count = be32(rom, ROM_ASSET_TABLE);
    if ((uint32_t)asset >= count) return {};
    const uint64_t start = (uint64_t)ROM_ASSET_AREA + be32(rom, ROM_ASSET_TABLE + 4 + 4 * asset);
    const uint64_t end = (uint64_t)ROM_ASSET_AREA + be32(rom, ROM_ASSET_TABLE + 8 + 4 * asset);
    if (end < start || end > rom.size()) return {};
    return rom.subspan(start, end - start);
}

std::span<const uint8_t> read(int asset) {
    if (const RuntimeAsset* r = runtime((uint32_t)asset)) return r->data;
    return read_rom(asset);
}

bool set_runtime(int asset, std::vector<uint8_t> data, const std::string& owner) {
    if (asset < 0 || asset >= COUNT || owner.empty()) return false;
    RuntimeAsset& r = g_runtime[asset];
    if (!r.owner.empty() && r.owner != owner) {
        log("assets: " + owner + " wants to serve asset " + std::to_string(asset) + ", which " + r.owner +
            " already serves; refused");
        return false;
    }
    r.owner = owner;
    r.data = std::move(data);
    return true;
}

void clear_runtime(int asset, const std::string& owner) {
    if (asset < 0 || asset >= COUNT || g_runtime[asset].owner != owner) return;
    g_runtime[asset] = {};
}

} // namespace jfg::assets

// The game's loaders (src/pi.c): an asset with a runtime version is read from it, the same way the ROM's would be.
JFG_WRAP(piRomLoad) {
    const RuntimeAsset* r = runtime((uint32_t)ctx->r4);
    if (r == nullptr) { piRomLoad_orig(rdram, ctx); return; }
    ctx->r2 = 0;
    if (r->data.empty()) return;
    const uint32_t out = jfg::call_game(mmAlloc, ctx, { (uint32_t)r->data.size(), COLOUR_TAG_GREY });
    if (out == 0) return;
    copy_to_rdram(rdram, out, r->data);
    ctx->r2 = (gpr)(int32_t)out;
}

JFG_WRAP(piRomLoadSection) {
    const RuntimeAsset* r = runtime((uint32_t)ctx->r4);
    if (r == nullptr) { piRomLoadSection_orig(rdram, ctx); return; }
    const uint32_t address = (uint32_t)ctx->r5, offset = (uint32_t)ctx->r6, size = (uint32_t)ctx->r7;
    ctx->r2 = 0;
    if (size == 0) return;
    std::vector<uint8_t> part(size, 0); // past the end reads as zeros (the ROM would give the next asset)
    if (offset < r->data.size()) std::copy_n(r->data.begin() + offset, std::min<size_t>(size, r->data.size() - offset), part.begin());
    copy_to_rdram(rdram, address, part);
    ctx->r2 = (gpr)(int32_t)size;
}

JFG_WRAP(piRomGetFileSize) {
    const RuntimeAsset* r = runtime((uint32_t)ctx->r4);
    if (r == nullptr) { piRomGetFileSize_orig(rdram, ctx); return; }
    ctx->r2 = (gpr)(int32_t)r->data.size();
}
