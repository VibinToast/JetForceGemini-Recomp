// Jet Force Gemini port, build check: the game's pre-linker (src/jfg/jfg_prelink.cpp, used for mods' hooks) must give
// exactly the bytes tools/prepare_overlays.py gave the recompiler. A difference means a mod hook on an overlay function
// would be regenerated from other code than the game runs, so the build stops.
//
// Usage: jfg_prelink_check <baserom.us.z64> <baserom.us.recomp.z64 (prepare_overlays.py's output)>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "jfg/jfg_prelink.h"

static bool read_file(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: jfg_prelink_check <baserom.us.z64> <baserom.us.recomp.z64>\n");
        return 2;
    }
    std::vector<uint8_t> rom, expected, linked;
    if (!read_file(argv[1], rom) || !read_file(argv[2], expected)) {
        std::fprintf(stderr, "prelink check: can't read %s or %s\n", argv[1], argv[2]);
        return 2;
    }
    const int n = jfg::prelink::link(rom, linked);
    if (n < 0) {
        std::fprintf(stderr, "prelink check: %s doesn't look like the US ROM\n", argv[1]);
        return 1;
    }
    if (linked.size() != expected.size()) {
        std::fprintf(stderr, "prelink check: FAILED - sizes differ (%zu and %zu bytes)\n", linked.size(), expected.size());
        return 1;
    }
    size_t changed = 0, differ = 0;
    for (size_t i = 0; i < linked.size(); i++) {
        changed += linked[i] != rom[i];
        if (linked[i] != expected[i]) {
            if (differ < 8) {
                std::fprintf(stderr, "prelink check: ROM 0x%08zX: game's pre-linker %02X, prepare_overlays.py %02X\n", i,
                             linked[i], expected[i]);
            }
            differ++;
        }
    }
    if (differ != 0) {
        std::fprintf(stderr,
                     "prelink check: FAILED - %zu bytes differ between src/jfg/jfg_prelink.cpp and tools/prepare_overlays.py;\n"
                     "  they must link the overlays the same way (mods' hooks regenerate overlay code from the game's copy)\n",
                     differ);
        return 1;
    }
    std::printf("prelink check: ok - both pre-linkers agree (%d overlays, %zu bytes changed)\n", n, changed);
    return 0;
}
