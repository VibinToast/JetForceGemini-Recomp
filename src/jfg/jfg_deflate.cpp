// Jet Force Gemini port: raw deflate for asset mods (jfg_assets.cpp packs a model or track entry again after its
// fixups). A file of its own because librecomp/mods.hpp turns miniz's compression functions off
// (MINIZ_NO_DEFLATE_APIS) in every file that includes it; the library itself has them.
#include <cstdint>
#include <vector>

#include "miniz.h"

namespace jfg {

// Raw deflate (no zlib header) of data at level 9, as the decomp's packer; empty when it fails.
std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& data) {
    mz_stream z{};
    if (mz_deflateInit2(&z, 9, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK) return {};
    std::vector<uint8_t> out(mz_deflateBound(&z, (mz_ulong)data.size()));
    z.next_in = data.data();
    z.avail_in = (unsigned int)data.size();
    z.next_out = out.data();
    z.avail_out = (unsigned int)out.size();
    const int status = mz_deflate(&z, MZ_FINISH);
    out.resize(out.size() - z.avail_out);
    mz_deflateEnd(&z);
    if (status != MZ_STREAM_END) return {};
    return out;
}

} // namespace jfg
