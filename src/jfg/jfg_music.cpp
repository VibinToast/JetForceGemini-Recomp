// Jet Force Gemini port: custom music from mods (JFG-Studio packs, MODDING_API_PLAN §2.2 "Music and sounds").
//
// A mod ships OGG Vorbis files and says which levels play them, in jfg_music.json:
//   { "tracks": { "<name>": "music/<file>.ogg", ... },
//     "levels": { "<level>": "<name>", ... } }
// <level> is "stock:N" (a stock level number) or the "append" name of a level the same mod adds ("level_x"), which
// the content linker numbered at load (jfg_assets.cpp, asset_number).
//
// While the player is in such a level, the track streams through the port's mixer (queue_samples, runtime/audio.cpp)
// and loops: from the LOOPSTART / LOOPLENGTH comments (in samples, the common convention) or the whole file. The
// game's own tune is silenced meanwhile (its global tune volume set to 0) and given back when a level without a track
// starts. The stream follows the game's music volume (musicVolume, the options menu) and the tune volume the game sets
// for pauses and fades (amTuneSetVolume), so it ducks and pauses where the game's music would.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "jfg_game.h"
#include "jfg_port.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../lib/rt64/src/contrib/stb/stb_vorbis.c"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}
extern "C" void amTuneSetGlobalVolume(uint8_t* rdram, recomp_context* ctx); // the game's tune volume (RecompiledFuncs)

namespace {

constexpr char CONTENT_FILE[] = "jfg_music.json";
constexpr uint32_t ADDR_musicVolume = 0x800FF38A;   // u16, 0-0x100: the options menu's music volume (menu.c)
constexpr uint32_t ADDR_tuneVolume = 0x800A0668;    // u8, 0-0x7F: amTuneSetVolume's value (pauses, fades)
constexpr uint32_t ADDR_tuneGlobal = 0x800A0690;    // s32: amTuneSetGlobalVolume's value (music volume x)

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

bool starts_with_nocase(const char* s, const char* prefix, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!s[i] || std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i])) return false;
    }
    return true;
}

struct MusicTrack {
    std::string mod_id, name;
    std::vector<uint8_t> ogg;
};

struct Assignment {
    std::string mod_id, level, track; // level: "stock:N" or an append name of the mod
};

std::vector<std::unique_ptr<MusicTrack>> g_tracks;
std::vector<Assignment> g_assignments;

// The stream, shared by the game thread (which picks it) and the audio thread (which mixes it).
struct Stream {
    const MusicTrack* track = nullptr;
    stb_vorbis* vorbis = nullptr;
    int channels = 2;
    uint32_t rate = 44100;
    uint32_t loop_start = 0, loop_end = 0; // in samples; loop_end 0 = the end of the file
    uint32_t position = 0;
    std::vector<float> chunk;              // decoded, interleaved stereo, not yet mixed
    size_t chunk_at = 0;
    double phase = 0.0;                    // resampling position between two output frames
    float last[2] = { 0, 0 }, next[2] = { 0, 0 };
    float volume = 0.0f;                   // 0-1, set by the game thread
};
std::mutex g_mutex;
Stream g_stream;

uint32_t comment_number(stb_vorbis* v, const char* key) {
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    const size_t n = std::strlen(key);
    for (int i = 0; i < c.comment_list_length; i++) {
        const char* s = c.comment_list[i];
        if (s && starts_with_nocase(s, key, n) && s[n] == '=') return (uint32_t)std::strtoul(s + n + 1, nullptr, 10);
    }
    return 0;
}

void close_stream() {
    if (g_stream.vorbis) stb_vorbis_close(g_stream.vorbis);
    g_stream = Stream{};
}

