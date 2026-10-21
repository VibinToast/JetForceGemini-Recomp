// Jet Force Gemini port: the Items tab / window (F9, F10 > Tools): weapons and ammo, health, keys and objects,
// Tribals, ship and Floyd parts, multiplayer unlocks and game flags.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>

#include "imgui/imgui.h"
#include "jfg_fields.h"
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
    return valid_ptr(header) ? (int)LevelHeader::Region::get(header) : 0;
}

void collected_checkbox(uint32_t ch, int n, const char* name) {
    const uint32_t addr = ch + Character::Collected::offset + (uint32_t)(n >> 3);
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
    if (valid_ptr(table)) for (int r = 0; r < 31; r++) { total += (int8_t)rd8(table + r); rescued += RegionStat::Rescued::get(game + Game::RegionStats + RegionStat::Size * r); }
    ImGui::Text("Rescued in total: %d of %d", rescued, total);
    if (valid_ptr(table) && ImGui::Button("Mark every region fully rescued")) {
        for (int r = 0; r < 31; r++) {
            RegionStat::Rescued::set(game + Game::RegionStats + RegionStat::Size * r, (uint8_t)std::max(0, (int)(int8_t)rd8(table + r)));
            RegionStat::Killed::set(game + Game::RegionStats + RegionStat::Size * r, 0);
        }
        call_game(mainSetGameFlag, ctx, { 88u, 1u }); // ALL_TRIBALS_RESCUED, as frontShowRegionStats does
    }
    {
        bool flag = call_game(mainGetGameFlag, ctx, { 88u }) != 0;
        if (ImGui::Checkbox("\"All tribals rescued\" flag", &flag)) call_game(mainSetGameFlag, ctx, { 88u, flag ? 1u : 0u });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all##trib")) for (int r = 0; r < 31; r++) RegionStat::Counts::set(game + Game::RegionStats + RegionStat::Size * r, 0);
    if (ImGui::BeginTable("tribals", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Rescued", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Killed", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Tribals", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int r = 1; r < 31; r++) {
            const int count = valid_ptr(table) ? (int)(int8_t)rd8(table + r) : 0;
            if (count <= 0) continue;
            const uint32_t rec = game + Game::RegionStats + RegionStat::Size * (uint32_t)r;
            ImGui::PushID(r);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", region_names[r]);
            ImGui::TableNextColumn();
            int v = RegionStat::Rescued::get(rec);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##saved", &v)) RegionStat::Rescued::set(rec, (uint8_t)std::clamp(v, 0, count));
            ImGui::TableNextColumn();
            v = RegionStat::Killed::get(rec);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##killed", &v)) RegionStat::Killed::set(rec, (uint8_t)std::clamp(v, 0, count));
            ImGui::TableNextColumn();
            ImGui::Text("%d", count);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("This visit (the region you are in)");
    const int region = current_region();
    ImGui::Text("Region: %s", (region >= 1 && region <= 30) ? region_names[region] : "(none)");
    int v = Game::TribalsRescuedCount::get(game);
    if (ImGui::InputInt("Rescued so far", &v)) Game::TribalsRescuedCount::set(game, (uint8_t)std::clamp(v, 0, 255));
    v = Game::TribalsKilledCount::get(game);
    if (ImGui::InputInt("Killed so far", &v)) Game::TribalsKilledCount::set(game, (uint8_t)std::clamp(v, 0, 255));
    v = Game::TribalsLeftCount::get(game);
    if (ImGui::InputInt("Remaining", &v)) Game::TribalsLeftCount::set(game, (uint8_t)std::clamp(v, 0, 255));
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
    if (flag >= 0 && flag < 20) return (GlobalFlags::Flags0To19::get(ADDR_globalflags) >> flag) & 1;
    if (flag >= 66 && flag <= 70) return (GlobalFlags::Flags66To70::get(ADDR_globalflags) >> (flag - 66)) & 1;
    if (flag >= 73 && flag <= 76) return (GlobalFlags::Flags73To76::get(ADDR_globalflags) >> (flag - 73)) & 1;
    if (flag >= 82 && flag <= 87) return (GlobalFlags::Flags82To87::get(ADDR_globalflags) >> (flag - 82)) & 1;
    return false;
}
void set_multi_unlocked(recomp_context* ctx, int flag, bool on) {
    if (on) { call_game(mainSetGameFlag, ctx, { (uint32_t)flag, 1u }); return; }
    call_game(mainSetGameFlag, ctx, { (uint32_t)flag, 0u });
    if (flag >= 0 && flag < 20) GlobalFlags::Flags0To19::set(ADDR_globalflags, GlobalFlags::Flags0To19::get(ADDR_globalflags) & ~(1u << flag));
    else if (flag >= 66 && flag <= 70) GlobalFlags::Flags66To70::set(ADDR_globalflags, (uint8_t)(GlobalFlags::Flags66To70::get(ADDR_globalflags) & ~(1u << (flag - 66))));
    else if (flag >= 73 && flag <= 76) GlobalFlags::Flags73To76::set(ADDR_globalflags, (uint8_t)(GlobalFlags::Flags73To76::get(ADDR_globalflags) & ~(1u << (flag - 73))));
    else if (flag >= 82 && flag <= 87) GlobalFlags::Flags82To87::set(ADDR_globalflags, (uint16_t)(GlobalFlags::Flags82To87::get(ADDR_globalflags) & ~(1u << (flag - 82))));
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
        uint16_t owned = Character::WeaponsOwned::get(ch);
        ImGui::Text("Current weapon slot: %d", (int)Character::CurrentWeapon::get(ch));
        if (ImGui::Button("Give all")) { owned = 0x7FFF; set_owned_weapons(ch, owned); }
        ImGui::SameLine();
        if (ImGui::Button("Fill ammo")) {
            for (int w = 0; w < 16; w++) Character::Ammo::set(ch, w, 999);
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
                int ammo = Character::Ammo::get(ch, w);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##ammo", &ammo, 10, 100)) Character::Ammo::set(ch, w, (uint16_t)std::clamp(ammo, 0, 0xFFFF));
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
        int level = Character::HealthLevel::get(ch);
        const bool on_screen = g.edit_character == (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t obj = on_screen ? player_object(ctx) : 0;
        const uint32_t live = (obj != 0 && valid_ptr(Obj::Damage::get(obj))) ? Obj::Damage::get(obj) + Damage::Health::offset : 0;
        int units = (int16_t)rd16(live != 0 ? live : ch + Character::Health::offset) / 256;
        auto set_health = [&](int new_units) {
            const uint16_t v = (uint16_t)(std::clamp(new_units, 1, 5 * level + 4) * 256);
            Character::Health::set(ch, (int16_t)v);
            if (live != 0) wr16(live, v);
        };
        int upgrades = level - 6;
        if (ImGui::SliderInt("Upgrades", &upgrades, 0, 6)) {
            level = std::clamp(upgrades, 0, 6) + 6;
            Character::HealthLevel::set(ch, (int16_t)level);
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

// The same as the Items tab, as rows for F10's Items window (jfg_ui_tools.cpp). They act on g.edit_character.
std::vector<ToolField> item_fields() {
    using F = ToolField;
    std::vector<F> out;
    auto ch = []() { return character_ptr(std::clamp(g.edit_character, 0, 2)); };
    auto heading = [&](const char* text) { out.push_back({ F::Heading, text }); };
    auto note = [&](const char* text) { out.push_back({ F::Note, text }); };
    auto action = [&](const char* label, std::function<void(recomp_context*)> act) {
        out.push_back({ F::Action, label, 0, 1, 1, 1, nullptr, [act](recomp_context* c, int) { act(c); } });
    };
    auto flag_toggle = [&](int flag, const std::string& label) {
        out.push_back({ F::Toggle, label, 0, 1, 1, 1,
            [flag](recomp_context* c) { return call_game(mainGetGameFlag, c, { (uint32_t)flag }) != 0 ? 1 : 0; },
            [flag](recomp_context* c, int v) { call_game(mainSetGameFlag, c, { (uint32_t)flag, v ? 1u : 0u }); } });
    };
    auto collected_toggle = [&](int n, const char* name) {
        out.push_back({ F::Toggle, name, 0, 1, 1, 1,
            [ch, n](recomp_context*) {
                const uint32_t c = ch();
                return c != 0 && (rd8(c + Character::Collected::offset + (uint32_t)(n >> 3)) & (0x80u >> (n & 7))) ? 1 : 0;
            },
            [ch, n](recomp_context*, int v) {
                const uint32_t c = ch();
                if (c == 0) return;
                const uint32_t addr = c + Character::Collected::offset + (uint32_t)(n >> 3);
                const uint8_t mask = (uint8_t)(0x80u >> (n & 7));
                wr8(addr, (uint8_t)((rd8(addr) & ~mask) | (v ? mask : 0)));
            } });
    };

    out.push_back({ F::Note, "", 0, 1, 1, 1, nullptr, nullptr, [ch](recomp_context*) {
        const uint32_t c = ch();
        if (c == 0) return std::string("No game loaded.");
        return std::string("Editing ") + character_names[std::clamp(g.edit_character, 0, 2)] +
               ", current weapon slot " + std::to_string(Character::CurrentWeapon::get(c)) + ".";
    } });

    heading("Weapons and ammo");
    action("Give all weapons", [ch](recomp_context*) { if (const uint32_t c = ch()) set_owned_weapons(c, 0x7FFF); });
    action("Fill ammo", [ch](recomp_context*) { if (const uint32_t c = ch()) for (int w = 0; w < 16; w++) Character::Ammo::set(c, w, 999); });
    for (int w = 0; w < 15; w++) {
        out.push_back({ F::Toggle, std::to_string(w) + "  " + weapon_names[w], 0, 1, 1, 1,
            [ch, w](recomp_context*) { const uint32_t c = ch(); return c != 0 && ((Character::WeaponsOwned::get(c) >> w) & 1) ? 1 : 0; },
            [ch, w](recomp_context*, int v) {
                const uint32_t c = ch();
                if (c == 0) return;
                const uint16_t owned = Character::WeaponsOwned::get(c);
                set_owned_weapons(c, (uint16_t)((owned & ~(1u << w)) | ((v ? 1u : 0u) << w)));
            } });
        out.push_back({ F::Number, std::string("    Ammo: ") + weapon_names[w], 0, 999, 10, 100,
            [ch, w](recomp_context*) { const uint32_t c = ch(); return c != 0 ? (int)Character::Ammo::get(c, w) : 0; },
            [ch, w](recomp_context*, int v) { if (const uint32_t c = ch()) Character::Ammo::set(c, w, (uint16_t)std::clamp(v, 0, 0xFFFF)); } });
        out.push_back({ F::Action, std::string("    Hold the ") + weapon_names[w], 0, 1, 1, 1, nullptr, [w](recomp_context* c, int) {
            if (g.edit_character == (int16_t)rd16(ADDR_currentCharacter)) select_weapon(c, w);
        } });
    }

    heading("Health");
    // Game character: +0 s16 health (256 per unit), +2 s16 health level (6..12, 5 * level + 4 units at most). The
    // character on screen keeps its live health in its object's damage data (object + 0x4C, +6).
    auto set_health = [ch](recomp_context* c, int units) {
        const uint32_t p = ch();
        if (p == 0) return;
        const int level = Character::HealthLevel::get(p);
        const uint16_t v = (uint16_t)(std::clamp(units, 1, 5 * level + 4) * 256);
        Character::Health::set(p, (int16_t)v);
        const uint32_t obj = g.edit_character == (int16_t)rd16(ADDR_currentCharacter) ? player_object(c) : 0;
        if (obj != 0 && valid_ptr(Obj::Damage::get(obj))) wr16(Obj::Damage::get(obj) + Damage::Health::offset, v);
    };
    auto get_health = [ch](recomp_context* c) {
        const uint32_t p = ch();
        if (p == 0) return 0;
        const uint32_t obj = g.edit_character == (int16_t)rd16(ADDR_currentCharacter) ? player_object(c) : 0;
        const uint32_t live = (obj != 0 && valid_ptr(Obj::Damage::get(obj))) ? Obj::Damage::get(obj) + Damage::Health::offset : p + Character::Health::offset;
        return (int)(int16_t)rd16(live) / 256;
    };
    out.push_back({ F::Number, "Health upgrades", 0, 6, 1, 1,
        [ch](recomp_context*) { const uint32_t p = ch(); return p != 0 ? (int)Character::HealthLevel::get(p) - 6 : 0; },
        [ch, set_health](recomp_context* c, int v) {
            const uint32_t p = ch();
            if (p == 0) return;
            const int level = std::clamp(v, 0, 6) + 6;
            Character::HealthLevel::set(p, (int16_t)level);
            set_health(c, 5 * level + 4);
        } });
    out.push_back({ F::Number, "Health (units)", 1, 64, 1, 5, get_health, set_health });
    action("Fill health", [set_health](recomp_context* c) { set_health(c, 99); });

    heading("Keys");
    for (const NamedFlag& k : key_items) collected_toggle(k.id, k.name);
    heading("Objects");
    for (const NamedFlag& k : object_items) collected_toggle(k.id, k.name);

    heading("Tribals");
    note("For each region the game keeps your best visit: Tribals rescued and killed. When the rescued numbers of all "
         "regions add up to the total it sets \"All tribals rescued\", which earns the Tribals' ship part; editing the "
         "numbers here does not run that check, so set the flag as well.");
    out.push_back({ F::Note, "", 0, 1, 1, 1, nullptr, nullptr, [](recomp_context*) {
        const uint32_t game = rd32(ADDR_gameplay), table = rd32(ADDR_tribaltable);
        int total = 0, rescued = 0;
        if (valid_ptr(game) && valid_ptr(table))
            for (int r = 0; r < 31; r++) { total += (int8_t)rd8(table + r); rescued += RegionStat::Rescued::get(game + Game::RegionStats + RegionStat::Size * r); }
        return "Rescued in total: " + std::to_string(rescued) + " of " + std::to_string(total);
    } });
    action("Mark every region fully rescued", [](recomp_context* c) {
        const uint32_t game = rd32(ADDR_gameplay), table = rd32(ADDR_tribaltable);
        if (!valid_ptr(game) || !valid_ptr(table)) return;
        for (int r = 0; r < 31; r++) {
            RegionStat::Rescued::set(game + Game::RegionStats + RegionStat::Size * r, (uint8_t)std::max(0, (int)(int8_t)rd8(table + r)));
            RegionStat::Killed::set(game + Game::RegionStats + RegionStat::Size * r, 0);
        }
        call_game(mainSetGameFlag, c, { 88u, 1u }); // ALL_TRIBALS_RESCUED, as frontShowRegionStats does
    });
    flag_toggle(88, "\"All tribals rescued\" flag (88)");
    action("Clear all region records", [](recomp_context*) {
        const uint32_t game = rd32(ADDR_gameplay);
        if (valid_ptr(game)) for (int r = 0; r < 31; r++) RegionStat::Counts::set(game + Game::RegionStats + RegionStat::Size * r, 0);
    });
    auto region_count = [](int r) { const uint32_t t = rd32(ADDR_tribaltable); return valid_ptr(t) ? (int)(int8_t)rd8(t + r) : 0; };
    for (int r = 1; r < 31; r++) {
        for (int k = 0; k < 2; k++) {
            ToolField f{ F::Number, "", 0, 20, 1, 5,
                [r, k](recomp_context*) { const uint32_t game = rd32(ADDR_gameplay); return valid_ptr(game) ? (int)rd8(game + Game::RegionStats + RegionStat::Size * r + k) : 0; },
                [r, k, region_count](recomp_context*, int v) {
                    const uint32_t game = rd32(ADDR_gameplay);
                    if (valid_ptr(game)) wr8(game + Game::RegionStats + RegionStat::Size * r + k, (uint8_t)std::clamp(v, 0, std::max(0, region_count(r))));
                },
                [r, k, region_count](recomp_context*) {
                    return std::string(region_names[r]) + (k == 0 ? ": rescued (of " : ": killed (of ") + std::to_string(region_count(r)) + ")";
                } };
            out.push_back(f);
        }
    }
    out.push_back({ F::Note, "", 0, 1, 1, 1, nullptr, nullptr, [](recomp_context*) {
        const int region = current_region();
        return std::string("This visit: ") + ((region >= 1 && region <= 30) ? region_names[region] : "(no region)");
    } });
    const char* const visit_labels[3] = { "Rescued so far", "Killed so far", "Remaining" };
    for (int k = 0; k < 3; k++) {
        out.push_back({ F::Number, visit_labels[k], 0, 255, 1, 5,
            [k](recomp_context*) { const uint32_t game = rd32(ADDR_gameplay); return valid_ptr(game) ? (int)rd8(game + Game::TribalsRescuedCount::offset + k) : 0; },
            [k](recomp_context*, int v) { const uint32_t game = rd32(ADDR_gameplay); if (valid_ptr(game)) wr8(game + Game::TribalsRescuedCount::offset + k, (uint8_t)std::clamp(v, 0, 255)); } });
    }

    heading("Ship parts");
    action("Give all ship parts", [](recomp_context* c) { for (const NamedFlag& f : ship_part_flags) call_game(mainSetGameFlag, c, { (uint32_t)f.id, 1u }); });
    for (const NamedFlag& f : ship_part_flags) flag_toggle(f.id, std::string(f.name) + " (" + std::to_string(f.id) + ")");
    heading("Floyd parts");
    for (const NamedFlag& f : robot_flags) flag_toggle(f.id, std::string(f.name) + " (" + std::to_string(f.id) + ")");

    heading("Multiplayer unlocks");
    note("Bonuses that unlock multiplayer characters and levels; shared by all save files.");
    action("Unlock all", [](recomp_context* c) {
        for (const MultiUnlock& u : multi_characters) set_multi_unlocked(c, u.flag, true);
        for (const MultiUnlock& u : multi_levels) set_multi_unlocked(c, u.flag, true);
    });
    for (const MultiUnlock* list : { std::begin(multi_characters), std::begin(multi_levels) }) {
        const MultiUnlock* end = list == std::begin(multi_characters) ? std::end(multi_characters) : std::end(multi_levels);
        for (const MultiUnlock* u = list; u != end; u++) {
            const int flag = u->flag;
            out.push_back({ F::Toggle, u->name, 0, 1, 1, 1,
                [flag](recomp_context*) { return multi_unlocked(flag) ? 1 : 0; },
                [flag](recomp_context* c, int v) { set_multi_unlocked(c, flag, v != 0); } });
        }
    }

    heading("Progress flags");
    for (const NamedFlag& f : progress_flags) flag_toggle(f.id, std::string(f.name) + " (" + std::to_string(f.id) + ")");

    heading("Any game flag by number");
    out.push_back({ F::Number, "Flag number", 0, 127, 1, 10,
        [](recomp_context*) { return std::clamp(g.raw_flag, 0, 127); },
        [](recomp_context*, int v) { g.raw_flag = std::clamp(v, 0, 127); } });
    out.push_back({ F::Toggle, "That flag is set", 0, 1, 1, 1,
        [](recomp_context* c) { return call_game(mainGetGameFlag, c, { (uint32_t)std::clamp(g.raw_flag, 0, 127) }) != 0 ? 1 : 0; },
        [](recomp_context* c, int v) { call_game(mainSetGameFlag, c, { (uint32_t)std::clamp(g.raw_flag, 0, 127), v ? 1u : 0u }); } });
    return out;
}

} // namespace jfg
