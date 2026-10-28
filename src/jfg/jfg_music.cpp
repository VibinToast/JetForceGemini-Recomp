// Jet Force Gemini port: custom music and sound effects from mods (JFG-Studio packs, MODDING_API_PLAN §2.2 "Music and
// sounds").
//
// Sounds: a mod ships OGG Vorbis or 16-bit PCM WAV files and names them in jfg_sounds.json:
//   { "sounds": { "<name>": "sounds/<file>.ogg" (or .wav), ... } }
// and plays them by name with jfg_sound_play / jfg_sound_loop (mod_api/jfg_api.h), at a volume, a pan and a pitch it
// can change while they play (jfg_sound_set). They are decoded when the mod is enabled and mixed with the game's
// samples at the game's sound effects volume (the options menu), on top of the port's main volume.
//
// The same file can replace the game's own sound effects, by id (the ids JFG_DEBUG_SOUNDS logs):
//   "replace": { "0xE1": "<name>", "0xFA": { "sound": "<name>", "loop": true, "volume": 0.8 }, ... }
// The game still plays its sound, so its handle, its looping and its stopping work as before, but silently: the
// replaced ids' volume in the game's sound table (sfxIndex, and the placed sounds' copy at D_800F29F0; 10 bytes a
// sound, +2 volume, +3 minimum volume) is 0, set again every tick. In its place the mod's sound plays (amSndPlay,
// amSndPlayXYZ: sound_replaced_play), a placed one quieter with its distance from the hero over the game's range for
// it (+6). A plain one-shot plays out; a "loop" started with a handle stops when the game stops it or the handle is
// cleared (sound_replace_tick). A placed one can't go by its handle: the placed sounds' manager
// lets go of a silent point at once, clearing it, so it stops when the game stops it by that handle (amSndStopXYZ:
// sound_replaced_stop), when the same id starts again, or when one of its "stopped_by" ids plays ("0xFA": { "sound":
// "charge", "stopped_by": [ "0xE1" ] }: the shot ends the charge). A placed sound already playing on its handle is not
// started again (the game doesn't).
//
// Music:
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
#include "librecomp/helpers.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "jfg_game.h"
#include "jfg_port.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../lib/rt64/src/contrib/stb/stb_vorbis.c"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}
extern "C" void amTuneSetGlobalVolume(uint8_t* rdram, recomp_context* ctx); // the game's tune volume (RecompiledFuncs)
extern "C" void frontGetSfxVolume(uint8_t* rdram, recomp_context* ctx);     // the options menu's sound effects volume

