// Jet Force Gemini port: the "Enemies ignore you" cheat (Tools > Cheats), for watching squads patrol and wander.
//
// A squad member goes after a hero once the hero is its target (member data +0x64). While the cheat is on, nothing
// puts a hero there (decomp PORT_FINDINGS request 13):
// - sight: CheckLineOfSight (overlay 3) reports nothing seen. The game does the same for a few ticks after a level
//   loads, while its blind counter runs down; with no sighting there is no "spotted" state and no sniper fire either;
// - hearing: squadsAddInterestingEvent drops every event (the heroes' voices and footsteps, shots, explosions, crates;
//   bolts don't say who fired them, so the squads' reactions to other noises go too);
// - being shot: squadsPlayerShootsSquaddie leaves the member alone. It still takes the damage and dies.
// Without a target members neither face nor fire at the hero. Squadrons with flag 0x40 (go for the hero at spawn) are
// left as they are, so Tribals keep whatever their spawn sets up. A squad that was already after the hero when the
// cheat went on keeps that target until its area is reset. Smart squads (behaviour 0x11) hold where they stand;
// patrols and wanderers carry on. Squad-locked doors still open when every member is dead.
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

JFG_WRAP(CheckLineOfSight) {
    if (jfg::g.squads_ignore) {
        ctx->r2 = 0;
        return;
    }
    CheckLineOfSight_orig(rdram, ctx);
}

JFG_WRAP(squadsAddInterestingEvent) {
    if (jfg::g.squads_ignore) return;
    squadsAddInterestingEvent_orig(rdram, ctx);
}

JFG_WRAP(squadsPlayerShootsSquaddie) {
    if (jfg::g.squads_ignore) return;
    squadsPlayerShootsSquaddie_orig(rdram, ctx);
}
