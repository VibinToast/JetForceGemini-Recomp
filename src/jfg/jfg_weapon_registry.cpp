// Jet Force Gemini port: the weapon registry - weapons beyond the game's own, for mods (mod API version 6).
//
// The game has 16 weapon slots and no room for more: a character's owned weapons are a u16 mask (+0x0A), its ammo,
// capacity and charge are arrays of 16 (+0x14, +0x34, +0x54), the shot counts too (Game +0x474), and weaponTable has 16
// rows. Slots 0-14 are the heroes' weapons; 15 is a cut proximity mine the game never gives (Restored Content's pickup
// does). So slot 15 is the carrier: every weapon past 14 lives in the port, and the one selected borrows slot 15 -
// its ammo and capacity are copied in, its row (a stock weapon's WeaponDef row with its own ammo numbers) is written to
// row 15, its names to texts 0x39 + 15 and 0x48 + 15, and bit 15 is owned while any of them is. Ids: 15 is the
// proximity mine (built in), mods' weapons get 16, 17, ... in the order they register (jfg_weapon_register), any
// number of them; an id is only good for this run, so saves and mods keep names ("mod_id:weapon").
// - Switching: controlSelectNextWeapon / PreviousWeapon step through the owned slots by the mask. Their wrappers step
//   through the carried weapons while slot 15 is in hand (the game's code still runs, for its sound, hand and selector
//   slide, then 15 is put back with the next carried weapon), and pick the first (next) or last (previous) carried
//   weapon on stepping into 15.
// - Firing: the fire code (func_800386B0_392B0) picks the shot by the current weapon. For one shot of a carried weapon
//   with a host, the hero holds the host weapon with the carried weapon's ammo and its charge, then all is put back
//   and the shot counted on 15. jfg_on_weapon_fire comes first, for any weapon: a mod can cancel the game's shot and
//   fire its own. Throwing: controlThrowWeapon(15) answers the carried weapon's `thrown`.
// - Pick-ups: weaponPickupControl gives weapon data +0 (s16). jfg_weapon_pickup_set points a pickup at any weapon: a
//   stock one is written there, a carried one is 15 there and kept by object here; for the pickup's tick, slot 15
//   shows that weapon (owned or not, its ammo, row and names), so the game's own code gives it, then the carrier
//   goes back to the weapon in hand. A slot-15 pickup that wasn't pointed anywhere gives the proximity mine.
// - The pause menu's weapon screen (overlay 18, decomp MSR 8e): its next / previous (func_overlay_18_00001218 /
//   _00001294) wrap over 0..14 only, so their wrappers go over all 16 by the owned mask; slot 15 shows the carried
//   weapon in hand: its name (text 0x39 + 15, as the HUD), its ammo, and for a mod's weapon no description (texts
//   254-258 are the proximity mine's help). Its 3D model stays the proximity mine's (item 89).
// - Icons: the weapon panel, the HUD's weapon strip and the pause screen's d-pad icons draw slot 15 as front-end item
//   25, which is entry 25 of the front-end item list (D_800A51D0, s16 ids: 0x8000 | a 2D sprite; decomp MSR 8a). A
//   weapon with its own icon (jfg_weapon_set_icon: a pack's sprite) has its sprite written there while it is in hand,
//   and the item freed (freeFrontEndItem), so the next frame loads it; others get the game's entry back. A weapon can
//   also have a big picture (jfg_weapon_set_icon_image): where a mod draws the weapon's icon (jfg_hud_weapon_icon) it
//   is drawn instead, sharp at the window's resolution, through the overlay.
// - Saving: the carried weapons' ammo by name, per character, in saves/<save>_weapons.txt when the game saves to that
//   file (the jfg_auto_save.cpp hooks, as Permanent Tribal Saves); choosing a file loads its own. A file saved
//   without it but with slot 15 owned (the proximity mine before the registry) takes slot 15 as the proximity mine.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace recomp { void* alloc(uint8_t* rdram, size_t size); }
namespace jfg {
void overlay_set_image(int id, int w, int h, std::vector<uint32_t> pixels); // jfg_overlay.cpp
void overlay_draw(int id, float x, float y, float w, float h, uint32_t tint);
}

