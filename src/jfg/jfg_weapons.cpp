// Jet Force Gemini port: the Weapons tab - per-weapon stats.
//
// The player's weapons are described by weaponTable (0x800A1490, 0x30 bytes per weapon, indexed by the weapon slot;
// controlGetWeaponDef), read when a shot is fired (controlUpdateWeapon -> func_800386B0, which hands the values to the
// weapon's spawn function). Fields used here:
//   +0x03 u8   damage (passed to the projectile; laser bolts: laserbolt data+0x61 and, as in the original, the bolt's
//              model/size class); +0x04 u8 the same value in the table, kept equal
//   +0x05 u8   frames between shots (data+0x538 countdown, decremented by the frames each tick simulates)
//   +0x06 s8   > 0: frames from the button press to the release of a thrown weapon (data+0x53D); -1 charge, -2 beam
//   +0x07 u8   projectile lifetime (laser bolts live lifetime*6 frames)
//   +0x08 s16, +0x0A s16  random spread added to the aim, sideways / up-down (angle units, 65536 = full turn)
//   +0x10 f32  projectile / throw speed
// Other per-weapon behaviour:
//   plasma shotgun charge: data+0x53E grows by 2x the frames while the button is held, up to character+0x57 << 5;
//     a faster charge adds the extra growth at the start of each tick (the game clamps it to the maximum itself).
//   mines: the player's mines are recorded when created (hook at the end of mineInit); beyond the limit the oldest is
//     removed the way mineRemove does it (mine data+0x01 = 6, +0xAC = 0). (The game's own list at 0x800F2D20, up to 8
//     with the oldest dropped, only holds some mine types.)
//   pistol: rapid presses build "heat" (data+0x542, +0x2A per shot, no shot at >= 0xD3); "no rapid-fire limit" clears it.
//   laser bolt size: laserboltInit's scale (bolt data+0x6C) for bolts fired by the player (hook at its end).
// When a weapon's "Modify" box is off its original values are written back.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

