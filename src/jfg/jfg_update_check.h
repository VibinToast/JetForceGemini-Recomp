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
//   update_channel=stable|testing  testing: test builds (GitHub prereleases) are offered too. Not in the file: testing
//                           when this build is a test build, else stable.
// Read once at start-up (src/main.cpp), before the launcher or the update check runs.
void jfg_launcher_config_load(const std::filesystem::path& app_folder);
bool jfg_launcher_check_updates();
void jfg_launcher_set_check_updates(bool check); // and saves it to the file (the check itself runs at start-up)
bool jfg_launcher_skip();
void jfg_launcher_set_skip(bool skip); // and saves it to the file
bool jfg_launcher_show_official_pack();
void jfg_launcher_set_show_official_pack(bool show); // and saves it to the file
std::string jfg_launcher_config_rom(); // an absolute path, or "" when none is set
bool jfg_launcher_testing_channel(); // update_channel=testing
void jfg_launcher_set_testing_channel(bool testing); // saves it, and picks what to offer again (no new request to GitHub)

struct JfgUpdateInfo {
    std::string version;    // the release it would update to, like "1.2.0" (a test build: "1.4.0-test.4")
    std::string changelog;  // the notes of every release newer than this build, newest first, as markdown (jfg_changelog_rml)
    bool can_install = false; // the release has a build for this system and a checksum for it
    std::string unavailable;  // when !can_install: why, for the dialog
    bool test_build = false;     // it is a test build (a GitHub prerelease, offered on the Testing channel only)
    bool back_to_stable = false; // this is a test build on the Stable channel and `version` is the latest release, older
                                 // or not: installing it goes back (changelog: that release's notes only)
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
// <0, 0, >0 as a is older, the same, newer than b: "1.2.10" > "1.2.9", and a test build comes before its release
// (1.3.0 < 1.4.0-test.1 < 1.4.0-test.4 < 1.4.0). A leading "v" and missing parts (1.2 = 1.2.0) are fine.
int jfg_compare_versions(const std::string& a, const std::string& b);
// Goes up each time the answer of jfg_update_available may have changed (the check finished, the channel changed).
int jfg_update_serial();
// This build's version as the update check and the launcher's version line use it: the project version, or
// JFG_DEBUG_FAKE_VERSION when set (a test switch, to try the channels as a test build would).
std::string jfg_update_current_version(const std::string& project_version);
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

// What is coming after this build, from GitHub (empty until it has arrived, or when offline), as markdown. When a newer
// release is in testing and its branch (release/<x.y>) has hand-written notes in CHANGELOG.md's "## Unreleased"
// section: a "## Coming in x.y" heading and those notes. Otherwise the commits made since the last release: a
// "## Since the last release" heading and one "- " line each, leaving out commits by GitHub's bots (the release commit
// of the Release workflow).
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