namespace jfg::weapons {
namespace {
constexpr int CARRIER = 15, FIRST_MOD = 16, STOCK = 15, HEROES = 3, SAVE_SLOTS = 6;
constexpr uint32_t ADDR_front_text = 0x800A51C8; // char**, NULL until initFront
constexpr int WEAPON_NAMES = 0x39, AMMO_NAMES = 0x48;
constexpr uint32_t PICKUP_WEAPON = 0x88;         // weaponPickupControl's data in the object: s16 weapon
constexpr uint32_t ROW_MAX_AMMO = 0x24, ROW_START_AMMO = 0x28, ROW_PICKUP_AMMO = 0x2A, ROW_CHARGE = 0x03;
constexpr uint32_t ROW_THROW_DELAY = 0x06;
constexpr int HELP_TEXT = 179;                  // front_text[179 + w * 5 ..]: the weapon screen's five help lines
constexpr int HELP_LINES = 5;
constexpr uint32_t OVERLAY_18_ROM = 0x01F2F9D8; // the pause menu's weapon screen
constexpr uint32_t SHOWN_WEAPON = 0x66B0;       // overlay 18: s32, the weapon the screen shows
constexpr uint32_t ADDR_FRONT_ITEMS = 0x800A51D0; // s16*: the front-end item list (initFront loads asset 0x1A)
constexpr int CARRIER_ITEM = 10 + CARRIER;        // front-end item 25: slot 15's icon
constexpr uint16_t ITEM_SPRITE = 0x8000;
constexpr int BIG_ICON_IMAGE = 0x7E570000;        // overlay picture ids of the weapons' big icons: + weapon id
constexpr float BIG_ICON_WIDTH = 44.0f;           // a big icon's width at scale 1 (the game's are about 32 wide)
constexpr uint8_t THROW_DELAY = 0x1A;

const char* const STOCK_NAMES[STOCK] = {
    "stock:pistol", "stock:homing_missiles", "stock:machine_gun", "stock:plasma_shotgun", "stock:shocker",
    "stock:tri_rocket_launcher", "stock:flamethrower", "stock:sniper_rifle", "stock:grenades", "stock:shuriken",
    "stock:fish_food", "stock:timed_mines", "stock:remote_mines", "stock:flares", "stock:cluster_bombs",
};

struct Weapon {
    std::string name, display, ammo_display;
    int host = -1;       // the stock weapon whose shot it fires (0..14), -1 = the default shot (or a mod's own)
    int charge = -1;     // the charge for the host's shot, -1 = as it is
    bool thrown = false;
    int row_from = 0;    // the stock weapon whose WeaponDef row it takes
    int max_ammo = -1, start_ammo = -1, pickup_ammo = -1; // -1 = the row's
    uint32_t name_text = 0, ammo_text = 0; // the names, in the runtime's heap (made when first shown)
    int icon = -1;       // its HUD icon: a 2D sprite number (jfg_weapon_set_icon), -1 = slot 15's own
    int big_w = 0, big_h = 0; // its big icon's size (jfg_weapon_set_icon_image), 0 = none
    int held_model = -1; // the model in the hero's hand (jfg_weapon_set_held_model: a pack's), -1 = slot 8's
};
// Index id - CARRIER.
std::vector<Weapon> g_weapons = {
    // 15: fires as the timed mine uncharged ((0x20 - 0x20) >> 4 == 0: mine type 0, object 0x127, the proximity
    // mine), thrown, the remote mine's row (refire, speed, 10 mines, +5 a pickup).
    { "port:proximity_mine", "Proximity Mine", "Proximity Mines", 11, 0x20, true, 12 },
};

// What a hero carries past slot 14: ammo by id (owned when present), and the one slot 15 holds.
struct Hero {
    std::map<int, int> ammo;
    int current = -1;
};
Hero g_heroes[HEROES];
// The same by name, as saved: [slot][hero] name -> ammo, and the one in hand.
struct SavedHero {
    std::map<std::string, int> ammo;
    std::string current;
};
SavedHero g_disk[SAVE_SLOTS][HEROES];
bool g_loaded = false;
std::map<uint32_t, int> g_pickups; // pickup object -> carried id (this level)
uint32_t g_blank_text = 0;         // "" in the runtime's heap: a mod weapon's help lines
int g_item_stock = -1;             // item 25's entry as the game has it (read before it is first changed)
uint32_t g_help[HELP_LINES] = {};  // the proximity mine's help lines, while a mod's weapon blanks them

const Weapon* weapon_of(int id) {
    return id >= CARRIER && id < CARRIER + (int)g_weapons.size() ? &g_weapons[(size_t)(id - CARRIER)] : nullptr;
}

// A pointer a mod passed: the game's memory, or mods' own above 0x81000000 (docs/MEMORY.md, to 512 MB).
bool mod_ptr(uint32_t p) { return p >= 0x80000400u && p < 0xA0000000u; }

std::string read_string(uint32_t addr, size_t max = 128) {
    std::string s;
    if (!mod_ptr(addr)) return s;
    for (size_t i = 0; i < max; i++) {
        const char c = (char)rd8(addr + (uint32_t)i);
        if (c == 0) break;
        s += c;
    }
    return s;
}

uint32_t heap_string(const std::string& text) {
    const size_t n = text.size() + 1;
    void* block = recomp::alloc(g_rdram, (n + 3) & ~size_t(3));
    if (block == nullptr) return 0;
    const uint32_t at = 0x80000000u + (uint32_t)((uint8_t*)block - g_rdram);
    for (size_t i = 0; i < n; i++) wr8(at + (uint32_t)i, i < text.size() ? (uint8_t)text[i] : 0);
    return at;
}

bool in_game() { return rd32(ADDR_mainGameMode) == 0; }

int popcount16(uint16_t v) {
    int n = 0;
    for (; v != 0; v &= (uint16_t)(v - 1)) n++;
    return n;
}

// Row 15, capacity, charge and ammo of `id` (the hero's own ammo of it, or none); its names when `names`.
void show(uint32_t ch, int id, int ammo, bool names) {
    Weapon* w = const_cast<Weapon*>(weapon_of(id));
    if (w == nullptr || !valid_ptr(ch)) return;
    const uint32_t from = ADDR_weaponTable + WeaponDef::Size * (uint32_t)w->row_from;
    const uint32_t to = ADDR_weaponTable + WeaponDef::Size * CARRIER;
    for (uint32_t i = 0; i < WeaponDef::Size; i++) wr8(to + i, rd8(from + i));
    if (w->max_ammo >= 0) wr16(to + ROW_MAX_AMMO, (uint16_t)w->max_ammo);
    if (w->start_ammo >= 0) wr16(to + ROW_START_AMMO, (uint16_t)w->start_ammo);
    if (w->pickup_ammo >= 0) wr16(to + ROW_PICKUP_AMMO, (uint16_t)w->pickup_ammo);
    // The throw lets go row +0x06 ticks after the press (decomp MSR 8b); a row without one (0) would throw again
    // every few ticks. A thrown weapon gets the grenade's 26.
    if (w->thrown && (int8_t)rd8(to + ROW_THROW_DELAY) <= 0) wr8(to + ROW_THROW_DELAY, THROW_DELAY);
    if (names) {
        const uint32_t table = rd32(ADDR_front_text);
        if (valid_ptr(table)) {
            if (w->name_text == 0) w->name_text = heap_string(w->display);
            if (w->ammo_text == 0) w->ammo_text = heap_string(w->ammo_display);
            if (w->name_text != 0) wr32(table + 4 * (WEAPON_NAMES + CARRIER), w->name_text);
            if (w->ammo_text != 0) wr32(table + 4 * (AMMO_NAMES + CARRIER), w->ammo_text);
            // The weapon screen's help lines: the proximity mine's own, none for a mod's weapon.
            const uint32_t help = table + 4 * (uint32_t)(HELP_TEXT + CARRIER * HELP_LINES);
            if (g_blank_text == 0) g_blank_text = heap_string("");
            for (int i = 0; i < HELP_LINES && g_blank_text != 0; i++) {
                const uint32_t at = help + 4 * (uint32_t)i, now = rd32(at);
                if (now != g_blank_text) g_help[i] = now; // the game's (the table is filled again on a language change)
                wr32(at, id == CARRIER ? g_help[i] : g_blank_text);
            }
        }
    }
    Character::Ammo::set(ch, CARRIER, (uint16_t)std::clamp(ammo, 0, 0xFFFF));
    wr16(ch + 0x34 + 2 * CARRIER, rd16(to + ROW_MAX_AMMO)); // capacity
    Character::MaxCharge::set(ch, CARRIER, rd8(to + ROW_CHARGE));
}

// Bit 15 and the weapon count as the carried weapons say.
void set_owned_bit(uint32_t ch, bool owned) {
    uint16_t mask = Character::WeaponsOwned::get(ch);
    mask = owned ? (uint16_t)(mask | (1u << CARRIER)) : (uint16_t)(mask & ~(1u << CARRIER));
    Character::WeaponsOwned::set(ch, mask);
    Character::WeaponCount::set(ch, (uint8_t)popcount16(mask));
}

// Slot 15 takes the weapon `h` has in hand (the first it owns if none); its names too when `names`.
void load_carrier(uint32_t ch, Hero& h, bool names) {
    if (h.ammo.empty()) { h.current = -1; return; }
    if (h.ammo.find(h.current) == h.ammo.end()) h.current = h.ammo.begin()->first;
    if (!(Character::WeaponsOwned::get(ch) & (1u << CARRIER))) set_owned_bit(ch, true);
    show(ch, h.current, h.ammo[h.current], names);
}

// Slot 15's ammo back into the weapon in hand (firing and the game's pick-ups change it there).
void store_carrier(uint32_t ch, Hero& h) {
    if (h.current >= 0 && h.ammo.find(h.current) != h.ammo.end()) h.ammo[h.current] = Character::Ammo::get(ch, CARRIER);
}

int current_hero() {
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    return in_game() && c >= 0 && c < HEROES ? c : -1;
}

// The hero a ControlPlayer belongs to (its +1: the character), in single player.
int hero_of_player(uint32_t player) {
    if (!in_game() || !valid_ptr(player)) return -1;
    const int c = rd8(player + 1) & 3;
    return c < HEROES ? c : -1;
}

// Slot 15 owned with no carried weapon: the game gave it (a file saved before the registry, a proximity-mine pickup
// left alone): it is the proximity mine.
void adopt(uint32_t ch, Hero& h) {
    if (h.ammo.empty() && (Character::WeaponsOwned::get(ch) & (1u << CARRIER))) {
        h.ammo[CARRIER] = Character::Ammo::get(ch, CARRIER);
        h.current = CARRIER;
    }
}

// --- saving --------------------------------------------------------------------------------------------------------
std::filesystem::path file_path() {
    std::filesystem::path save = ultramodern::get_save_file_path();
    return save.parent_path() / (save.stem().u8string() + u8"_weapons.txt");
}

// Lines: slot hero current name ammo (current 1 for the one in hand).
void load_file() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(file_path());
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        int slot = -1, hero = -1, current = 0, ammo = 0;
        std::string name;
        if (!(s >> slot >> hero >> current >> name >> ammo)) continue;
        if (slot < 0 || slot >= SAVE_SLOTS || hero < 0 || hero >= HEROES) continue;
        g_disk[slot][hero].ammo[name] = ammo;
        if (current) g_disk[slot][hero].current = name;
    }
}