namespace {
constexpr uint32_t ADDR_weaponTable = 0x800A1490, WEAPON_DEF_SIZE = 0x30;
constexpr int WEAPON_COUNT = 15;
constexpr const char* WEAPONS_FILE = "jfg_weapons.txt";

enum WeaponField : uint32_t {
    WF_DAMAGE = 1, WF_DELAY = 2, WF_SPEED = 4, WF_LIFE = 8, WF_SPREAD = 16, WF_THROW = 32, WF_SIZE = 64,
    WF_CHARGE = 128, WF_MINES = 256, WF_HEAT = 512,
};

struct WeaponInfo { const char* name; uint32_t fields; bool bolt; };
const WeaponInfo WEAPON_INFO[WEAPON_COUNT] = {
    { "Pistol",              WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_SIZE | WF_HEAT, true },
    { "Homing Missiles",     WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD, false },
    { "Machine Gun",         WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_SIZE, true },
    { "Plasma Shotgun",      WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_CHARGE, false },
    { "Shocker",             WF_DELAY | WF_SPREAD, false },
    { "Tri-Rocket Launcher", WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD, false },
    { "Flamethrower",        WF_SPREAD, false },
    { "Sniper Rifle",        WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_SIZE, true },
    { "Grenades",            WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Shuriken",            WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Fish Food",           WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Timed Mines",         WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_MINES, false },
    { "Remote Mines",        WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW | WF_MINES, false },
    { "Flares",              WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Cluster Bombs",       WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
};

struct WeaponStats {
    int damage = 0, delay = 0, throw_delay = 0, life = 0, spread_x = 0, spread_y = 0;
    float speed = 0;
};

struct WeaponMod {
    bool on = false;
    WeaponStats s;
    int size = 100;          // laser bolt size, percent
    int charge = 100;        // plasma charge speed, percent
    int mines = 8;           // mines on the field (shared by both mine types)
    bool no_heat = false;    // pistol: no rapid-fire limit
};

WeaponStats g_weapon_orig[WEAPON_COUNT];
WeaponMod g_weapon_mod[WEAPON_COUNT];
bool g_weapons_loaded = false;
std::vector<std::pair<uint32_t, uint32_t>> g_player_mines; // the player's mines (object, data), oldest first

WeaponStats read_weapon_stats(int w) {
    const uint32_t d = ADDR_weaponTable + WEAPON_DEF_SIZE * (uint32_t)w;
    WeaponStats s;
    s.damage = rd8(d + 3); s.delay = rd8(d + 5); s.throw_delay = (int8_t)rd8(d + 6); s.life = rd8(d + 7);
    s.spread_x = (int16_t)rd16(d + 8); s.spread_y = (int16_t)rd16(d + 0xA); s.speed = rdf(d + 0x10);
    return s;
}

void write_weapon_stats(int w, const WeaponStats& s, const WeaponStats& orig) {
    const uint32_t d = ADDR_weaponTable + WEAPON_DEF_SIZE * (uint32_t)w;
    const uint32_t f = WEAPON_INFO[w].fields;
    if (f & WF_DAMAGE) { wr8(d + 3, (uint8_t)std::clamp(s.damage, 0, 255)); wr8(d + 4, (uint8_t)std::clamp(s.damage, 0, 255)); }
    if (f & WF_DELAY) wr8(d + 5, (uint8_t)std::clamp(s.delay, 0, 255));
    if ((f & WF_THROW) && orig.throw_delay > 0) wr8(d + 6, (uint8_t)std::clamp(s.throw_delay, 1, 127));
    if (f & WF_LIFE) wr8(d + 7, (uint8_t)std::clamp(s.life, 1, 255));
    if (f & WF_SPREAD) { wr16(d + 8, (uint16_t)std::clamp(s.spread_x, 0, 8192)); wr16(d + 0xA, (uint16_t)std::clamp(s.spread_y, 0, 8192)); }
    if (f & WF_SPEED) wrf(d + 0x10, std::clamp(s.speed, 0.1f, 2000.0f));
}

void restore_weapon_stats(int w) {
    const uint32_t d = ADDR_weaponTable + WEAPON_DEF_SIZE * (uint32_t)w;
    const WeaponStats& o = g_weapon_orig[w];
    wr8(d + 3, (uint8_t)o.damage); wr8(d + 4, (uint8_t)o.damage); wr8(d + 5, (uint8_t)o.delay); wr8(d + 6, (uint8_t)o.throw_delay);
    wr8(d + 7, (uint8_t)o.life); wr16(d + 8, (uint16_t)o.spread_x); wr16(d + 0xA, (uint16_t)o.spread_y); wrf(d + 0x10, o.speed);
}
} // namespace

bool g_weapon_orig_read = false;
std::recursive_mutex g_weapon_mutex; // UI thread edits, game thread applies

// Sets one weapon value from its saved form "<slot>.<field>" (jfg_weapons.txt, profiles with "weapon." in front).
void apply_weapon_key(const char* key, int value) {
    int w = -1; char field[48] = {};
    if (std::sscanf(key, "%d.%47s", &w, field) != 2 || w < 0 || w >= WEAPON_COUNT) return;
    WeaponMod& m = g_weapon_mod[w];
    if (!std::strcmp(field, "modify")) m.on = value != 0;
    else if (!std::strcmp(field, "damage")) m.s.damage = std::clamp(value, 0, 255);
    else if (!std::strcmp(field, "delay")) m.s.delay = std::clamp(value, 0, 255);
    else if (!std::strcmp(field, "throw_delay")) m.s.throw_delay = std::clamp(value, 1, 127);
    else if (!std::strcmp(field, "lifetime")) m.s.life = std::clamp(value, 1, 255);
    else if (!std::strcmp(field, "spread_x")) m.s.spread_x = std::clamp(value, 0, 8192);
    else if (!std::strcmp(field, "spread_y")) m.s.spread_y = std::clamp(value, 0, 8192);
    else if (!std::strcmp(field, "speed_x100")) m.s.speed = std::clamp(value, 10, 200000) / 100.0f;
    else if (!std::strcmp(field, "size")) m.size = std::clamp(value, 10, 1000);
    else if (!std::strcmp(field, "charge")) m.charge = std::clamp(value, 10, 1000);
    else if (!std::strcmp(field, "mines")) m.mines = std::clamp(value, 1, 8);
    else if (!std::strcmp(field, "no_heat")) m.no_heat = value != 0;
}

// Every weapon back to its original values, "Modify" off.
void reset_weapons() {
    for (int w = 0; w < WEAPON_COUNT; w++) { g_weapon_mod[w] = WeaponMod(); g_weapon_mod[w].s = g_weapon_orig[w]; }
}

std::string weapons_text(const char* prefix) {
    std::string out;
    char buf[512];
    for (int w = 0; w < WEAPON_COUNT; w++) {
        const WeaponMod& m = g_weapon_mod[w];
        std::snprintf(buf, sizeof(buf), "%s%d.modify=%d\n%s%d.damage=%d\n%s%d.delay=%d\n%s%d.throw_delay=%d\n%s%d.lifetime=%d\n%s%d.spread_x=%d\n",
            prefix, w, m.on ? 1 : 0, prefix, w, m.s.damage, prefix, w, m.s.delay, prefix, w, m.s.throw_delay, prefix, w, m.s.life, prefix, w, m.s.spread_x);
        out += buf;
        std::snprintf(buf, sizeof(buf), "%s%d.spread_y=%d\n%s%d.speed_x100=%d\n%s%d.size=%d\n%s%d.charge=%d\n%s%d.mines=%d\n%s%d.no_heat=%d\n",
            prefix, w, m.s.spread_y, prefix, w, (int)std::lround(m.s.speed * 100.0f), prefix, w, m.size, prefix, w, m.charge, prefix, w, m.mines, prefix, w, m.no_heat ? 1 : 0);
        out += buf;
    }
    return out;
}

namespace {
void load_weapons() {
    if (g_weapons_loaded || !g_weapon_orig_read) return;
    g_weapons_loaded = true;
    reset_weapons();
    if (std::FILE* f = std::fopen(WEAPONS_FILE, "r")) {
        char key[64]; int value;
        while (std::fscanf(f, "%63[^=]=%d\n", key, &value) == 2) apply_weapon_key(key, value);
        std::fclose(f);
    }
}
} // namespace

void save_weapons() {
    if (std::FILE* f = std::fopen(WEAPONS_FILE, "w")) {
        std::fputs(weapons_text("").c_str(), f);
        std::fclose(f);
    }
    profile_autosave();
}

// Throwing. The character's control code starts its throw animation (Juno: 0x15 / 0x2E, Vela: 0x16 / 0x31, Lupus:
// 0x0E / 0x1B, the second one for the Shuriken) and locks the controls for 40 frames (data+0x577, counted down by the
// frames each tick simulates); the weapon leaves the hand after its throw delay. A throw delay set shorter than the
// original also plays the animation and runs the lock that much faster (factor = original / new delay, at most 8).
float g_throw_factor = 1.0f;
bool is_throw_anim(uint32_t obj) {
    const int anim = (int8_t)rd8(obj + 0x3B);
    switch ((int16_t)rd16(ADDR_currentCharacter)) {
    case 0: return anim == 0x16 || anim == 0x31; // Vela
    case 1: return anim == 0x15 || anim == 0x2E; // Juno
    case 2: return anim == 0x0E || anim == 0x1B; // Lupus
    default: return false;
    }
}

// Game thread, every tick (jfg_debug_frame).
void apply_weapons() {
    if (g_rdram == nullptr) return;
    if (!g_weapon_orig_read) {
        for (int w = 0; w < WEAPON_COUNT; w++) g_weapon_orig[w] = read_weapon_stats(w);
        g_weapon_orig_read = true;
    }
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    load_weapons();
    for (int w = 0; w < WEAPON_COUNT; w++) {
        if (g_weapon_mod[w].on) write_weapon_stats(w, g_weapon_mod[w].s, g_weapon_orig[w]);
        else restore_weapon_stats(w);
    }

    const uint32_t obj = g_cam.player;
    if (obj == 0 || rd32(ADDR_mainGameMode) != 0) return;
    const uint32_t data = rd32(obj + 0x68);
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
    if (!valid_ptr(data) || ch == 0) return;
    const int weapon = rd8(ch + CHAR_WEAPON_OFFSET);

    // Pistol: no rapid-fire limit.
    if (weapon == 0 && g_weapon_mod[0].on && g_weapon_mod[0].no_heat) wr8(data + 0x542, 0);

    // Throwing: faster animation and shorter control lock with a shorter throw delay (see g_throw_factor).
    g_throw_factor = 1.0f;
    if (weapon >= 0 && weapon < WEAPON_COUNT && (WEAPON_INFO[weapon].fields & WF_THROW) && g_weapon_mod[weapon].on) {
        const int o = g_weapon_orig[weapon].throw_delay, m = g_weapon_mod[weapon].s.throw_delay;
        if (o > 0 && m > 0 && m < o) g_throw_factor = std::min(8.0f, (float)o / (float)m);
    }
    if (g_throw_factor > 1.0f && is_throw_anim(obj)) {
        const int lock = (int8_t)rd8(data + 0x577);
        if (lock > 0) {
            const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
            const int extra = (int)std::lround((g_throw_factor - 1.0f) * frames);
            wr8(data + 0x577, (uint8_t)std::max(0, lock - extra));
        }
    }

    // Plasma shotgun: faster (or slower) charge.
    static int last_charge = 0;
    const int charge = (int16_t)rd16(data + 0x53E);
    if (weapon == 3 && g_weapon_mod[3].on && g_weapon_mod[3].charge != 100 && charge > last_charge) {
        const int grown = charge - last_charge;
        const int max_charge = (int)rd8(ch + 0x54 + 3) << 5;
        const int extra = grown * (g_weapon_mod[3].charge - 100) / 100;
        const int next = std::clamp(charge + extra, 0, std::max(max_charge, charge));
        wr16(data + 0x53E, (uint16_t)next);
        last_charge = next;
    }
    else last_charge = charge;

    // Mines on the field: the oldest of the player's mines beyond the limit is removed the way mineRemove does it.
    const bool mines_on = g_weapon_mod[11].on || g_weapon_mod[12].on;
    for (size_t i = 0; i < g_player_mines.size();) { // drop mines that are gone (object freed or reused, or already going)
        const uint32_t mo = g_player_mines[i].first, md = g_player_mines[i].second;
        const bool alive = valid_ptr(mo) && rd32(mo + 0x68) == md && rd8(md + 1) != 5 && rd8(md + 1) != 6;
        if (alive) i++; else g_player_mines.erase(g_player_mines.begin() + (long)i);
    }
    if (mines_on) {
        const int limit = g_weapon_mod[11].on ? g_weapon_mod[11].mines : g_weapon_mod[12].mines;
        while ((int)g_player_mines.size() > limit) {
            const uint32_t md = g_player_mines.front().second;
            wr8(md + 1, 6);   // what mineRemove does
            wr8(md + 0xAC, 0);
            g_player_mines.erase(g_player_mines.begin());
        }
    }
}

// Hook at the entry of objAnimDframe(object, speed, frames): the player's throw animation runs faster (see above), and
// the player's animations keep up with the ground while sprinting (jfg_camera_controls). Not the jump: it is timed to
// the jump's arc, and sped up it ran out early and left the character in its landing crouch in mid-air.
// The speed arrives as the bits of a float in a1 (o32: the first argument is not a float).
extern "C" uint32_t jfg_anim_speed(uint8_t* rdram, uint32_t obj, uint32_t speed_bits) {
    if ((g_throw_factor <= 1.0f && g_cam.sprint_factor == 1.0f) || obj == 0 || obj != g_cam.player) return speed_bits;
    g_rdram = rdram;
    const bool throwing = g_throw_factor > 1.0f && is_throw_anim(obj);
    if (!throwing) {
        const uint32_t data = rd32(obj + 0x68);
        if (g_cam.sprint_factor == 1.0f || !valid_ptr(data) || rd8(data + PD_STATE) == PD_STATE_JUMP) return speed_bits;
    }
    float speed; std::memcpy(&speed, &speed_bits, 4);
    speed *= throwing ? g_throw_factor : g_cam.sprint_factor;
    std::memcpy(&speed_bits, &speed, 4);
    return speed_bits;
}

// Hook at the end of mineInit (overlay 112): s1 = the new mine, s0 = its data. Mines placed while the player holds a
// mine weapon are the player's.
extern "C" void jfg_mine_init(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const uint32_t mine = (uint32_t)ctx->r17, md = (uint32_t)ctx->r16;
    const uint32_t md = valid_ptr(mine) ? rd32(mine + 0x68) : 0;
    if (!valid_ptr(mine) || !valid_ptr(md) || g_cam.player == 0) return;
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
    if (ch == 0) return;
    const int weapon = rd8(ch + CHAR_WEAPON_OFFSET);
    if (weapon != 11 && weapon != 12) return;
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    g_player_mines.emplace_back(mine, md);
}

// After laserboltInit (overlay 109; its wrapper): a0 = the new bolt. Scales bolts the player fires.
extern "C" void jfg_bolt_init(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const uint32_t bolt = (uint32_t)ctx->r4;
    if (!valid_ptr(bolt) || g_cam.player == 0) return;
    const uint32_t bd = rd32(bolt + 0x68);
    if (!valid_ptr(bd) || rd32(bd + 0x70) != g_cam.player) return;
    const int weapon = (int8_t)rd8(bd + 0x7C);
    static const bool log = std::getenv("JFG_DEBUG_BOLTS") != nullptr;
    if (log) std::fprintf(stderr, "[jfg] bolt: weapon %d damage %d speed %.1f life %d scale %.2f\n", weapon, rd8(bd + 0x61), rdf(bd + 0x64), (int)(int16_t)rd16(bd + 0x6A), rdf(bd + 0x6C));
    if (weapon < 0 || weapon >= WEAPON_COUNT || !WEAPON_INFO[weapon].bolt) return;
    const WeaponMod& m = g_weapon_mod[weapon];
    if (!m.on || m.size == 100) return;
    wrf(bd + 0x6C, rdf(bd + 0x6C) * (float)m.size / 100.0f);
}

namespace {
bool weapon_slider(const char* label, int* v, int lo, int hi, int def) {
    char fmt[48];
    std::snprintf(fmt, sizeof(fmt), "%%d   (default %d)", def);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 20.0f));
    return ImGui::SliderInt(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}
bool weapon_slider_f(const char* label, float* v, float lo, float hi, float def) {
    char fmt[48];
    std::snprintf(fmt, sizeof(fmt), "%%.1f   (default %.1f)", def);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 20.0f));
    return ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}
} // namespace

