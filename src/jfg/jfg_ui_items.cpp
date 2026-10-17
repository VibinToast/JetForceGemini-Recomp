// Jet Force Gemini port: the Items tab / window (F9, F10 > Tools): weapons and ammo, health, keys and objects,
// Tribals, ship and Floyd parts, multiplayer unlocks and game flags.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

namespace {
struct NamedFlag { int id; const char* name; };
// include/enums.h (GameFlags; list credited there to Jimmie1717, marked unverified).
const NamedFlag ship_part_flags[] = {
    {32, "Power cell"}, {33, "Radar dish"}, {34, "Fin"}, {35, "Cargo bay key"}, {36, "Deflector shield"},
    {37, "Fuse"}, {38, "Vela's hatch key"}, {39, "Juno's hatch key"}, {40, "Lupus's hatch key"},
    {41, "Nitrogen tank"}, {42, "Oxygen tank"}, {43, "Stabilizer"},
};
const NamedFlag robot_flags[] = { {44, "Floyd part: top"}, {45, "Floyd part: right"}, {46, "Floyd part: left"} };
const NamedFlag progress_flags[] = {
    {0, "Vela (character flag)"}, {1, "Juno (character flag)"}, {2, "Lupus (character flag)"}, {3, "Floyd"},
    {20, "Intro seen: Fet Bubb"}, {21, "Intro seen: Lurg"}, {22, "Intro seen: Mechantids"}, {23, "Intro seen: tutorial"},
    {77, "Defeated Fet Bubb"}, {78, "Defeated Lurg"}, {79, "Defeated Mechantids"},
    {88, "All tribals rescued"}, {89, "Quarry generator activated"},
    {90, "Disable Vela select"}, {91, "Disable Juno select"}, {92, "Disable Lupus select"},
    {94, "Pyramid activated"}, {95, "Jetpack"}, {101, "Machine gun lock (Goldwood)"},
};
// Per-character pickups (mainPlayerCollected / mainPlayerHas). Keys: the key object's colour number is the pickup
// number, in the order of the game's "... Key Required" messages. Objects: the numbers the pause menu's inventory
// page tests (overlay 18, inventoryScreen), named after the icon it draws for each (checked by setting every bit
// and looking at that page; keys 0, 1, 2, 3 and 9 are the five it shows).
const NamedFlag key_items[] = {
    {0, "Yellow key"}, {1, "Red key"}, {2, "Magenta key"}, {3, "Green key"}, {4, "Cyan (light blue) key"}, {5, "Lime key"},
    {6, "Purple key"}, {7, "Tango (orange) key"}, {8, "Lemon key"}, {9, "Blue key"},
    {10, "Vela Tri-Rocket Check"},
};
const NamedFlag object_items[] = {
    {21, "Crowbar"}, {26, "Ear plugs"}, {20, "Pants"}, {16, "Magazine"},
    {23, "Gold bar 1 of 3"}, {24, "Gold bar 2 of 3"}, {25, "Gold bar 3 of 3"},
    {17, "Mine key"}, {27, "Arcade chip (racing)"}, {22, "Night vision goggles"},
};

// Regions (byte 0x102 of the level header, 1..30) in the game's order; names are the game's own (text 408-437) with
// the planet in front.
const char* const region_names[31] = {
    "(none)",
    "Goldwood - Outset", "Goldwood - Lodge", "S.S. Anubis - Passageway", "Goldwood - Interior", "Goldwood - Rim",
    "Rith Essa - Bluff", "S.S. Anubis - Hold", "Sekhmet - Battle Cruiser", "Walkway - Peak", "S.S. Anubis - Depository",
    "Spawnship - Troop Carrier", "Rith Essa - Interior", "Cerulean - Dune", "Tawfret - Bog", "Tawfret - Bridge",
    "Tawfret - Castle", "Rith Essa - Ascent", "Ichor - Military Base", "Water Ruin - Lost Island", "Rith Essa - Mine",
    "Ichor - Perimeter", "Eschebone - Approach", "Eschebone - Thorax", "Eschebone - Cortex", "Gem Quarry - Landing",
    "Spacestation - Abandoned Wreck", "Mizar's Palace - Lobby", "Mizar's Palace - Flume", "Mizar's Palace - Chasm",
    "Asteroid - Mizar's Lair",
};
// Slot order is the game's weapon index (bit in the owned mask). The names are the game's own, in the order of its
// text table (ROM 0xC36F10: "Pistol", "Homing Missiles", ...); the order was checked against the HUD icon, the ammo
// count and the reticle of each slot.
const char* const weapon_names[16] = {
    "Pistol", "Homing Missiles", "Machine Gun", "Plasma Shotgun", "Shocker", "Tri-Rocket Launcher", "Flamethrower", "Sniper Rifle",
    "Grenades", "Shuriken", "Fish Food", "Timed Mines", "Remote Mines", "Flares", "Cluster Bombs", "(unused)",
};

void flag_checkboxes(recomp_context* ctx, const NamedFlag* flags, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bool on = call_game(mainGetGameFlag, ctx, { (uint32_t)flags[i].id }) != 0;
        char label[64];
        std::snprintf(label, sizeof(label), "%s (%d)", flags[i].name, flags[i].id);
        if (ImGui::Checkbox(label, &on)) {
            call_game(mainSetGameFlag, ctx, { (uint32_t)flags[i].id, on ? 1u : 0u });
        }
    }
}