void write_file() {
    std::ofstream out(file_path(), std::ios::trunc);
    for (int slot = 0; slot < SAVE_SLOTS; slot++) {
        for (int hero = 0; hero < HEROES; hero++) {
            const SavedHero& s = g_disk[slot][hero];
            for (const auto& [name, ammo] : s.ammo) {
                out << slot << ' ' << hero << ' ' << (name == s.current ? 1 : 0) << ' ' << name << ' ' << ammo << '\n';
            }
        }
    }
}

int find(const std::string& name) {
    for (size_t i = 0; i < g_weapons.size(); i++) if (g_weapons[i].name == name) return CARRIER + (int)i;
    for (int i = 0; i < STOCK; i++) if (name == STOCK_NAMES[i]) return i;
    return -1;
}

// What a file has, by name, into the heroes (weapons whose mod is gone are kept on disk, not given).
void heroes_from_disk(int slot) {
    for (int hero = 0; hero < HEROES; hero++) {
        Hero& h = g_heroes[hero];
        h = Hero{};
        if (slot < 0 || slot >= SAVE_SLOTS) continue;
        for (const auto& [name, ammo] : g_disk[slot][hero].ammo) {
            const int id = find(name);
            if (id >= CARRIER) h.ammo[id] = ammo;
            if (id >= CARRIER && name == g_disk[slot][hero].current) h.current = id;
        }
    }
}
} // namespace

// --- the hooks -------------------------------------------------------------------------------------------------------
void file_chosen() {
    load_file();
    const uint32_t game = rd32(ADDR_gameplay);
    heroes_from_disk(valid_ptr(game) ? Game::SaveSlot::get(game) : -1);
}

void on_save(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    load_file();
    for (int hero = 0; hero < HEROES; hero++) {
        const uint32_t ch = character_ptr(hero);
        Hero& h = g_heroes[hero];
        if (valid_ptr(ch) && hero == current_hero()) store_carrier(ch, h);
        SavedHero& s = g_disk[slot][hero];
        // Weapons of mods not loaded now stay saved.
        for (auto it = s.ammo.begin(); it != s.ammo.end();) it = find(it->first) >= CARRIER ? s.ammo.erase(it) : std::next(it);
        for (const auto& [id, ammo] : h.ammo) s.ammo[weapon_of(id)->name] = ammo;
        s.current = weapon_of(h.current) != nullptr ? weapon_of(h.current)->name : std::string();
    }
    write_file();
}

void forget(int slot) {
    load_file();
    for (int i = 0; i < SAVE_SLOTS; i++) {
        if (slot >= 0 && i != slot) continue;
        for (SavedHero& s : g_disk[i]) s = SavedHero{};
    }
    write_file();
}

// Slot 15's icon (front-end item 25) as the weapon in hand wants it.
void update_icon(recomp_context* ctx, int hero) {
    extern void freeFrontEndItem_call(recomp_context* ctx, int item);
    const uint32_t list = rd32(ADDR_FRONT_ITEMS);
    if (!valid_ptr(list)) return;
    const uint32_t at = list + 2 * CARRIER_ITEM;
    const uint16_t now = rd16(at);
    if (g_item_stock < 0) g_item_stock = now;
    const Weapon* w = hero >= 0 ? weapon_of(g_heroes[hero].current) : nullptr;
    const uint16_t want = w != nullptr && w->icon >= 0 ? (uint16_t)(ITEM_SPRITE | w->icon) : (uint16_t)g_item_stock;
    if (now == want) return;
    wr16(at, want);
    freeFrontEndItem_call(ctx, CARRIER_ITEM); // loaded again, as the new picture, where it is drawn next
}

// Start of each tick: slot 15's ammo back into the weapon in hand, then slot 15 as the carried weapons say.
void tick(recomp_context* ctx) {
    if (!in_game()) return;
    const int c = current_hero();
    update_icon(ctx, c);
    for (int hero = 0; hero < HEROES; hero++) {
        const uint32_t ch = character_ptr(hero);
        if (!valid_ptr(ch)) continue;
        Hero& h = g_heroes[hero];
        adopt(ch, h);
        if (h.ammo.empty()) continue;
        store_carrier(ch, h);
        if (hero == c) load_carrier(ch, h, false);
    }
}