namespace {

constexpr char CONTENT_FILE[] = "jfg_music.json";
constexpr uint32_t ADDR_musicVolume = 0x800FF38A;   // u16, 0-0x100: the options menu's music volume
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

// A script's choice for the current level (jfg.music / jfg.music_stop, jfg_lua.cpp): (mod, track), level.
std::string g_forced_mod, g_forced_name;
int g_forced_level = -1;

// The track a level plays, or nullptr. Later mods in the list win.
const MusicTrack* track_for(int level) {
    if (level == g_forced_level && !g_forced_name.empty()) {
        for (const auto& t : g_tracks) {
            if (t->mod_id == g_forced_mod && t->name == g_forced_name) return t.get();
        }
    }
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


// --- sound effects ------------------------------------------------------------------------------------------------

constexpr const char* SOUNDS_FILE = "jfg_sounds.json";

struct ModSound {
    std::string key;        // "<mod id>:<name>"
    std::vector<float> pcm; // interleaved, channels per frame
    int channels = 1;
    uint32_t rate = 32000;
    size_t frames = 0;
};
std::vector<std::unique_ptr<ModSound>> g_sounds; // filled as mods are enabled, read-only while the game runs

// The game's sound effects replaced by mods' (jfg_sounds.json "replace"), by id. The first mod to name an id wins.
struct Replacement {
    const ModSound* sound = nullptr;
    bool loop = false;
    float volume = 1.0f;
    uint16_t range = 0; // the game's range for it (placed sounds), read before it is muted
    std::vector<uint32_t> stopped_by; // ids whose playing stops this one's voices
    bool enabled = true;  // off (jfg_sound_replace_enable): the game's own sound plays again
    bool saved = false;   // the game's volume and minimum volume for it, kept to give back
    uint8_t volume_game = 0, minimum_game = 0;
};
std::map<uint32_t, Replacement> g_replace;

// A playing sound. id 0 = a free slot.
struct Voice {
    const ModSound* sound = nullptr;
    double position = 0.0; // in the sound's frames
    float volume = 1.0f, pan = 0.0f, pitch = 1.0f;
    bool loop = false;
    uint32_t id = 0;
};
constexpr int VOICES = 32;
std::mutex g_voice_mutex;
Voice g_voices[VOICES];
uint32_t g_next_voice = 1;
float g_sfx_volume = 1.0f; // the game's sound effects volume, 0-1 (game thread writes, audio thread reads)

// A 16-bit PCM WAV file, as stb_vorbis_decode_memory gives an OGG: frames, and malloc'd interleaved samples.
int decode_wav(const std::vector<char>& b, int* channels, int* rate, short** out) {
    auto u16 = [&](size_t at) { return (uint32_t)(uint8_t)b[at] | (uint32_t)(uint8_t)b[at + 1] << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (b.size() < 12 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return -1;
    int ch = 0, bits = 0, format = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t size = u32(at + 4);
        const size_t body = at + 8;
        if (body + size > b.size()) return -1;
        if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && size >= 16) {
            format = (int)u16(body);
            ch = (int)u16(body + 2);
            *rate = (int)u32(body + 4);
            bits = (int)u16(body + 14);
        } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
            if (format != 1 || bits != 16 || ch <= 0) return -1;
            const int frames = (int)(size / (2u * (uint32_t)ch));
            short* s = (short*)malloc((size_t)frames * ch * sizeof(short));
            if (!s) return -1;
            for (int i = 0; i < frames * ch; i++) s[i] = (short)u16(body + 2 * (size_t)i);
            *channels = ch;
            *out = s;
            return frames;
        }
        at = body + size + (size & 1);
    }
    return -1;
}

void on_sound_mod_enabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(SOUNDS_FILE, exists);
    if (!exists) return;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    const std::string id = mod.manifest.mod_id;
    if (!j.is_object() || !j.contains("sounds") || !j["sounds"].is_object()) {
        log("sounds: " + id + ": " + SOUNDS_FILE + " has no \"sounds\" object - its sounds are off");
        return;
    }
    int count = 0;
    for (auto& [name, file] : j["sounds"].items()) {
        if (!file.is_string()) continue;
        bool ok = false;
        const std::string path = file.get<std::string>();
        std::vector<char> bytes = mod.manifest.file_handle->read_file(path, ok);
        if (!ok && path.find_last_of("/\\") != std::string::npos) {
            // RecompModTool packs a code mod's additional files without their folders
            bytes = mod.manifest.file_handle->read_file(path.substr(path.find_last_of("/\\") + 1), ok);
        }
        if (!ok) {
            log("sounds: " + id + ": " + name + ": " + path + " is not in the mod");
            continue;
        }
        int channels = 0, rate = 0;
        short* samples = nullptr;
        const int frames = bytes.size() > 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0
            ? decode_wav(bytes, &channels, &rate, &samples)
            : stb_vorbis_decode_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &channels, &rate, &samples);
        if (frames <= 0 || channels <= 0 || !samples) {
            log("sounds: " + id + ": " + name + " is neither an OGG Vorbis file nor a 16-bit PCM WAV file");
            continue;
        }
        auto snd = std::make_unique<ModSound>();
        snd->key = id + ":" + name;
        snd->channels = std::min(channels, 2);
        snd->rate = (uint32_t)rate;
        snd->frames = (size_t)frames;
        snd->pcm.resize((size_t)frames * snd->channels);
        for (size_t f = 0; f < (size_t)frames; f++) {
            for (int c = 0; c < snd->channels; c++) snd->pcm[f * snd->channels + c] = samples[f * channels + c] / 32768.0f;
        }
        free(samples);
        g_sounds.push_back(std::move(snd));
        count++;
    }
    log("sounds: " + id + ": " + std::to_string(count) + " sounds");
    if (!j.contains("replace") || !j["replace"].is_object()) return;
    int replaced = 0;
    for (auto& [key, value] : j["replace"].items()) {
        const uint32_t sound_id = (uint32_t)std::strtoul(key.c_str(), nullptr, 0);
        std::string name;
        Replacement r;
        if (value.is_string()) {
            name = value.get<std::string>();
        } else if (value.is_object() && value.contains("sound") && value["sound"].is_string()) {
            name = value["sound"].get<std::string>();
            if (value.contains("loop") && value["loop"].is_boolean()) r.loop = value["loop"].get<bool>();
            if (value.contains("volume") && value["volume"].is_number()) r.volume = std::max(0.0f, value["volume"].get<float>());
            if (value.contains("stopped_by") && value["stopped_by"].is_array()) {
                for (const auto& v : value["stopped_by"]) {
                    if (v.is_string()) r.stopped_by.push_back((uint32_t)std::strtoul(v.get<std::string>().c_str(), nullptr, 0));
                }
            }
        }
        const std::string full = id + ":" + name;
        for (const auto& snd : g_sounds) {
            if (snd->key == full) r.sound = snd.get();
        }
        if (!r.sound || sound_id == 0 || sound_id > 0xFFFF) {
            log("sounds: " + id + ": replace " + key + ": no sound " + name + " in the mod, or not a sound id");
            continue;
        }
        if (g_replace.count(sound_id)) {
            log("sounds: " + id + ": replace " + key + ": another mod replaces it already");
            continue;
        }
        g_replace[sound_id] = r;
        replaced++;
    }
    log("sounds: " + id + ": replaces " + std::to_string(replaced) + " of the game's sounds");
}

