// Jet Force Gemini port: the launcher's mod store (Get Mods page). Recomp-Docs OFFICIAL_MODS_PLAN.md "Mod Store".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A collection of mods: one GitHub repo with a catalog.json. The owner's are built in; each can be turned off.
struct JfgStoreSource {
    std::string id, name, url;
    bool official = false, enabled = true, loaded = false;
    int mods = 0;            // in its catalog
    std::string error;       // why its catalog couldn't be read
};

// One mod as the page shows it: the catalog's entry from the highest-priority source, and what is installed.
struct JfgStoreMod {
    std::string id, name, short_text, description, version, authors, tags, source, source_name, size_text;
    std::string thumb;                    // its picture's file (the preview: a local file), empty if none
    std::string installed_version;        // empty if not installed
    bool installed = false, update = false, managed = false; // managed: the store installed it (or it is the catalog's file)
    bool blocked = false;                 // needs a newer port, or a dependency no enabled source has
    std::string blocked_reason;
    std::vector<std::string> dependencies;
    std::string shadowed;                 // another source also lists this id (the lower-priority one's name)
};

struct JfgStoreState {
    bool preview = false;                 // catalogs from bin/mod_store_preview, not the internet
    bool busy = false;
    int percent = 0;
    std::string busy_id, text;            // "Installing Rolling Shellback..." / the last result
    int serial = 0;                       // changes whenever the list should be read again
};

// Reads the sources' catalogs again (built-in sources, their on/off from mod_store.json) and what is installed.
void jfg_store_refresh();
std::vector<JfgStoreSource> jfg_store_sources();
std::vector<JfgStoreMod> jfg_store_mods();
JfgStoreState jfg_store_state();
void jfg_store_set_source_enabled(const std::string& source_id, bool enabled);
// Install or update (with the mod's dependencies first) in the background; remove at once (only mods the store
// manages). Changes take effect when the game starts.
void jfg_store_install(const std::string& mod_id);
void jfg_store_remove(const std::string& mod_id);
// Called each launcher frame: finishes a background install (the Mods page rescans).
void jfg_store_tick();
