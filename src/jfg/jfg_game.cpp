// Jet Force Gemini port: access to the game (jfg_game.h): calls into the game, level names, characters.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <span>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace recomp { std::span<const uint8_t> get_rom(); }

namespace jfg {

uint8_t* g_rdram = nullptr;

// Calls a recompiled game function with up to six integer arguments on the current (game) thread and returns $v0.
// The caller's register state is saved and restored, and the callee gets its own stack frame below the current one.
uint32_t call_game(void (*func)(uint8_t*, recomp_context*), recomp_context* ctx, std::initializer_list<uint32_t> args) {
    recomp_context saved = *ctx;
    ctx->r29 = (gpr)(int32_t)(((uint32_t)ctx->r29 - 0x100u) & ~0xFu);
    gpr* reg[4] = { &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7 };
    int i = 0;
    for (uint32_t a : args) {
        if (i < 4) *reg[i] = (gpr)(int32_t)a;
        else wr32_at(ctx->r29, i * 4, a); // o32: arguments 5+ live at 0x10($sp), 0x14($sp), ...
        i++;
    }
    func(g_rdram, ctx);
    const uint32_t ret = (uint32_t)ctx->r2;
    *ctx = saved;
    return ret;
}

// --- level names -------------------------------------------------------------------------------------------------
std::vector<std::string> g_level_names;

void load_level_names_once();
void load_level_names() { // called from the game thread and the UI thread
    static std::once_flag once;
    std::call_once(once, load_level_names_once);
}
void load_level_names_once() {
    const std::span<const uint8_t> rom = recomp::get_rom();
    auto be32 = [&](size_t o) -> uint32_t {
        if (o + 4 > rom.size()) return 0xFFFFFFFFu;
        return ((uint32_t)rom[o] << 24) | ((uint32_t)rom[o + 1] << 16) | ((uint32_t)rom[o + 2] << 8) | rom[o + 3];
    };
    const size_t offsets = (size_t)ROM_ASSET_DATA + be32(ROM_ASSET_TABLE + 4 * (0x1E + 1));
    const size_t headers = (size_t)ROM_ASSET_DATA + be32(ROM_ASSET_TABLE + 4 * (0x1F + 1));
    for (size_t i = 0; i < 1024; i++) {
        const uint32_t off = be32(offsets + 4 * i);
        if (off == 0xFFFFFFFFu || headers + off + 0x20 > rom.size()) break;
        char name[0x21] = {};
        std::memcpy(name, rom.data() + headers + off, 0x20);
        for (char& c : name) if (c != 0 && (c < 0x20 || c > 0x7E)) c = '?';
        g_level_names.emplace_back(name);
    }
    if (g_level_names.empty()) g_level_names.emplace_back("(level table not found)");
}

uint32_t character_ptr(int character) {
    const uint32_t game = rd32(ADDR_gameplay);
    if (!valid_ptr(game)) return 0;
    return game + Game::Characters + (uint32_t)(character & 3) * Character::Size;
}

// The first player's object (objGetPlayerlist), or 0.
uint32_t player_object(recomp_context* ctx) {
    const uint32_t scratch = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
    wr32(scratch, 0);
    const uint32_t list = call_game(objGetPlayerlist, ctx, { scratch });
    if (!valid_ptr(list) || (int32_t)rd32(scratch) < 1) return 0;
    const uint32_t obj = rd32(list);
    return valid_ptr(obj) ? obj : 0;
}

// Puts a weapon in the player's hand through the game's own controlSelectWeapon(playerObject, playerData, slot),
// which only accepts weapons the character owns - so the slot is added to the owned mask first.
bool select_weapon(recomp_context* ctx, int slot) {
    const uint32_t ch = character_ptr((int16_t)rd16(ADDR_currentCharacter));
    if (ch == 0 || slot < 0 || slot > 15) return false;
    const uint32_t obj = player_object(ctx);
    if (obj == 0) return false;
    const uint32_t data = Obj::Data::get(obj);
    if (!valid_ptr(data)) return false;
    Character::WeaponsOwned::set(ch, (uint16_t)(Character::WeaponsOwned::get(ch) | (1u << slot)));
    if (Character::Ammo::get(ch, slot) == 0) Character::Ammo::set(ch, slot, 50);
    return call_game(controlSelectWeapon, ctx, { obj, data, (uint32_t)slot }) != 0;
}

bool contains_ci(const std::string& s, const char* needle) {
    if (needle[0] == 0) return true;
    auto it = std::search(s.begin(), s.end(), needle, needle + std::strlen(needle),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != s.end();
}

// The game keeps the number of owned weapons next to the mask and only lets the player change weapon when it is 2 or
// more (the "all weapons" cheat writes mask 0x7FFF and count 15 the same way, in the character control overlays).
void set_owned_weapons(uint32_t ch, uint16_t owned) {
    owned &= 0x7FFF;
    int count = 0;
    for (int w = 0; w < 15; w++) count += (owned >> w) & 1;
    Character::WeaponsOwned::set(ch, owned);
    Character::WeaponCount::set(ch, (uint8_t)count);
}

} // namespace jfg