const ModSound* find_sound(size_t mod_index, const std::string& name) {
    const std::string key = recomp::mods::get_mod_id(mod_index) + ":" + name;
    for (const auto& s : g_sounds) {
        if (s->key == key) return s.get();
    }
    return nullptr;
}

// A float the MIPS ABI passes in a1-a3 (it follows an int or a pointer, so it goes in the integer registers).
float float_arg(recomp_context* ctx, int index) {
    const uint32_t bits = (uint32_t)(&ctx->r4)[index];
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// A voice for snd; the oldest makes room when all are in use. Its id.
uint32_t start_voice(const ModSound* snd, float volume, float pan, float pitch, bool loop) {
    std::lock_guard<std::mutex> lock(g_voice_mutex);
    Voice* slot = nullptr;
    for (Voice& v : g_voices) {
        if (v.id == 0) { slot = &v; break; }
        if (!slot || v.id < slot->id) slot = &v;
    }
    *slot = Voice{ snd, 0.0, std::max(0.0f, volume), std::clamp(pan, -1.0f, 1.0f), std::max(0.01f, pitch), loop,
                   g_next_voice++ };
    return slot->id;
}

void stop_voice(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_voice_mutex);
    for (Voice& v : g_voices) {
        if (id != 0 && v.id == id) v = Voice{};
    }
}

// Replaced sounds started with a handle: stopped when the handle no longer holds what the game put there.
struct Tracked {
    uint32_t handle_addr, handle, voice, id, tick;
    bool placed; // a placed sound: not stopped by its handle clearing
};
std::vector<Tracked> g_tracked;

// jfg_sound_play / jfg_sound_loop(const char* name, float volume, float pan, float pitch): a voice id, 0 when the mod
// has no such sound. The oldest voice makes room when all are in use.
void start_sound(uint8_t* rdram, recomp_context* ctx, size_t mod_index, bool loop) {
    const std::string name = _arg_string<0>(rdram, ctx);
    const ModSound* snd = find_sound(mod_index, name);
    if (!snd) {
        static std::map<std::string, bool> reported;
        if (!reported[name]) {
            reported[name] = true;
            log("sounds: " + recomp::mods::get_mod_id(mod_index) + " played " + name + ", which it does not have");
        }
        ctx->r2 = 0;
        return;
    }
    ctx->r2 = (gpr)start_voice(snd, float_arg(ctx, 1), float_arg(ctx, 2), float_arg(ctx, 3), loop);
}

