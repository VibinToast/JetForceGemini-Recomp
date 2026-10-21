// JFG PORT: update check and installer for the launcher (src/jfg/jfg_update_check.cpp, src/ui/ui_launcher.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// The launcher's own settings, bin\jfg_launcher.txt (written with explanations when it is missing):
//   check_for_updates=1|0   0: the launcher makes no requests to GitHub (no update check, no list of recent changes);
//   rom=<path>              a ROM to start the game with straight away, without the launcher (relative to bin);
//   skip_launcher=0|1       1: start the game straight away with the ROM loaded in the launcher (its Play page sets it);
//   show_official_pack=1|0  0: the launcher's Texture Packs page hides the official HD pack (and does not ask GitHub).
// Read once at start-up (src/main.cpp), before the launcher or the update check runs.
void jfg_launcher_config_load(const std::filesystem::path& app_folder);
bool jfg_launcher_check_updates();
void jfg_launcher_set_check_updates(bool check); // and saves it to the file (the check itself runs at start-up)
bool jfg_launcher_skip();
void jfg_launcher_set_skip(bool skip); // and saves it to the file
bool jfg_launcher_show_official_pack();
void jfg_launcher_set_show_official_pack(bool show); // and saves it to the file
std::string jfg_launcher_config_rom(); // an absolute path, or "" when none is set

struct JfgUpdateInfo {
    std::string version;    // the release it would update to, like "1.2.0"
    std::string changelog;  // the notes of every release newer than this build, newest first, as markdown (jfg_changelog_rml)
    bool can_install = false; // the release has a build for this system and a checksum for it
    std::string unavailable;  // when !can_install: why, for the dialog
};

struct JfgUpdateProgress {
    int stage = 0;          // 0 = not started, 1 = working, 2 = failed, 3 = finished: call jfg_update_restart() and quit
    int percent = 0;        // 0..100
    std::string text;       // what it is doing, or what went wrong
};

// Asks GitHub for the releases in the background (once). `current` is this build's version, like "1.0.0".
void jfg_update_check_start(const std::string& current);
// True once a newer release has been found; fills in `info`.
bool jfg_update_available(JfgUpdateInfo& info);
// Downloads, checks, unpacks and installs the release found, in the background. Progress: jfg_update_progress().
void jfg_update_install();
JfgUpdateProgress jfg_update_progress();
// Clears a failed attempt (stage 2) so the dialog starts clean when it is opened again.
void jfg_update_reset();
// Puts the dialog in the failed state with this message (the update could not even start).
void jfg_update_fail(const std::string& message);
// After stage 3: starts the new build. With no update waiting (and always on Linux) it starts this build again with the
// same arguments. The caller then quits this one.
void jfg_update_restart();
// Starts the game again, on the launcher (--launcher). The caller then quits this run. False if it could not start it.
bool jfg_restart_into_launcher();

// The markdown of CHANGELOG.md that ships in assets/jfg_launcher, from its first version heading (newest first).
std::string jfg_changelog_text();

// The commits made since the last release, from GitHub (empty until they have arrived, or when offline), as markdown: a
// "## Since the last release" heading and one "- " line each. Commits by GitHub's bots (the release commit of the Release
// workflow) are left out.
std::string jfg_changelog_commits();

// Changelog markdown (the two above, JfgUpdateInfo::changelog) as RML for the launcher's dialogs, with the version
// headings tagged Installed / New against `installed`.
std::string jfg_changelog_rml(const std::string& markdown, const std::string& installed);

// For the texture pack installer (jfg_pack_install.cpp), over https:
// The text at url (a GitHub API answer), false when it could not be had.
bool jfg_http_get_text(const std::string& url, std::string& out);
// Downloads url into file, carrying on from what file already holds. progress(done, total) returns false to stop.
// 1 = done, 0 = failed (what arrived is kept, to carry on from), -1 = stopped by progress.
int jfg_http_download(const std::string& url, const std::filesystem::path& file, uint64_t expected_size,
                      const std::function<bool(uint64_t done, uint64_t total)>& progress);
// The SHA-256 of a file, as lower case hex.
bool jfg_sha256_file(const std::filesystem::path& file, std::string& hex);