// objSetupPlayers (each level load): this level's pickups are new objects; slot 15 shows the weapon in hand.
void level_load(uint8_t* rdram) {
    g_rdram = rdram;
    g_pickups.clear();
    for (Weapon& w : g_weapons) w.name_text = w.ammo_text = 0; // the heap the names were in may be gone with the level
    g_blank_text = 0;
    const int c = current_hero();
    const uint32_t ch = c >= 0 ? character_ptr(c) : 0;
    if (valid_ptr(ch)) {
        Hero& h = g_heroes[c];
        adopt(ch, h);
        load_carrier(ch, h, true);
    }
}

// The first player's ammo of a carried weapon (15 up), -1 if not owned or no such weapon.
int carried_ammo(int id) {
    const int hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(ch) || weapon_of(id) == nullptr) return -1;
    Hero& h = g_heroes[hero];
    adopt(ch, h);
    store_carrier(ch, h);
    const auto it = h.ammo.find(id);
    return it != h.ammo.end() ? it->second : -1;
}

// Sets it (owned weapons only).
void set_carried_ammo(int id, int ammo) {
    const int hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(ch) || weapon_of(id) == nullptr) return;
    Hero& h = g_heroes[hero];
    adopt(ch, h);
    store_carrier(ch, h);
    if (h.ammo.find(id) == h.ammo.end()) return;
    h.ammo[id] = std::clamp(ammo, 0, 0xFFFF);
    load_carrier(ch, h, false);
}

// The weapon a hero has in hand, carried weapons by their own id; -1 none.
int weapon_in_hand(uint32_t ch, int hero) {
    if (!valid_ptr(ch)) return -1;
    const int w = Character::CurrentWeapon::get(ch);
    if (w == CARRIER && hero >= 0 && hero < HEROES && g_heroes[hero].current >= CARRIER) return g_heroes[hero].current;
    return w < 16 ? w : -1;
}
} // namespace jfg::weapons

namespace {
using namespace jfg;
using namespace jfg::weapons;

// The next (step 1) or previous (-1) carried weapon after `from`, -1 when `from` is the last that way.
int step(const Hero& h, int from, int dir) {
    if (dir > 0) {
        const auto it = h.ammo.upper_bound(from);
        return it != h.ammo.end() ? it->first : -1;
    }
    auto it = h.ammo.lower_bound(from);
    if (it == h.ammo.begin()) return -1;
    return std::prev(it)->first;
}

void select_step(uint8_t* rdram, recomp_context* ctx, int dir, void (*orig)(uint8_t*, recomp_context*)) {
    g_rdram = rdram;
    const int hero = hero_of_player((uint32_t)ctx->r5);
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(ch) || g_heroes[hero].ammo.empty()) {
        orig(rdram, ctx);
        return;
    }
    Hero& h = g_heroes[hero];
    set_owned_bit(ch, true); // the all-weapons cheat writes the mask without bit 15 each tick
    const int before = Character::CurrentWeapon::get(ch);
    store_carrier(ch, h);
    const int next = before == CARRIER ? step(h, h.current, dir) : -1;
    orig(rdram, ctx);
    g_rdram = rdram;
    if (next >= 0) {
        Character::CurrentWeapon::set(ch, CARRIER); // the game's switch ran (sound, hand, selector); 15 stays in hand
        h.current = next;
    } else if (before != CARRIER && Character::CurrentWeapon::get(ch) == CARRIER) {
        h.current = dir > 0 ? h.ammo.begin()->first : h.ammo.rbegin()->first;
    }
    load_carrier(ch, h, true);
}
} // namespace

JFG_WRAP(controlSelectNextWeapon) { select_step(rdram, ctx, 1, controlSelectNextWeapon_orig); }
JFG_WRAP(controlSelectPreviousWeapon) { select_step(rdram, ctx, -1, controlSelectPreviousWeapon_orig); }

extern "C" void func_overlay_18_00001194_1F30B6C(uint8_t* rdram, recomp_context* ctx); // show weapon w
extern "C" void func_overlay_18_000011E4_1F30BBC(uint8_t* rdram, recomp_context* ctx); // hide the one shown
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom);                             // jfg_glue.cpp
extern "C" void freeFrontEndItem(uint8_t* rdram, recomp_context* ctx);

namespace jfg::weapons {
void freeFrontEndItem_call(recomp_context* ctx, int item) { call_game(freeFrontEndItem, ctx, { (uint32_t)item }); }

// A carried weapon id (15 up) the registry knows.
bool known(int id) { return weapon_of(id) != nullptr; }

// Draws a weapon's big icon centred at x, y (overlay units) for a mod's jfg_hud_weapon_icon; false if it has none.
bool draw_big_icon(int id, float x, float y, float scale, float alpha) {
    const Weapon* w = weapon_of(id);
    if (w == nullptr || w->big_w <= 0) return false;
    const float bw = BIG_ICON_WIDTH * scale, bh = bw * (float)w->big_h / (float)w->big_w;
    const int a = (int)std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    jfg::overlay_draw(BIG_ICON_IMAGE + id, x - bw * 0.5f, y - bh * 0.5f, bw, bh, 0xFFFFFF00u | (uint32_t)a);
    return true;
}
}

namespace {
// The weapon screen's next (1) / previous (-1) over all 16 slots by the owned mask (owner: the MultiCharacter).
void screen_step(uint8_t* rdram, recomp_context* ctx, int dir, void (*orig)(uint8_t*, recomp_context*)) {
    g_rdram = rdram;
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_18_ROM), owner = (uint32_t)ctx->r4;
    if (base == 0 || !valid_ptr(owner) || !(Character::WeaponsOwned::get(owner) & (1u << CARRIER))) {
        orig(rdram, ctx);
        return;
    }
    const uint16_t owned = Character::WeaponsOwned::get(owner);
    const int cur = (int32_t)rd32(base + SHOWN_WEAPON);
    int w = cur;
    for (int i = 0; i < 16; i++) {
        w = (w + dir + 16) % 16;
        if (owned & (1u << w)) break;
    }
    if (w == cur) return;
    call_game(func_overlay_18_000011E4_1F30BBC, ctx, {});
    call_game(func_overlay_18_00001194_1F30B6C, ctx, { (uint32_t)w });
}
} // namespace

JFG_WRAP(func_overlay_18_00001218_1F30BF0) { screen_step(rdram, ctx, 1, func_overlay_18_00001218_1F30BF0_orig); }
JFG_WRAP(func_overlay_18_00001294_1F30C6C) { screen_step(rdram, ctx, -1, func_overlay_18_00001294_1F30C6C_orig); }