void sound_play(uint8_t* rdram, recomp_context* ctx, size_t mod_index) { start_sound(rdram, ctx, mod_index, false); }
void sound_loop(uint8_t* rdram, recomp_context* ctx, size_t mod_index) { start_sound(rdram, ctx, mod_index, true); }

// jfg_sound_set(int voice, float volume, float pan, float pitch): 1 while it plays, else 0.
void sound_set(uint8_t*, recomp_context* ctx) {
    const uint32_t id = (uint32_t)ctx->r4;
    std::lock_guard<std::mutex> lock(g_voice_mutex);
    for (Voice& v : g_voices) {
        if (id == 0 || v.id != id) continue;
        v.volume = std::max(0.0f, float_arg(ctx, 1));
        v.pan = std::clamp(float_arg(ctx, 2), -1.0f, 1.0f);
        v.pitch = std::max(0.01f, float_arg(ctx, 3));
        ctx->r2 = 1;
        return;
    }
    ctx->r2 = 0;
}

// jfg_sound_stop(int voice)
void sound_stop(uint8_t*, recomp_context* ctx) {
    stop_voice((uint32_t)ctx->r4);
}

// jfg_sound_replace_enable(int id, int on): a replacement (jfg_sounds.json "replace") on or off while the game runs; off,
// the game's own sound plays again (its volume given back next tick) and the stand-in's voices stop. 1 if the id is
// replaced, else 0.
void sound_replace_enable(uint8_t*, recomp_context* ctx) {
    const uint32_t id = (uint32_t)ctx->r4 & 0xFFFF;
    const bool on = ctx->r5 != 0;
    const auto it = g_replace.find(id);
    if (it == g_replace.end()) { ctx->r2 = 0; return; }
    if (it->second.enabled && !on) {
        for (size_t i = 0; i < g_tracked.size();) {
            if (g_tracked[i].id != id) { i++; continue; }
            stop_voice(g_tracked[i].voice);
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
        }
    }
    it->second.enabled = on;
    ctx->r2 = 1;
}

constexpr uint32_t ADDR_sfxIndex = 0x800E9508;     // SoundData*: amSndPlay's table
constexpr uint32_t ADDR_sfxIndexXYZ = 0x800F29F0;  // SoundData*: amSndPlayXYZ's (the same table, kept apart)
constexpr uint32_t ADDR_maxSound = 0x800E9510;     // s32: the highest id
constexpr uint32_t SOUND_DATA = 10;

// The replaced ids silent in the game's tables (their range and volumes kept first), or given their volumes back while
// a mod has turned the replacement off.
void mute_replaced() {
    using namespace jfg;
    const int max = (int)rd32(ADDR_maxSound);
    for (uint32_t table : { rd32(ADDR_sfxIndex), rd32(ADDR_sfxIndexXYZ) }) {
        if (!valid_ptr(table)) continue;
        for (auto& [id, r] : g_replace) {
            if ((int)id > max) continue;
            const uint32_t e = table + id * SOUND_DATA;
            if (!r.saved && rd8(e + 2) != 0) {
                r.range = rd16(e + 6);
                r.volume_game = rd8(e + 2);
                r.minimum_game = rd8(e + 3);
                r.saved = true;
            }
            if (r.enabled) {
                wr8(e + 2, 0);
                wr8(e + 3, 0);
            } else if (r.saved) {
                wr8(e + 2, r.volume_game);
                wr8(e + 3, r.minimum_game);
            }
        }
    }
}
} // namespace

