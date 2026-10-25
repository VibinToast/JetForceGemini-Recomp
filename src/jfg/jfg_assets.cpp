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
// "append" in place of "entry" adds a new entry after the set's last one (the -1 end stays). Its number is kept
// between game starts in bin/mod_ids.json (by mod and name); a new one gets "expect" (the number the mod was made
// for) when that is free, else the lowest free number, mods from the top of the mod list down. jfg_mods.log says which
// number each name got, and a number other than "expect" is logged.
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
std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& data); // jfg_deflate.cpp
int asset_number(const std::string& ref);
}

namespace {

// US ROM layout (ver/splat/jfg.us.yaml): the asset table (a count, then count + 1 start offsets relative to the
// asset area, the last one being its end) and the asset area right after it.
constexpr uint32_t ROM_ASSET_TABLE = 0xB1750;
constexpr uint32_t ROM_ASSET_AREA = 0xB1880;
constexpr uint32_t ROM_LIMIT = 0x0FC00000; // the cartridge window of the PI bus (0x10000000-0x1FBFFFFF)
constexpr char CONTENT_FILE[] = "jfg_assets.json";

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
};

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
std::vector<uint8_t> linked(const Replacement& r) {
    if (r.fixups.empty()) return r.data;
    const std::string what = r.file;
    if (!r.packed) {
        std::vector<uint8_t> out = r.data;
        apply_fixups(out, r.fixups, r.mod_id, what);
        return out;
    }
    std::vector<uint8_t> plain;
    if (!unpack_entry(r.data, plain)) {
        log("assets: " + r.mod_id + ": " + what + " says it is packed but doesn't unpack - its fixups are skipped");
        return r.data;
    }
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
std::vector<int> assign_numbers(int table_asset, int data_asset, size_t count,
                                const std::vector<const Replacement*>& appends) {
    const std::string key = std::to_string(table_asset) + "/" + std::to_string(data_asset);
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
    std::vector<Replacement> reps = std::move(g_pending);
    g_pending.clear();
    std::vector<Patch> patches = std::move(g_patches);
    g_patches.clear();
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
    }
    save_ids();
    for (auto& [data_asset, set] : sets) {
        entries += std::max(0, replace_entries(assets[set.table], assets[data_asset], set.table, data_asset, set.entries,
                                               set.appends, numbers[data_asset], set.patches));
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