int current_region() {
    const uint32_t header = rd32(ADDR_levelHeader);
    return valid_ptr(header) ? (int)rd8(header + 0x102) : 0;
}

void collected_checkbox(uint32_t ch, int n, const char* name) {
    const uint32_t addr = ch + CHAR_COLLECTED_OFFSET + (uint32_t)(n >> 3);
    const uint8_t mask = (uint8_t)(0x80u >> (n & 7));
    bool on = (rd8(addr) & mask) != 0;
    char label[64];
    std::snprintf(label, sizeof(label), "%s##c%d", name, n);
    if (ImGui::Checkbox(label, &on)) wr8(addr, (uint8_t)((rd8(addr) & ~mask) | (on ? mask : 0)));
}

void draw_tribals(recomp_context* ctx) {
    const uint32_t game = rd32(ADDR_gameplay);
    const uint32_t table = rd32(ADDR_tribaltable);
    ImGui::TextWrapped("For each region the game keeps your best visit: how many Tribals were rescued and how many died. "
                       "When a visit sets a new record and the rescued numbers of all regions add up to the total, it sets the "
                       "\"All tribals rescued\" flag, which is what earns the Tribals' ship part. Editing numbers here does not "
                       "run that check, so the button below sets the flag as well.");
    int total = 0, rescued = 0;
    if (valid_ptr(table)) for (int r = 0; r < 31; r++) { total += (int8_t)rd8(table + r); rescued += rd8(game + GAME_REGION_STATS_OFFSET + 8 * r); }
    ImGui::Text("Rescued in total: %d of %d", rescued, total);
    if (valid_ptr(table) && ImGui::Button("Mark every region fully rescued")) {
        for (int r = 0; r < 31; r++) {
            wr8(game + GAME_REGION_STATS_OFFSET + 8 * r, (uint8_t)std::max(0, (int)(int8_t)rd8(table + r)));
            wr8(game + GAME_REGION_STATS_OFFSET + 8 * r + 1, 0);
        }
        call_game(mainSetGameFlag, ctx, { 88u, 1u }); // ALL_TRIBALS_RESCUED, as frontShowRegionStats does
    }
    {
        bool flag = call_game(mainGetGameFlag, ctx, { 88u }) != 0;
        if (ImGui::Checkbox("\"All tribals rescued\" flag", &flag)) call_game(mainSetGameFlag, ctx, { 88u, flag ? 1u : 0u });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all##trib")) for (int r = 0; r < 31; r++) wr16(game + GAME_REGION_STATS_OFFSET + 8 * r, 0);
    if (ImGui::BeginTable("tribals", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Rescued", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Killed", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Tribals", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int r = 1; r < 31; r++) {
            const int count = valid_ptr(table) ? (int)(int8_t)rd8(table + r) : 0;
            if (count <= 0) continue;
            const uint32_t rec = game + GAME_REGION_STATS_OFFSET + 8 * (uint32_t)r;
            ImGui::PushID(r);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", region_names[r]);
            ImGui::TableNextColumn();
            int v = rd8(rec);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##saved", &v)) wr8(rec, (uint8_t)std::clamp(v, 0, count));
            ImGui::TableNextColumn();
            v = rd8(rec + 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##killed", &v)) wr8(rec + 1, (uint8_t)std::clamp(v, 0, count));
            ImGui::TableNextColumn();
            ImGui::Text("%d", count);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("This visit (the region you are in)");
    const int region = current_region();
    ImGui::Text("Region: %s", (region >= 1 && region <= 30) ? region_names[region] : "(none)");
    int v = rd8(game + 4);
    if (ImGui::InputInt("Rescued so far", &v)) wr8(game + 4, (uint8_t)std::clamp(v, 0, 255));
    v = rd8(game + 5);
    if (ImGui::InputInt("Killed so far", &v)) wr8(game + 5, (uint8_t)std::clamp(v, 0, 255));
    v = rd8(game + 6);
    if (ImGui::InputInt("Remaining", &v)) wr8(game + 6, (uint8_t)std::clamp(v, 0, 255));
    ImGui::TextDisabled("\"Killed\" above 0 or \"Remaining\" above 0 when you leave means the visit is not a full rescue.");
}

// Multiplayer unlocks. Totem poles (totemPoleControl: mainSetGameFlag(romdef+0xA)), Floyd medals and race wins set game
// flags that mainSetGameFlag also copies into globalflags (0x800FF318), the part of the Controller Pak / EEPROM data
// shared by all save files, which is what the multiplayer menus read:
//   flags 0..19  -> globalflags+0x64 bit n: multiplayer characters (frontMultiSelect's table at 0x82381050: flags 0-2 the
//                   three heroes, 4-6 set with the jetpack (flag 95), 8-16 and 18 the totem-pole characters, 17 the
//                   Cyborg won with expert Floyd medals ("Won the CYBORG"));
//   flags 66..70 -> +0x6A bits 0..4: race tracks (frontMultiModeSelect's table at 0x823C285C: 66 level 268, 67 level
//                   258, 68 level 397, 70 level 202);
//   flags 73..76 -> +0x6B bits 0..3: target ranges (table at 0x823C2878: 73 level 251, 74 level 364, 75 level 408);
//   flags 82..87 -> +0x68 bits 0..5: battle arenas (table at 0x823C2828: 82 level 221, 83 level 144, 84 level 355,
//                   85 level 270).
// Unlocking sets the game flag the way the game does (which also saves the shared data); locking clears both bits
// (the shared data is written the next time the game saves it).
constexpr uint32_t ADDR_globalflags = 0x800FF318;
struct MultiUnlock { int flag; const char* name; };
const MultiUnlock multi_characters[] = {
    {4, "Character (jetpack, flag 4)"}, {5, "Character (jetpack, flag 5)"}, {6, "Character (jetpack, flag 6)"},
    {8, "Totem character (flag 8)"}, {9, "Totem character (flag 9)"}, {10, "Totem character (flag 10)"},
    {11, "Totem character (flag 11)"}, {12, "Totem character (flag 12)"}, {13, "Totem character (flag 13)"},
    {14, "Totem character (flag 14)"}, {15, "Totem character (flag 15)"}, {16, "Totem character (flag 16)"},
    {18, "Totem character (flag 18)"}, {17, "Cyborg Ant (expert Floyd medals)"},
};
const MultiUnlock multi_levels[] = {
    {82, "Battle arena: Mine (level 221)"}, {83, "Battle arena: Sewer (level 144)"}, {84, "Battle arena: Tunnels (level 355)"},
    {85, "Battle arena: Swamp (level 270)"},
    {66, "Race: Mizar's Race (level 268)"}, {67, "Race: Jeff and Barry (level 258)"}, {68, "Race: Jeff and Barry II (level 397)"},
    {70, "Race: Greenwood Village (level 202)"},
    {73, "Target range: Goldwood (level 251)"}, {74, "Target range: SS Anubis (level 364)"}, {75, "Target range: Rith Essa (level 408)"},
};
bool multi_unlocked(int flag) {
    if (flag >= 0 && flag < 20) return (rd32(ADDR_globalflags + 0x64) >> flag) & 1;
    if (flag >= 66 && flag <= 70) return (rd8(ADDR_globalflags + 0x6A) >> (flag - 66)) & 1;
    if (flag >= 73 && flag <= 76) return (rd8(ADDR_globalflags + 0x6B) >> (flag - 73)) & 1;
    if (flag >= 82 && flag <= 87) return (rd16(ADDR_globalflags + 0x68) >> (flag - 82)) & 1;
    return false;
}
void set_multi_unlocked(recomp_context* ctx, int flag, bool on) {
    if (on) { call_game(mainSetGameFlag, ctx, { (uint32_t)flag, 1u }); return; }
    call_game(mainSetGameFlag, ctx, { (uint32_t)flag, 0u });
    if (flag >= 0 && flag < 20) wr32(ADDR_globalflags + 0x64, rd32(ADDR_globalflags + 0x64) & ~(1u << flag));
    else if (flag >= 66 && flag <= 70) wr8(ADDR_globalflags + 0x6A, (uint8_t)(rd8(ADDR_globalflags + 0x6A) & ~(1u << (flag - 66))));
    else if (flag >= 73 && flag <= 76) wr8(ADDR_globalflags + 0x6B, (uint8_t)(rd8(ADDR_globalflags + 0x6B) & ~(1u << (flag - 73))));
    else if (flag >= 82 && flag <= 87) wr16(ADDR_globalflags + 0x68, (uint16_t)(rd16(ADDR_globalflags + 0x68) & ~(1u << (flag - 82))));
}
void draw_multiplayer_unlocks(recomp_context* ctx) {
    ImGui::TextWrapped("Bonuses found in the game (totem poles, Floyd medals, race wins) that unlock multiplayer characters "
                       "and levels. They are shared by all save files. The game does not name the totem characters in "
                       "its data, so they are listed by flag.");
    if (ImGui::Button("Unlock all##multi")) {
        for (const MultiUnlock& u : multi_characters) set_multi_unlocked(ctx, u.flag, true);
        for (const MultiUnlock& u : multi_levels) set_multi_unlocked(ctx, u.flag, true);
    }
    ImGui::SeparatorText("Characters");
    if (ImGui::BeginTable("multi_chars", 2)) {
        for (const MultiUnlock& u : multi_characters) {
            ImGui::TableNextColumn();
            bool on = multi_unlocked(u.flag);
            if (ImGui::Checkbox(u.name, &on)) set_multi_unlocked(ctx, u.flag, on);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Levels");
    for (const MultiUnlock& u : multi_levels) {
        bool on = multi_unlocked(u.flag);
        if (ImGui::Checkbox(u.name, &on)) set_multi_unlocked(ctx, u.flag, on);
    }
}
} // namespace

void draw_inventory(recomp_context* ctx) {
    if (g.edit_character < 0) {
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        g.edit_character = (c >= 0 && c < 3) ? c : 1;
    }
    ImGui::Combo("Character##inv", &g.edit_character, character_names, 3);
    const uint32_t ch = character_ptr(g.edit_character);
    if (ch == 0) {
        ImGui::TextDisabled("No game loaded.");
        return;
    }

    if (ImGui::CollapsingHeader("Weapons and ammo", ImGuiTreeNodeFlags_DefaultOpen)) {
        uint16_t owned = rd16(ch + CHAR_WEAPONS_OFFSET);
        ImGui::Text("Current weapon slot: %d", (int)rd8(ch + CHAR_WEAPON_OFFSET));
        if (ImGui::Button("Give all")) { owned = 0x7FFF; set_owned_weapons(ch, owned); }
        ImGui::SameLine();
        if (ImGui::Button("Fill ammo")) {
            for (int w = 0; w < 16; w++) wr16(ch + CHAR_AMMO_OFFSET + 2 * w, 999);
        }
        if (ImGui::BeginTable("weapons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Ammo", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (int w = 0; w < 15; w++) {
                ImGui::PushID(w);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool has = (owned >> w) & 1;
                char label[48];
                std::snprintf(label, sizeof(label), "%d  %s", w, weapon_names[w]);
                if (ImGui::Checkbox(label, &has)) {
                    owned = (uint16_t)((owned & ~(1u << w)) | ((has ? 1u : 0u) << w));
                    set_owned_weapons(ch, owned);
                }
                ImGui::TableNextColumn();
                int ammo = rd16(ch + CHAR_AMMO_OFFSET + 2 * w);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##ammo", &ammo, 10, 100)) wr16(ch + CHAR_AMMO_OFFSET + 2 * w, (uint16_t)std::clamp(ammo, 0, 0xFFFF));
                ImGui::TableNextColumn();
                if (g.edit_character == (int16_t)rd16(ADDR_currentCharacter) && ImGui::SmallButton("Hold")) select_weapon(ctx, w);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Health")) {
        // Game character: +0 s16 health (256 per unit), +2 s16 health level. A level allows 5 * level + 4 units; a new
        // character starts at level 6 and each "Gemini capacity" pickup adds one, up to 12 (healthPickupControl).
        // The character on screen keeps its live health in its object's damage data (object + 0x4C, +6).
        int level = (int16_t)rd16(ch + CHAR_HEALTH_LEVEL_OFFSET);
        const bool on_screen = g.edit_character == (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t obj = on_screen ? player_object(ctx) : 0;
        const uint32_t live = (obj != 0 && valid_ptr(rd32(obj + 0x4C))) ? rd32(obj + 0x4C) + 6 : 0;
        int units = (int16_t)rd16(live != 0 ? live : ch + CHAR_HEALTH_OFFSET) / 256;
        auto set_health = [&](int new_units) {
            const uint16_t v = (uint16_t)(std::clamp(new_units, 1, 5 * level + 4) * 256);
            wr16(ch + CHAR_HEALTH_OFFSET, v);
            if (live != 0) wr16(live, v);
        };
        int upgrades = level - 6;
        if (ImGui::SliderInt("Upgrades", &upgrades, 0, 6)) {
            level = std::clamp(upgrades, 0, 6) + 6;
            wr16(ch + CHAR_HEALTH_LEVEL_OFFSET, (uint16_t)level);
            set_health(5 * level + 4);
        }
        ImGui::Text("Maximum health: %d units", 5 * level + 4);
        if (ImGui::InputInt("Health", &units)) set_health(units);
        if (ImGui::Button("Fill health")) set_health(5 * level + 4);
    }

    if (ImGui::CollapsingHeader("Keys and objects (per character)")) {
        ImGui::SeparatorText("Keys");
        if (ImGui::BeginTable("keys", 2)) {
            for (const NamedFlag& k : key_items) { ImGui::TableNextColumn(); collected_checkbox(ch, k.id, k.name); }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Objects");
        if (ImGui::BeginTable("objects", 2)) {
            for (const NamedFlag& k : object_items) { ImGui::TableNextColumn(); collected_checkbox(ch, k.id, k.name); }
            ImGui::EndTable();
        }
        if (ImGui::TreeNode("Other pickup numbers (no known use)")) {
            for (int n = 0, col = 0; n < COLLECTED_BITS; n++) {
                bool named = false;
                for (const NamedFlag& k : key_items) named |= k.id == n;
                for (const NamedFlag& k : object_items) named |= k.id == n;
                if (named) continue;
                if ((col++ & 7) != 0) ImGui::SameLine((float)((col - 1) & 7) * ImGui::GetFontSize() * 4.0f);
                char label[8];
                std::snprintf(label, sizeof(label), "%02d", n);
                collected_checkbox(ch, n, label);
            }
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Tribals")) draw_tribals(ctx);

    if (ImGui::CollapsingHeader("Ship parts")) {
        if (ImGui::Button("Give all##ship")) {
            for (const NamedFlag& f : ship_part_flags) call_game(mainSetGameFlag, ctx, { (uint32_t)f.id, 1u });
        }
        flag_checkboxes(ctx, ship_part_flags, std::size(ship_part_flags));
    }
    if (ImGui::CollapsingHeader("Floyd parts")) flag_checkboxes(ctx, robot_flags, std::size(robot_flags));
    if (ImGui::CollapsingHeader("Multiplayer unlocks")) draw_multiplayer_unlocks(ctx);
    if (ImGui::CollapsingHeader("Progress flags")) flag_checkboxes(ctx, progress_flags, std::size(progress_flags));
    if (ImGui::CollapsingHeader("Any game flag by number")) {
        ImGui::InputInt("Flag", &g.raw_flag);
        g.raw_flag = std::clamp(g.raw_flag, 0, 127);
        bool on = call_game(mainGetGameFlag, ctx, { (uint32_t)g.raw_flag }) != 0;
        if (ImGui::Checkbox("Set", &on)) call_game(mainSetGameFlag, ctx, { (uint32_t)g.raw_flag, on ? 1u : 0u });
    }
}

} // namespace jfg