// The game's amSndPlay / amSndPlayXYZ wrappers, after the game's code: a replaced id plays the mod's sound. handle_addr:
// where the game keeps the sound's handle (0 none); was_playing: that handle was set before the call (a placed sound
// the game then didn't start again). For a placed sound, x, y, z.
namespace jfg {
void sound_replaced_play(recomp_context* ctx, uint32_t id, uint32_t handle_addr, bool was_playing, bool placed,
                         float x, float y, float z) {
    if (g_replace.empty()) return;
    id &= 0xFFFF;
    // Any sound, replaced or not, ends the voices of the replacements it stops.
    for (const auto& [rid, rep] : g_replace) {
        if (std::find(rep.stopped_by.begin(), rep.stopped_by.end(), id) == rep.stopped_by.end()) continue;
        for (size_t i = 0; i < g_tracked.size();) {
            if (g_tracked[i].id != rid) { i++; continue; }
            stop_voice(g_tracked[i].voice);
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
        }
    }
    const auto it = g_replace.find(id);
    if (it == g_replace.end() || was_playing || !it->second.enabled) return;
    const Replacement& r = it->second;
    if (placed) { // the same placed sound again: the older one makes way
        for (size_t i = 0; i < g_tracked.size();) {
            if (g_tracked[i].id != id || !g_tracked[i].placed) { i++; continue; }
            stop_voice(g_tracked[i].voice);
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
        }
    }
    float volume = r.volume;
    if (placed) {
        const uint32_t hero = player_object(ctx);
        if (valid_ptr(hero) && r.range > 0) {
            const float dx = x - Obj::PosX::get(hero), dy = y - Obj::PosY::get(hero), dz = z - Obj::PosZ::get(hero);
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            volume *= std::clamp(1.0f - d / (float)r.range, 0.0f, 1.0f);
        }
        if (volume <= 0.01f) return;
    }
    const uint32_t voice = start_voice(r.sound, volume, 0.0f, 1.0f, r.loop);
    static const bool debug = std::getenv("JFG_DEBUG_SOUNDS") != nullptr;
    if (debug) std::fprintf(stderr, "[jfg] sound 0x%X replaced by %s (volume %.2f%s)\n", id & 0xFFFF, r.sound->key.c_str(), volume, r.loop ? ", loop" : "");
    if (valid_ptr(handle_addr)) {
        const uint32_t handle = rd32(handle_addr);
        // Only a loop goes by the handle: a one-shot plays out (its tail would be cut when the game reuses the handle).
        if (handle != 0 && (r.loop || placed)) g_tracked.push_back({ handle_addr, handle, voice, id, jfg::g_tick_count, placed });
        else if (handle == 0 && r.loop) stop_voice(voice); // the game has nothing playing to end the loop
        else if (!r.stopped_by.empty()) g_tracked.push_back({ 0, 0, voice, id, jfg::g_tick_count, true });
    } else if (placed || !r.stopped_by.empty()) {
        g_tracked.push_back({ 0, 0, voice, id, jfg::g_tick_count, true }); // for stopped_by and restarts
    }
}

// amSndStopXYZ(mask)'s wrapper (jfg_diagnostics.cpp), before the game's code: the stand-ins started on that handle stop.
void sound_replaced_stop(uint32_t mask) {
    if (mask == 0) return;
    for (size_t i = 0; i < g_tracked.size();) {
        if (g_tracked[i].handle != mask) { i++; continue; }
        stop_voice(g_tracked[i].voice);
        g_tracked[i] = g_tracked.back();
        g_tracked.pop_back();
    }
}
} // namespace jfg