void draw_weapons() {
    if (!g_weapon_orig_read) { ImGui::TextDisabled("Not available yet."); return; }
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    ImGui::TextWrapped("Tick \"Modify\" to change a weapon; untick it and the weapon is back to its original values. "
                       "Changes apply from the next shot, to every character using that weapon, and are saved in "
                       "jfg_weapons.txt.");
    bool changed = false;
    for (int w = 0; w < WEAPON_COUNT; w++) {
        const WeaponInfo& info = WEAPON_INFO[w];
        const WeaponStats& o = g_weapon_orig[w];
        WeaponMod& m = g_weapon_mod[w];
        ImGui::PushID(w);
        ImGui::Separator();
        changed |= ImGui::Checkbox("Modify", &m.on);
        ImGui::SameLine();
        const bool open = ImGui::CollapsingHeader(info.name);
        if (open) {
            ImGui::Indent();
            ImGui::BeginDisabled(!m.on);
            const uint32_t f = info.fields;
            if (f & WF_DAMAGE) changed |= weapon_slider("Damage", &m.s.damage, 1, 100, o.damage);
            if (f & WF_DAMAGE && info.bolt) ImGui::TextDisabled("  (also sets the bolt's look, as in the game)");
            if (f & WF_DELAY) changed |= weapon_slider("Shot delay (frames)", &m.s.delay, 0, 120, o.delay);
            if (f & WF_DELAY) ImGui::TextDisabled("  (frames between shots; 60 frames = 1 second)");
            if ((f & WF_THROW) && o.throw_delay > 0) changed |= weapon_slider("Throw delay (frames)", &m.s.throw_delay, 1, 60, o.throw_delay);
            if ((f & WF_THROW) && o.throw_delay > 0) ImGui::TextDisabled("  (press to release; the game keeps at least about 4 ticks for the throw)");
            if (f & WF_SPEED) changed |= weapon_slider_f(info.bolt ? "Speed" : "Speed / throw", &m.s.speed, 1.0f, std::max(400.0f, o.speed * 4.0f), o.speed);
            if (f & WF_LIFE) changed |= weapon_slider(info.bolt ? "Lifetime (x0.1 s)" : "Lifetime / fuse", &m.s.life, 1, 255, o.life);
            if (f & WF_SPREAD) {
                changed |= weapon_slider("Spread sideways", &m.s.spread_x, 0, 4096, o.spread_x);
                changed |= weapon_slider("Spread up/down", &m.s.spread_y, 0, 4096, o.spread_y);
            }
            if (f & WF_SIZE) changed |= weapon_slider("Bolt size (%)", &m.size, 25, 400, 100);
            if (f & WF_CHARGE) changed |= weapon_slider("Charge speed (%)", &m.charge, 25, 800, 100);
            if (f & WF_MINES) {
                changed |= weapon_slider("Mines at once", &m.mines, 1, 8, 8);
                ImGui::TextDisabled("  (your mines of both kinds; the oldest is removed when another is placed)");
            }
            if (f & WF_HEAT) {
                changed |= ImGui::Checkbox("No rapid-fire limit", &m.no_heat);
                ImGui::SameLine(); ImGui::TextDisabled("(fast presses no longer slow it down)");
            }
            if (w == 6) ImGui::TextDisabled("  (its flames have their own code; only the spread is in the weapon table)");
            if (w == 4) ImGui::TextDisabled("  (its beam has its own code; damage and range are not in the weapon table)");
            ImGui::EndDisabled();
            if (ImGui::Button("Reset to default")) {
                const bool on = m.on;
                m = WeaponMod();
                m.on = on;
                m.s = o;
                changed = true;
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (changed) save_weapons();
}

// The same as the Weapons tab, as rows for F10's Weapons window (jfg_ui_tools.cpp).
std::vector<ToolField> weapon_fields() {
    using F = ToolField;
    std::vector<F> out;
    out.push_back({ F::Note, "", 0, 1, 1, 1, nullptr, nullptr, [](recomp_context*) {
        return std::string(g_weapon_orig_read ? "Turn \"Modify\" on to change a weapon; off puts it back to its original values. Changes apply "
                                                "from the next shot, to every character, and are saved in jfg_weapons.txt."
                                              : "Not available yet: start a game first.");
    } });
    // A value of weapon w: read and written under the weapon lock, saved on every change.
    auto number = [&](int w, const char* label, int lo, int hi, int step, int big, std::function<int&(WeaponMod&)> ref) {
        out.push_back({ F::Number, std::string("    ") + label, lo, hi, step, big,
            [w, ref](recomp_context*) { std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex); return ref(g_weapon_mod[w]); },
            [w, ref, lo, hi](recomp_context*, int v) {
                std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
                if (!g_weapon_orig_read || !g_weapon_mod[w].on) return;
                ref(g_weapon_mod[w]) = std::clamp(v, lo, hi);
                save_weapons();
            } });
    };
    for (int w = 0; w < WEAPON_COUNT; w++) {
        const WeaponInfo& info = WEAPON_INFO[w];
        const uint32_t f = info.fields;
        out.push_back({ F::Heading, info.name });
        out.push_back({ F::Toggle, "    Modify", 0, 1, 1, 1,
            [w](recomp_context*) { std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex); return g_weapon_mod[w].on ? 1 : 0; },
            [w](recomp_context*, int v) {
                std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
                if (!g_weapon_orig_read) return;
                g_weapon_mod[w].on = v != 0;
                save_weapons();
            } });
        if (f & WF_DAMAGE) number(w, "Damage", 1, 100, 1, 10, [](WeaponMod& m) -> int& { return m.s.damage; });
        if (f & WF_DELAY) number(w, "Shot delay (frames, 60 = 1 s)", 0, 120, 1, 10, [](WeaponMod& m) -> int& { return m.s.delay; });
        if (f & WF_THROW) number(w, "Throw delay (frames)", 1, 60, 1, 5, [](WeaponMod& m) -> int& { return m.s.throw_delay; });
        if (f & WF_SPEED) {
            out.push_back({ F::Number, info.bolt ? "    Speed" : "    Speed / throw", 1, 2000, 5, 50,
                [w](recomp_context*) { std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex); return (int)std::lround(g_weapon_mod[w].s.speed); },
                [w](recomp_context*, int v) {
                    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
                    if (!g_weapon_orig_read || !g_weapon_mod[w].on) return;
                    g_weapon_mod[w].s.speed = (float)std::clamp(v, 1, 2000);
                    save_weapons();
                } });
        }
        if (f & WF_LIFE) number(w, info.bolt ? "Lifetime (x0.1 s)" : "Lifetime / fuse", 1, 255, 1, 10, [](WeaponMod& m) -> int& { return m.s.life; });
        if (f & WF_SPREAD) {
            number(w, "Spread sideways", 0, 4096, 16, 256, [](WeaponMod& m) -> int& { return m.s.spread_x; });
            number(w, "Spread up/down", 0, 4096, 16, 256, [](WeaponMod& m) -> int& { return m.s.spread_y; });
        }
        if (f & WF_SIZE) number(w, "Bolt size (%)", 25, 400, 5, 50, [](WeaponMod& m) -> int& { return m.size; });
        if (f & WF_CHARGE) number(w, "Charge speed (%)", 25, 800, 5, 50, [](WeaponMod& m) -> int& { return m.charge; });
        if (f & WF_MINES) number(w, "Mines at once (both kinds)", 1, 8, 1, 1, [](WeaponMod& m) -> int& { return m.mines; });
        if (f & WF_HEAT) {
            out.push_back({ F::Toggle, "    No rapid-fire limit", 0, 1, 1, 1,
                [w](recomp_context*) { std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex); return g_weapon_mod[w].no_heat ? 1 : 0; },
                [w](recomp_context*, int v) {
                    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
                    if (!g_weapon_orig_read || !g_weapon_mod[w].on) return;
                    g_weapon_mod[w].no_heat = v != 0;
                    save_weapons();
                } });
        }
        out.push_back({ F::Action, "    Reset to default", 0, 1, 1, 1, nullptr, [w](recomp_context*, int) {
            std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
            if (!g_weapon_orig_read) return;
            const bool on = g_weapon_mod[w].on;
            g_weapon_mod[w] = WeaponMod();
            g_weapon_mod[w].on = on;
            g_weapon_mod[w].s = g_weapon_orig[w];
            save_weapons();
        } });
    }
    return out;
}

} // namespace jfg

JFG_WRAP(laserboltInit) {
    const gpr bolt = ctx->r4;
    laserboltInit_orig(rdram, ctx);
    recomp_context c = *ctx;
    c.r4 = bolt;
    jfg_bolt_init(rdram, &c);
}

JFG_WRAP(objAnimDframe) {
    ctx->r5 = (gpr)(int32_t)jfg_anim_speed(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5);
    objAnimDframe_orig(rdram, ctx);
}
