// JFG PORT: update check and installer for the launcher (src/jfg/jfg_update_check.cpp, src/ui/ui_launcher.cpp).
#pragma once
#include <string>

struct JfgUpdateInfo {
    std::string version;    // the release it would update to, like "1.2.0"
    std::string changelog;  // the notes of every release newer than this build, newest first
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
// After stage 3: starts the new build. The caller then quits this one.
void jfg_update_restart();

// The text of CHANGELOG.md that ships in assets/jfg_launcher, tidied for showing as plain text (newest first).
std::string jfg_changelog_text();