extern "C" void jfg_register_music_mods(void) {
    recomp::mods::ModContentType type{
        .content_filename = CONTENT_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_music_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(type);
    recomp::mods::ModContentType sounds{
        .content_filename = SOUNDS_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_sound_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(sounds);
    recomp::overlays::register_ext_base_export("jfg_sound_play", sound_play);
    recomp::overlays::register_ext_base_export("jfg_sound_loop", sound_loop);
    recomp::overlays::register_base_export("jfg_sound_set", sound_set);
    recomp::overlays::register_base_export("jfg_sound_stop", sound_stop);
    recomp::overlays::register_base_export("jfg_sound_replace_enable", sound_replace_enable);
}

// Mixes the mods' playing sounds into the game's audio (as jfg_music_mix): linear resampling to the game's rate times
// the pitch, an equal-power pan. Audio thread.
extern "C" void jfg_sounds_mix(float* samples, size_t frames, uint32_t rate, float gain) {
    if (rate == 0) return;
    std::lock_guard<std::mutex> lock(g_voice_mutex);
    const float sfx = g_sfx_volume * gain * 0.5f; // 0.5: the headroom the game's own samples get
    for (Voice& v : g_voices) {
        if (v.id == 0 || !v.sound) continue;
        const ModSound& s = *v.sound;
        const double step = (double)s.rate * v.pitch / (double)rate;
        const float angle = (v.pan + 1.0f) * 0.25f * 3.14159265f;
        const float gl = std::cos(angle) * 1.41421356f * v.volume * sfx;
        const float gr = std::sin(angle) * 1.41421356f * v.volume * sfx;
        for (size_t i = 0; i < frames; i++) {
            if (v.position >= (double)(s.frames - 1)) {
                if (!v.loop || s.frames < 2) { v = Voice{}; break; }
                v.position -= (double)(s.frames - 1);
            }
            const size_t f = (size_t)v.position;
            const float t = (float)(v.position - (double)f);
            float l, r;
            if (s.channels == 2) {
                l = s.pcm[f * 2] + (s.pcm[f * 2 + 2] - s.pcm[f * 2]) * t;
                r = s.pcm[f * 2 + 1] + (s.pcm[f * 2 + 3] - s.pcm[f * 2 + 1]) * t;
            } else {
                l = r = s.pcm[f] + (s.pcm[f + 1] - s.pcm[f]) * t;
            }
            samples[i * 2] += l * gl;
            samples[i * 2 + 1] += r * gr;
            v.position += step;
        }
    }
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

// A script asks for one of its mod's tracks in the current level ("" = the level's own music again).
void music_force(const std::string& mod_id, const std::string& name) {
    g_forced_mod = mod_id;
    g_forced_name = name;
    g_forced_level = name.empty() ? -1 : (int)(int16_t)rd16(ADDR_currentLevel);
}

// Every game tick: the replaced sounds kept silent, and their stand-ins stopped with them.
void sound_replace_tick() {
    if (g_replace.empty()) return;
    mute_replaced();
    for (size_t i = 0; i < g_tracked.size();) {
        const Tracked& t = g_tracked[i];
        if (t.placed || rd32(t.handle_addr) == t.handle) { i++; continue; }
        static const bool debug = std::getenv("JFG_DEBUG_SOUNDS") != nullptr;
        if (debug) std::fprintf(stderr, "[jfg] sound 0x%X: the game's ended after %u ticks, its stand-in stops\n", t.id, jfg::g_tick_count - t.tick);
        stop_voice(t.voice);
        g_tracked[i] = g_tracked.back();
        g_tracked.pop_back();
    }
    // Placed and stopped_by entries whose voice has ended, and any never stopped, are forgotten.
    {
        std::lock_guard<std::mutex> lock(g_voice_mutex);
        for (size_t i = 0; i < g_tracked.size();) {
            bool alive = false;
            for (const Voice& v : g_voices) alive = alive || v.id == g_tracked[i].voice;
            if (alive || !g_tracked[i].placed) { i++; continue; }
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
        }
    }
    if (g_tracked.size() > 64) g_tracked.erase(g_tracked.begin(), g_tracked.begin() + 32);
}

// Every game tick: starts or stops the level's track and keeps the game's tune silent while one plays.
void music_tick(recomp_context* ctx) {
    static uint32_t saved_global = 0x100;
    sound_replace_tick();
    if (!g_sounds.empty()) {
        const recomp_context saved = *ctx;
        const uint32_t sfx = call_game(frontGetSfxVolume, ctx, {});
        *ctx = saved;
        g_sfx_volume = std::min(1.0f, (float)(sfx & 0xFFFF) / 256.0f); // mods' sounds follow it (jfg_sounds_mix)
    }
    if (g_assignments.empty() && g_forced_name.empty()) return;
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
