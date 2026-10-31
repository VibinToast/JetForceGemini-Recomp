// Jet Force Gemini port: text for the mod overlay (jfg_overlay.cpp, jfg_overlay_text): the port's own fonts
// (assets/, the menus' and the launcher's), measured on the game thread and turned into pictures on the present thread
// with stb_truetype (ImGui's copy, compiled here on its own), at the window's pixel size so text is sharp at any
// resolution. UTF-8; characters a font lacks are left out.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../runtime/support.hpp"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imgui/imstb_truetype.h"

namespace jfg::overlay {
namespace {
// By jfg_overlay_text's font: 0 Lato Bold (the menus' font), 1 JFG Bold (the launcher's, after the game's lettering),
// 2 Lato Regular.
const char* const FONT_FILES[] = { "LatoLatin-Bold.ttf", "jfg_launcher/JFG-Bold.ttf", "LatoLatin-Regular.ttf" };
constexpr int FONT_COUNT = sizeof(FONT_FILES) / sizeof(FONT_FILES[0]);

struct Font {
    bool tried = false, ok = false;
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, gap = 0;
};
Font g_fonts[FONT_COUNT];
std::mutex g_font_mutex;

Font* font(int id) {
    if (id < 0 || id >= FONT_COUNT) id = 0;
    Font& f = g_fonts[id];
    if (!f.tried) {
        f.tried = true;
        const std::filesystem::path path = dino::runtime::get_asset_path(FONT_FILES[id]);
        if (FILE* file = std::fopen(path.string().c_str(), "rb")) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            f.data.resize(size > 0 ? (size_t)size : 0);
            const bool read = size > 0 && std::fread(f.data.data(), 1, f.data.size(), file) == f.data.size();
            std::fclose(file);
            if (read && stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
                stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.gap);
                f.ok = true;
            }
        }
        if (!f.ok) std::fprintf(stderr, "[overlay] font %s not loaded\n", path.string().c_str());
    }
    return f.ok ? &f : (id != 0 ? font(0) : nullptr);
}

// The next code point of UTF-8 text at i (advancing i); 0 at the end.
int next_char(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    int cp = c & (0x3F >> n);
    while (n-- > 0 && i < s.size()) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

// Lays the text out at scale (font units to pixels): calls glyph(index, x) for each character and returns the width.
template <typename F>
float lay_out(Font* f, const std::string& text, float scale, F&& glyph) {
    float x = 0.0f;
    int prev = 0;
    for (size_t i = 0; i < text.size();) {
        const int cp = next_char(text, i);
        if (cp == 0) break;
        const int g = stbtt_FindGlyphIndex(&f->info, cp);
        if (g == 0 && cp != ' ') continue;
        if (prev != 0) x += scale * stbtt_GetGlyphKernAdvance(&f->info, prev, g);
        glyph(g, x);
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&f->info, g, &advance, &lsb);
        x += scale * advance;
        prev = g;
    }
    return x;
}
} // namespace

// The text's width at size (the height of a line from the top of its capitals to the bottom of its descenders, in any
// unit: the width comes back in the same unit).
float text_width(int font_id, const std::string& text, float size) {
    std::lock_guard<std::mutex> lock(g_font_mutex);
    Font* f = font(font_id);
    if (f == nullptr || size <= 0.0f) return 0.0f;
    const float scale = stbtt_ScaleForPixelHeight(&f->info, size);
    return lay_out(f, text, scale, [](int, float) {});
}

// The text as a picture size pixels high (0xAABBGGRR words as ImGui keeps RGBA32: white, the coverage in alpha),
// w x h; false if there is nothing to draw.
bool text_picture(int font_id, const std::string& text, int size, std::vector<uint32_t>& pixels, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_font_mutex);
    Font* f = font(font_id);
    if (f == nullptr || size <= 0 || text.empty()) return false;
    const float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size);
    const float width = lay_out(f, text, scale, [](int, float) {});
    const int pad = 2;
    w = (int)std::ceil(width) + pad * 2;
    h = size + pad * 2;
    if (w <= pad * 2 || w > 4096 || h > 1024) return false;
    std::vector<unsigned char> alpha((size_t)w * h, 0);
    const int baseline = pad + (int)std::lround(f->ascent * scale);
    lay_out(f, text, scale, [&](int g, float x) {
        int x0, y0, x1, y1;
        const float shift = x - std::floor(x);
        stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
        const int gx = pad + (int)std::floor(x) + x0, gy = baseline + y0, gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) return;
        std::vector<unsigned char> bits((size_t)gw * gh);
        stbtt_MakeGlyphBitmapSubpixel(&f->info, bits.data(), gw, gh, gw, scale, scale, shift, 0.0f, g);
        for (int yy = 0; yy < gh; yy++) {
            for (int xx = 0; xx < gw; xx++) {
                const int px = gx + xx, py = gy + yy;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                unsigned char& a = alpha[(size_t)py * w + px];
                a = (unsigned char)std::min(255, a + bits[(size_t)yy * gw + xx]); // overlapping glyphs (kerning)
            }
        }
    });
    pixels.resize((size_t)w * h);
    for (size_t i = 0; i < pixels.size(); i++) pixels[i] = ((uint32_t)alpha[i] << 24) | 0x00FFFFFFu;
    return true;
}

} // namespace jfg::overlay