// The fire code (obj, player): jfg_on_weapon_fire, then a carried weapon fires as its host. JfgWeaponFire: s32 weapon,
// character; void* hero; s32 charge, cancel, take_ammo.
JFG_WRAP(func_800386B0_392B0) {
    const int hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(ch)) {
        func_800386B0_392B0_orig(rdram, ctx);
        return;
    }
    const uint32_t player = (uint32_t)ctx->r5;
    const int in_hand = weapon_in_hand(ch, hero);
    if (api::listened(api::Event::WeaponFire)) {
        const gpr a0 = ctx->r4, a1 = ctx->r5;
        int32_t cancel, take;
        {
            api::Payload p(ctx, 0x18);
            wr32(p.addr + 0x00, (uint32_t)in_hand);
            wr32(p.addr + 0x04, (uint32_t)hero);
            wr32(p.addr + 0x08, (uint32_t)a0);
            wr32(p.addr + 0x0C, (uint32_t)(int32_t)PlayerData::Charge::get(player));
            wr32(p.addr + 0x10, 0);
            wr32(p.addr + 0x14, 1);
            api::fire(rdram, ctx, api::Event::WeaponFire, (int32_t)p.addr);
            g_rdram = rdram;
            cancel = (int32_t)rd32(p.addr + 0x10);
            take = (int32_t)rd32(p.addr + 0x14);
        }
        ctx->r4 = a0;
        ctx->r5 = a1;
        if (cancel != 0) {
            const int slot = Character::CurrentWeapon::get(ch);
            if (take != 0 && slot < 16 && Character::Ammo::get(ch, (uint32_t)slot) > 0 && slot != 0) {
                Character::Ammo::set(ch, (uint32_t)slot, (uint16_t)(Character::Ammo::get(ch, (uint32_t)slot) - 1));
            }
            return;
        }
    }
    const Weapon* w = Character::CurrentWeapon::get(ch) == CARRIER ? weapon_of(in_hand) : nullptr;
    if (w == nullptr || w->host < 0) {
        func_800386B0_392B0_orig(rdram, ctx);
        return;
    }
    const uint32_t as = (uint32_t)w->host;
    const uint32_t game = rd32(ADDR_gameplay);
    const uint16_t as_ammo = Character::Ammo::get(ch, as);
    const uint32_t as_shots = Game::WeaponShots::get(game, as);
    const int16_t charge = PlayerData::Charge::get(player);
    Character::Ammo::set(ch, as, Character::Ammo::get(ch, CARRIER));
    Character::CurrentWeapon::set(ch, (uint8_t)as);
    if (w->charge >= 0) PlayerData::Charge::set(player, (int16_t)w->charge);
    func_800386B0_392B0_orig(rdram, ctx);
    g_rdram = rdram;
    Character::Ammo::set(ch, CARRIER, Character::Ammo::get(ch, as));
    Character::Ammo::set(ch, as, as_ammo);
    Character::CurrentWeapon::set(ch, CARRIER);
    PlayerData::Charge::set(player, charge);
    const uint32_t shots = Game::WeaponShots::get(game, as);
    if (shots != as_shots) {
        Game::WeaponShots::set(game, CARRIER, Game::WeaponShots::get(game, CARRIER) + (shots - as_shots));
        Game::WeaponShots::set(game, as, as_shots);
    }
}

JFG_WRAP(controlThrowWeapon) {
    const int weapon = (int32_t)ctx->r4;
    if (weapon == CARRIER) {
        const int hero = current_hero();
        const Weapon* w = hero >= 0 ? weapon_of(g_heroes[hero].current) : nullptr;
        if (w == nullptr) w = weapon_of(CARRIER);
        if (w->thrown) {
            ctx->r2 = 1;
            return;
        }
        if (w->host >= 0) ctx->r4 = (gpr)w->host; // as its host throws (or not)
    }
    controlThrowWeapon_orig(rdram, ctx);
}

// A pickup of slot 15 gives the weapon it was pointed at: slot 15 shows that weapon for the call.
JFG_WRAP(weaponPickupControl) {
    const uint32_t obj = (uint32_t)ctx->r4;
    const int hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(obj) || !valid_ptr(ch) || (int16_t)rd16(obj + PICKUP_WEAPON) != CARRIER) {
        weaponPickupControl_orig(rdram, ctx);
        return;
    }
    const auto found = g_pickups.find(obj);
    const int id = found != g_pickups.end() ? found->second : CARRIER;
    Hero& h = g_heroes[hero];
    adopt(ch, h);
    store_carrier(ch, h);
    const bool had = h.ammo.find(id) != h.ammo.end();
    set_owned_bit(ch, had);
    show(ch, id, had ? h.ammo[id] : 0, true);
    weaponPickupControl_orig(rdram, ctx);
    g_rdram = rdram;
    const int ammo_after = Character::Ammo::get(ch, CARRIER);
    const bool taken = (Character::WeaponsOwned::get(ch) & (1u << CARRIER)) && (!had || ammo_after != h.ammo[id]);
    if (Character::WeaponsOwned::get(ch) & (1u << CARRIER)) {
        if (!had && h.ammo.empty()) h.current = id;
        h.ammo[id] = ammo_after;
    }
    if (h.ammo.empty()) {
        set_owned_bit(ch, false);
        return;
    }
    set_owned_bit(ch, true);
    // Taken: the names stay the picked weapon's until the next switch (the message panel reads them as it draws).
    load_carrier(ch, h, !taken);
}

