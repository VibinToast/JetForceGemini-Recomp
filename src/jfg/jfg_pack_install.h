// JFG PORT: the official HD texture pack in the launcher's Texture Packs page (src/ui/ui_launcher.cpp): which version is
// out, which is installed, and downloading / updating it (src/jfg/jfg_pack_install.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct JfgOfficialPack {
    std::string variant;    // "1K", "2K", "4K"
    uint64_t size = 0;      // the download, bytes
    std::string installed;  // the version in texture_packs, "" if none
    std::string action;     // "Download", "Update" or "Installed"
};

struct JfgOfficialState {
    int stage = 0;            // 0 = not asked (Check for Updates is off), 1 = asking GitHub, 2 = known, 3 = could not ask
    std::string latest;       // the latest version, like "1.1.0"
    std::vector<JfgOfficialPack> packs; // what the latest release has, smallest first
    bool busy = false;        // a download is running
    int percent = 0;          // its progress, 0..100
    std::string text;         // what it is doing, or how the last one ended
    int serial = 0;           // changes when the packs in texture_packs may have changed (an install finished)
};

// Asks GitHub for the latest pack release, once, in the background (only with Check for Updates on).
void jfg_official_check_start();
// The state for the page. scan: also read which official packs are in texture_packs (packs; otherwise it is empty).
JfgOfficialState jfg_official_state(bool scan);
// Downloads and installs the variant from the latest release, in the background: the pack it replaces (an older version
// of the same variant) is deleted, and the choice of pack moves to the new one.
void jfg_official_install(const std::string& variant);
void jfg_official_cancel();

// An installed official pack that is older than the latest release (once GitHub has been asked).
struct JfgOfficialUpdate { std::string variant, installed, latest; };
std::vector<JfgOfficialUpdate> jfg_official_updates();
// The pack in texture_packs (as listed: "texture_packs/<file>") is an official one with a newer version out.
bool jfg_official_outdated(const std::string& folder);