bool open_stream(const MusicTrack* t) {
    close_stream();
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(t->ogg.data(), (int)t->ogg.size(), &err, nullptr);
    if (!v) {
        log("music: " + t->mod_id + ": " + t->name + " is not an OGG Vorbis file this decoder reads (error " +
            std::to_string(err) + ")");
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    g_stream.track = t;
    g_stream.vorbis = v;
    g_stream.channels = std::max(1, info.channels);
    g_stream.rate = info.sample_rate;
    const uint32_t total = stb_vorbis_stream_length_in_samples(v);
    const uint32_t start = comment_number(v, "LOOPSTART"), length = comment_number(v, "LOOPLENGTH");
    g_stream.loop_start = start < total ? start : 0;
    g_stream.loop_end = (length && start + length <= total) ? start + length : total;
    return true;
}

// Decodes more of the stream into chunk (stereo); loops at loop_end.
bool refill() {
    Stream& s = g_stream;
    s.chunk.clear();
    s.chunk_at = 0;
    if (!s.vorbis) return false;
    float buf[4096];
    int frames_wanted = 1024;
    if (s.loop_end && s.position + frames_wanted > s.loop_end) frames_wanted = (int)(s.loop_end - s.position);
    int got = frames_wanted > 0
        ? stb_vorbis_get_samples_float_interleaved(s.vorbis, s.channels, buf, frames_wanted * s.channels) : 0;
    if (got <= 0) {
        stb_vorbis_seek(s.vorbis, s.loop_start);
        s.position = s.loop_start;
        got = stb_vorbis_get_samples_float_interleaved(s.vorbis, s.channels, buf,
                                                       std::min(1024, (int)(sizeof buf / sizeof buf[0]) / s.channels) *
                                                           s.channels);
        if (got <= 0) return false;
    }
    s.position += (uint32_t)got;
    s.chunk.resize((size_t)got * 2);
    for (int i = 0; i < got; i++) {
        const float l = buf[i * s.channels];
        const float r = s.channels > 1 ? buf[i * s.channels + 1] : l;
        s.chunk[i * 2] = l;
        s.chunk[i * 2 + 1] = r;
    }
    return true;
}

bool next_frame(float out[2]) {
    Stream& s = g_stream;
    if (s.chunk_at >= s.chunk.size() && !refill()) return false;
    out[0] = s.chunk[s.chunk_at];
    out[1] = s.chunk[s.chunk_at + 1];
    s.chunk_at += 2;
    return true;
}

void on_music_mod_enabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(CONTENT_FILE, exists);
    if (!exists) return;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    const std::string id = mod.manifest.mod_id;
    if (!j.is_object()) {
        log("music: " + id + ": " + CONTENT_FILE + " isn't valid JSON - its music is off");
        return;
    }
    std::map<std::string, const MusicTrack*> mine;
    if (j.contains("tracks") && j["tracks"].is_object()) {
        for (auto& [name, file] : j["tracks"].items()) {
            if (!file.is_string()) continue;
            bool ok = false;
            std::vector<char> bytes = mod.manifest.file_handle->read_file(file.get<std::string>(), ok);
            if (!ok) {
                log("music: " + id + ": " + name + ": " + file.get<std::string>() + " is not in the mod");
                continue;
            }
            auto t = std::make_unique<MusicTrack>();
            t->mod_id = id;
            t->name = name;
            t->ogg.assign(bytes.begin(), bytes.end());
            mine[name] = t.get();
            g_tracks.push_back(std::move(t));
        }
    }
    if (j.contains("levels") && j["levels"].is_object()) {
        for (auto& [level, name] : j["levels"].items()) {
            if (!name.is_string() || !mine.count(name.get<std::string>())) {
                log("music: " + id + ": level " + level + " names a track the mod does not have");
                continue;
            }
            g_assignments.push_back({ id, level, name.get<std::string>() });
        }
    }
    log("music: " + id + ": " + std::to_string(mine.size()) + " tracks");
}

// The track a level plays, or nullptr. Later mods in the list win.
const MusicTrack* track_for(int level) {
    const MusicTrack* found = nullptr;
    for (const Assignment& a : g_assignments) {
        int n = -1;
        if (a.level.rfind("stock:", 0) == 0) n = std::atoi(a.level.c_str() + 6);
        else n = jfg::asset_number(a.mod_id + ":" + a.level);
        if (n != level) continue;
        for (const auto& t : g_tracks) {
            if (t->mod_id == a.mod_id && t->name == a.track) found = t.get();
        }
    }
    return found;
}

} // namespace

extern "C" void jfg_register_music_mods(void) {
    recomp::mods::ModContentType type{
        .content_filename = CONTENT_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_music_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(type);
}

// Mixes the stream into the game's audio (interleaved stereo floats at the game's rate, before the device conversion;
// gain: the port's main volume). Audio thread.
extern "C" void jfg_music_mix(float* samples, size_t frames, uint32_t rate, float gain) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stream& s = g_stream;
    if (!s.vorbis || s.volume <= 0.0f || rate == 0) return;
    const float v = s.volume * gain * 0.5f; // 0.5: the headroom the game's own samples get
    static const MusicTrack* reported = nullptr;
    if (reported != s.track) {
        reported = s.track;
        log("music: mixing " + s.track->name + " (" + std::to_string(s.rate) + " Hz) into the game's " +
            std::to_string(rate) + " Hz, volume " + std::to_string((int)(v * 200)) + "%");
    }
    const double step = (double)s.rate / (double)rate;
    for (size_t i = 0; i < frames; i++) {
        s.phase += step;
        while (s.phase >= 1.0) {
            s.last[0] = s.next[0];
            s.last[1] = s.next[1];
            if (!next_frame(s.next)) return;
            s.phase -= 1.0;
        }
        const float t = (float)s.phase;
        samples[i * 2] += (s.last[0] + (s.next[0] - s.last[0]) * t) * v;
        samples[i * 2 + 1] += (s.last[1] + (s.next[1] - s.last[1]) * t) * v;
    }
}

namespace jfg {

// Every game tick: starts or stops the level's track and keeps the game's tune silent while one plays.
void music_tick(recomp_context* ctx) {
    static uint32_t saved_global = 0x100;
    if (g_assignments.empty()) return;
    const int level = (int)(int16_t)rd16(ADDR_currentLevel);
    const MusicTrack* want = track_for(level);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (want != g_stream.track) {
        if (want) {
            if (open_stream(want)) {
                log("music: level " + std::to_string(level) + " plays " + want->mod_id + ":" + want->name);
            }
        } else {
            close_stream();
            call_game(amTuneSetGlobalVolume, ctx, { saved_global });  // the game's tune back at the player's volume
            log("music: level " + std::to_string(level) + " plays the game's tune");
        }
    }
    if (g_stream.track) {
        const uint32_t music = rd16(ADDR_musicVolume);
        saved_global = music;
        if (rd32(ADDR_tuneGlobal) != 0) call_game(amTuneSetGlobalVolume, ctx, { 0u });
        const float tune = std::min(1.0f, (float)rd8(ADDR_tuneVolume) / 127.0f);
        g_stream.volume = std::min(1.0f, (float)music / 256.0f) * tune;
    }
}

} // namespace jfg