// --- the held model (decomp MSR 10e) ---------------------------------------------------------------------------------
// The hero's gun is a bare child object (hero +0x60 -> +0x04) with one model instance per slot (+0x6C); the draw
// (func_8000BE5C_CA5C) clamps slot 15 to 8. A weapon with a held model has the pack's model loaded per hero
// (modLoadModel, the flags the gun's own were loaded with) and put in slot 8 only for the draw's call, so nothing else
// ever sees it; it is freed (modFreeModel) just before the hero is (func_80007C68_8868 frees the gun's models). The
// barrel point (objMakeGunMtx: the model's joint point 0, a "barrel" node with jfgformats 0.7.1) is swapped the same
// way, so shots leave the held model's muzzle; a model without one leaves the barrel where it was.
extern "C" void modFreeModel(uint8_t* rdram, recomp_context* ctx);
extern "C" void modLoadModel(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000F978_10578(uint8_t* rdram, recomp_context* ctx);

namespace {
constexpr uint32_t OBJ_CHILDREN = 0x60, OBJ_MODELS = 0x6C, OBJ_DEF = 0x40, DEF_BEHAVIOUR = 0x1C, INST_POSE = 0x0A;
constexpr int GUN_SLOT = 8;
struct Held { uint32_t hero = 0, inst = 0; int model = -1; };
std::vector<Held> g_held;

uint32_t gun_of(uint32_t hero) {
    const uint32_t children = valid_ptr(hero) ? rd32(hero + OBJ_CHILDREN) : 0;
    return valid_ptr(children) ? rd32(children + 4) : 0;
}

void free_held(recomp_context* ctx, Held& h) {
    if (h.inst != 0) call_game(modFreeModel, ctx, { h.inst });
    h.inst = 0;
}

// The pack's instance of `model` for this hero's gun (loaded when first wanted), 0 if it can't be.
uint32_t held_instance(recomp_context* ctx, uint32_t hero, uint32_t gun, int model) {
    Held* h = nullptr;
    for (Held& e : g_held) if (e.hero == hero) h = &e;
    if (h == nullptr) { g_held.push_back({ hero, 0, -1 }); h = &g_held.back(); }
    if (h->inst != 0 && h->model == model) return h->inst;
    free_held(ctx, *h);
    const uint32_t def = rd32(gun + OBJ_DEF);
    if (!valid_ptr(def)) return 0;
    const uint32_t flags = call_game(func_8000F978_10578, ctx, { (uint32_t)(int32_t)(int16_t)rd16(def + DEF_BEHAVIOUR) }) & 7;
    const uint32_t inst = call_game(modLoadModel, ctx, { (uint32_t)model, flags });
    h->inst = valid_ptr(inst) ? inst : 0;
    h->model = model;
    return h->inst;
}
} // namespace

// Runs the gun's draw or its barrel matrix (both: obj, modInst, model, slot unclamped, ...) with the held model in
// slot 8 when the first player's hero holds a modded weapon that has one.
static void with_held(uint8_t* rdram, recomp_context* ctx, void (*orig)(uint8_t*, recomp_context*), bool draw) {
    const uint32_t hero = (uint32_t)ctx->r4;
    const int slot = (int32_t)ctx->r7;
    const int c = current_hero();
    const uint32_t ch = c >= 0 ? character_ptr(c) : 0;
    const Weapon* w = slot == CARRIER && valid_ptr(ch) && valid_ptr(hero) && Obj::Kind::get(hero) == 1 &&
                              hero == g_cam.player ? weapon_of(weapon_in_hand(ch, c)) : nullptr;
    const uint32_t gun = w != nullptr && w->held_model >= 0 ? gun_of(hero) : 0;
    const uint32_t models = valid_ptr(gun) ? rd32(gun + OBJ_MODELS) : 0;
    if (!valid_ptr(models)) {
        orig(rdram, ctx);
        return;
    }
    const gpr a[4] = { ctx->r4, ctx->r5, ctx->r6, ctx->r7 };
    const uint32_t inst = held_instance(ctx, hero, gun, w->held_model);
    g_rdram = rdram;
    ctx->r4 = a[0]; ctx->r5 = a[1]; ctx->r6 = a[2]; ctx->r7 = a[3];
    if (inst == 0) {
        orig(rdram, ctx);
        return;
    }
    const uint32_t stock = rd32(models + 4 * GUN_SLOT);
    wr32(models + 4 * GUN_SLOT, inst);
    if (draw) wr8(inst + INST_POSE, 1); // lit each frame (the game marks only the stock instance)
    orig(rdram, ctx);
    g_rdram = rdram;
    wr32(models + 4 * GUN_SLOT, stock);
}

// The gun's draw (obj, modInst, model, slot unclamped, mf).
JFG_WRAP(func_8000BE5C_CA5C) { with_held(rdram, ctx, func_8000BE5C_CA5C_orig, true); }
// The barrel's world position from the hand joint (obj, modInst, model, slot unclamped, mtx, &x, &y, &z).
JFG_WRAP(objMakeGunMtx) { with_held(rdram, ctx, objMakeGunMtx_orig, false); }

// An object's final free (obj, ...): a hero's held model goes first.
JFG_WRAP(func_80007C68_8868) {
    const uint32_t obj = (uint32_t)ctx->r4;
    for (size_t i = 0; i < g_held.size(); i++) {
        if (g_held[i].hero != obj) continue;
        const gpr a0 = ctx->r4, a1 = ctx->r5;
        free_held(ctx, g_held[i]);
        g_rdram = rdram;
        ctx->r4 = a0; ctx->r5 = a1;
        g_held.erase(g_held.begin() + (long)i);
        break;
    }
    func_80007C68_8868_orig(rdram, ctx);
}

// --- the helpers mods import -------------------------------------------------------------------------------------------
namespace {
// jfg_weapon_register(const JfgWeaponDef* def): { const char* name, *display, *ammo_display; s32 host, charge, thrown,
// row_from, max_ammo, start_ammo, pickup_ammo; }. The weapon's id (16 up; the same id again for a name already
// registered), -1 if the definition is bad.
void api_weapon_register(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t d = (uint32_t)ctx->r4;
    ctx->r2 = (gpr)-1;
    if (!mod_ptr(d)) return;
    Weapon w;
    w.name = read_string(rd32(d + 0x00));
    w.display = read_string(rd32(d + 0x04), 40);
    w.ammo_display = read_string(rd32(d + 0x08), 40);
    w.host = (int32_t)rd32(d + 0x0C);
    w.charge = (int32_t)rd32(d + 0x10);
    w.thrown = rd32(d + 0x14) != 0;
    w.row_from = (int32_t)rd32(d + 0x18);
    w.max_ammo = (int32_t)rd32(d + 0x1C);
    w.start_ammo = (int32_t)rd32(d + 0x20);
    w.pickup_ammo = (int32_t)rd32(d + 0x24);
    if (w.name.empty() || w.name.find(':') == std::string::npos || w.name.find(' ') != std::string::npos) return;
    if (w.host < -1 || w.host >= STOCK || w.row_from < 0 || w.row_from >= STOCK) return;
    if (w.display.empty()) w.display = w.name;
    if (w.ammo_display.empty()) w.ammo_display = w.display;
    const int existing = find(w.name);
    if (existing >= CARRIER) {
        ctx->r2 = (gpr)existing;
        return;
    }
    if (existing >= 0) return; // a stock name
    g_weapons.push_back(std::move(w));
    const int id = CARRIER + (int)g_weapons.size() - 1;
    std::fprintf(stderr, "[jfg] weapons: %s is weapon %d\n", g_weapons.back().name.c_str(), id);
    // A file chosen before the mod registered: its saved ammo of this weapon is given now.
    const uint32_t game = rd32(ADDR_gameplay);
    const int slot = valid_ptr(game) && auto_save_file_in_play() ? Game::SaveSlot::get(game) : -1;
    if (slot >= 0 && slot < SAVE_SLOTS) {
        load_file();
        for (int hero = 0; hero < HEROES; hero++) {
            const SavedHero& s = g_disk[slot][hero];
            const auto it = s.ammo.find(g_weapons.back().name);
            if (it == s.ammo.end() || g_heroes[hero].ammo.count(id)) continue;
            g_heroes[hero].ammo[id] = it->second;
            if (s.current == it->first) g_heroes[hero].current = id;
        }
    }
    ctx->r2 = (gpr)id;
}

// jfg_weapon_set_icon(int id, int sprite): a modded weapon's HUD icon is that 2D sprite (a pack's: its number from
// jfg_asset_number), drawn where the game draws slot 15's; -1 gives slot 15's own back.
void api_weapon_set_icon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    Weapon* w = const_cast<Weapon*>(weapon_of((int32_t)ctx->r4));
    const int sprite = (int32_t)ctx->r5;
    if (w != nullptr) w->icon = sprite >= 0 && sprite < 0x4000 ? sprite : -1;
}

// jfg_weapon_set_held_model(int id, int model): a modded weapon's model in the hero's hand, a pack's model (its number
// from jfg_asset_number, e.g. "mod:model_gun"): static (no joints), drawn on the hand where the game draws slot 8's
// (the gun's own size and axes); -1 gives slot 8's back.
void api_weapon_set_held_model(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    Weapon* w = const_cast<Weapon*>(weapon_of((int32_t)ctx->r4));
    const int model = (int32_t)ctx->r5;
    if (w != nullptr && w->name.rfind("stock:", 0) != 0) w->held_model = model >= 0 && model < 0x10000 ? model : -1;
}

// jfg_weapon_set_icon_image(int id, int w, int h, const unsigned int* rgba): a modded weapon's big icon, w x h words
// 0xRRGGBBAA (at most 512 x 512), drawn sharp where mods draw the weapon's icon (jfg_hud_weapon_icon).
void api_weapon_set_icon_image(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4, w = (int32_t)ctx->r5, h = (int32_t)ctx->r6;
    const uint32_t src = (uint32_t)ctx->r7;
    Weapon* weapon = const_cast<Weapon*>(weapon_of(id));
    if (weapon == nullptr || w <= 0 || h <= 0 || w > 512 || h > 512 || !mod_ptr(src)) return;
    std::vector<uint32_t> pixels((size_t)w * h);
    for (size_t i = 0; i < pixels.size(); i++) pixels[i] = rd32(src + (uint32_t)i * 4);
    jfg::overlay_set_image(BIG_ICON_IMAGE + id, w, h, std::move(pixels));
    weapon->big_w = w;
    weapon->big_h = h;
}

// jfg_weapon_find(const char* name): the id of a weapon by name ("stock:pistol" .. "stock:cluster_bombs",
// "port:proximity_mine", mods' "mod_id:weapon"), -1 if none.
void api_weapon_find(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    ctx->r2 = (gpr)find(read_string((uint32_t)ctx->r4));
}

// jfg_weapon_count(void): how many weapon ids there are: 0 .. count - 1 are all weapons.
void api_weapon_count(uint8_t*, recomp_context* ctx) {
    ctx->r2 = (gpr)(CARRIER + (int)g_weapons.size());
}

// jfg_weapon_info(int id, JfgWeaponInfo* out): { char name[48]; char display[40]; s32 modded, host, thrown, max_ammo; }.
// 1, or 0 for no such weapon.
void api_weapon_info(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4;
    const uint32_t out = (uint32_t)ctx->r5;
    ctx->r2 = 0;
    if (!mod_ptr(out) || id < 0 || id >= CARRIER + (int)g_weapons.size()) return;
    std::string name, display;
    int modded = 0, host = id, thrown = 0, max_ammo = 0;
    if (id < STOCK) {
        name = STOCK_NAMES[id];
        const uint32_t table = rd32(ADDR_front_text);
        if (valid_ptr(table)) display = read_string(rd32(table + 4 * (uint32_t)(WEAPON_NAMES + id)), 39);
        max_ammo = (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)id + ROW_MAX_AMMO);
        thrown = -1; // the game's own rule
    } else {
        const Weapon* w = weapon_of(id);
        name = w->name;
        display = w->display;
        modded = id >= FIRST_MOD;
        host = w->host;
        thrown = w->thrown;
        max_ammo = w->max_ammo >= 0 ? w->max_ammo
                                    : (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)w->row_from + ROW_MAX_AMMO);
    }
    for (uint32_t i = 0; i < 48; i++) wr8(out + i, i < name.size() && i < 47 ? (uint8_t)name[i] : 0);
    for (uint32_t i = 0; i < 40; i++) wr8(out + 48 + i, i < display.size() && i < 39 ? (uint8_t)display[i] : 0);
    wr32(out + 88, (uint32_t)modded);
    wr32(out + 92, (uint32_t)host);
    wr32(out + 96, (uint32_t)thrown);
    wr32(out + 100, (uint32_t)max_ammo);
    ctx->r2 = 1;
}

// jfg_hero_has_weapon(int id): 1 if the first player's hero owns it.
void api_hero_has_weapon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4, hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    ctx->r2 = 0;
    if (!valid_ptr(ch)) return;
    if (id >= 0 && id < STOCK) ctx->r2 = (Character::WeaponsOwned::get(ch) >> id) & 1;
    else if (weapon_of(id) != nullptr) ctx->r2 = g_heroes[hero].ammo.count(id) ? 1 : 0;
}

// jfg_hero_give_weapon(int id, int ammo): the first player's hero owns it, with that ammo (-1: its start ammo; owned
// already: added, up to the capacity).
void api_hero_give_weapon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4, ammo = (int32_t)ctx->r5, hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(ch)) return;
    int row = id, max = 0, start = 0;
    const Weapon* w = weapon_of(id);
    if (id >= 0 && id < STOCK) {
        max = (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)row + ROW_MAX_AMMO);
        start = (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)row + ROW_START_AMMO);
        const uint16_t mask = Character::WeaponsOwned::get(ch);
        const int add = ammo >= 0 ? ammo : start;
        if (!(mask & (1u << id))) {
            Character::WeaponsOwned::set(ch, (uint16_t)(mask | (1u << id)));
            Character::WeaponCount::set(ch, (uint8_t)popcount16((uint16_t)(mask | (1u << id))));
            wr16(ch + 0x34 + 2 * (uint32_t)id, (uint16_t)max);
            Character::MaxCharge::set(ch, (uint32_t)id, rd8(ADDR_weaponTable + WeaponDef::Size * (uint32_t)id + ROW_CHARGE));
            Character::Ammo::set(ch, (uint32_t)id, (uint16_t)std::clamp(add, 0, max));
        } else {
            Character::Ammo::set(ch, (uint32_t)id, (uint16_t)std::clamp(Character::Ammo::get(ch, (uint32_t)id) + add, 0, max));
        }
        return;
    }
    if (w == nullptr) return;
    row = w->row_from;
    max = w->max_ammo >= 0 ? w->max_ammo : (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)row + ROW_MAX_AMMO);
    start = w->start_ammo >= 0 ? w->start_ammo : (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)row + ROW_START_AMMO);
    Hero& h = g_heroes[hero];
    adopt(ch, h);
    store_carrier(ch, h);
    const int add = ammo >= 0 ? ammo : start;
    const auto it = h.ammo.find(id);
    h.ammo[id] = std::clamp((it != h.ammo.end() ? it->second : 0) + add, 0, max);
    load_carrier(ch, h, true);
}

// jfg_hero_take_weapon(int id): the first player's hero no longer owns it (not the pistol). If it was in hand the hero
// switches to the next owned weapon.
void api_hero_take_weapon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4, hero = current_hero();
    const uint32_t ch = hero >= 0 ? character_ptr(hero) : 0;
    if (!valid_ptr(ch) || id <= 0) return;
    uint16_t mask = Character::WeaponsOwned::get(ch);
    if (id < STOCK) {
        mask = (uint16_t)(mask & ~(1u << id));
        Character::WeaponsOwned::set(ch, mask);
        Character::WeaponCount::set(ch, (uint8_t)popcount16(mask));
        Character::Ammo::set(ch, (uint32_t)id, 0);
        if (Character::CurrentWeapon::get(ch) == id) Character::CurrentWeapon::set(ch, 0);
        return;
    }
    Hero& h = g_heroes[hero];
    adopt(ch, h);
    store_carrier(ch, h);
    if (!h.ammo.erase(id)) return;
    if (h.ammo.empty()) {
        set_owned_bit(ch, false);
        Character::Ammo::set(ch, CARRIER, 0);
        if (Character::CurrentWeapon::get(ch) == CARRIER) Character::CurrentWeapon::set(ch, 0);
        h.current = -1;
        return;
    }
    load_carrier(ch, h, true);
}

// jfg_weapon_pickup_set(void* pickup, int id): a weapon pickup (kind JFG_KIND_WEAPON) gives that weapon from now on.
// Its model stays the one it has.
void api_weapon_pickup_set(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const int id = (int32_t)ctx->r5;
    if (!valid_ptr(obj) || id < 0 || id >= CARRIER + (int)g_weapons.size()) return;
    if (id < STOCK) {
        g_pickups.erase(obj);
        wr16(obj + PICKUP_WEAPON, (uint16_t)id);
    } else {
        g_pickups[obj] = id;
        wr16(obj + PICKUP_WEAPON, CARRIER);
    }
}

// jfg_weapon_pickup_get(void* pickup): the weapon a weapon pickup gives.
void api_weapon_pickup_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    ctx->r2 = (gpr)-1;
    if (!valid_ptr(obj)) return;
    const int w = (int16_t)rd16(obj + PICKUP_WEAPON);
    const auto it = g_pickups.find(obj);
    ctx->r2 = (gpr)(w == CARRIER && it != g_pickups.end() ? it->second : w);
}
} // namespace

namespace jfg::weapons {
// For the port's own users (jfg_weapon_ammo.cpp): the exports, called from C++.
void give(uint8_t* rdram, recomp_context* ctx, int id, int ammo) {
    recomp_context c = *ctx;
    c.r4 = (gpr)id;
    c.r5 = (gpr)ammo;
    api_hero_give_weapon(rdram, &c);
}
bool owned(uint8_t* rdram, recomp_context* ctx, int id) {
    recomp_context c = *ctx;
    c.r4 = (gpr)id;
    api_hero_has_weapon(rdram, &c);
    return c.r2 != 0;
}
int capacity(int id) {
    if (id >= 0 && id < STOCK) return (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)id + ROW_MAX_AMMO);
    const Weapon* w = weapon_of(id);
    if (w == nullptr) return 0;
    return w->max_ammo >= 0 ? w->max_ammo : (int16_t)rd16(ADDR_weaponTable + WeaponDef::Size * (uint32_t)w->row_from + ROW_MAX_AMMO);
}
int pickup_get(uint8_t* rdram, recomp_context* ctx, uint32_t obj) {
    recomp_context c = *ctx;
    c.r4 = (gpr)(int32_t)obj;
    api_weapon_pickup_get(rdram, &c);
    return (int)(int32_t)c.r2;
}
void pickup_set(uint8_t* rdram, recomp_context* ctx, uint32_t obj, int id) {
    recomp_context c = *ctx;
    c.r4 = (gpr)(int32_t)obj;
    c.r5 = (gpr)id;
    api_weapon_pickup_set(rdram, &c);
}
} // namespace jfg::weapons

namespace jfg {
void register_weapon_registry_api() {
    recomp::overlays::register_base_export("jfg_weapon_register", api_weapon_register);
    recomp::overlays::register_base_export("jfg_weapon_find", api_weapon_find);
    recomp::overlays::register_base_export("jfg_weapon_set_icon", api_weapon_set_icon);
    recomp::overlays::register_base_export("jfg_weapon_set_icon_image", api_weapon_set_icon_image);
    recomp::overlays::register_base_export("jfg_weapon_set_held_model", api_weapon_set_held_model);
    recomp::overlays::register_base_export("jfg_weapon_count", api_weapon_count);
    recomp::overlays::register_base_export("jfg_weapon_info", api_weapon_info);
    recomp::overlays::register_base_export("jfg_hero_has_weapon", api_hero_has_weapon);
    recomp::overlays::register_base_export("jfg_hero_give_weapon", api_hero_give_weapon);
    recomp::overlays::register_base_export("jfg_hero_take_weapon", api_hero_take_weapon);
    recomp::overlays::register_base_export("jfg_weapon_pickup_set", api_weapon_pickup_set);
    recomp::overlays::register_base_export("jfg_weapon_pickup_get", api_weapon_pickup_get);
}
} // namespace jfg
