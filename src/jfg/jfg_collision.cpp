// Jet Force Gemini port: the level's (track's) collision with bigger limits for pack levels (JFG-170; decomp MSR 15, 16).
//
// The game finds the track triangles near a move by segment bounds and a coarse cell grid per segment, and keeps
// them in fixed-size lists. Big custom tracks overflow those lists, and the hero falls through floors. The port has
// its own versions of the functions involved, written from the decomp's behaviour spec (MSR 16):
//   - trackMakePolylist: the poly list every track collision test reads. The game takes the first 10 segments a
//     query box meets; a pack level takes up to PACK_SEGMENTS. The list size is the level header's +0xEA; a pack
//     level gets at least PACK_POLY_LIST (and its edge list, +0xEC, PACK_EDGE_LIST) when its track loads. The game
//     could write up to 10 entries past the list; this version checks before every write.
//   - trackGetHeights: the floors under a point. The game gives up at 32 segments holding the point, after its stack
//     list has already overflowed past about 42; a pack level gives up at PACK_HEIGHT_SEGMENTS.
//   - trackGetBlockList: the segments holding a point, with no capacity; it now stops at 32 (trackPolyHeight's list).
//   - The cells: each face is filed in the cells of its x / z bounding box, so a long diagonal face is in the list for
//     every query near any part of its box and fills it. In a pack level a face whose cells meet the query's is also
//     tested exactly: its x / z triangle against the query box (2D separating axes, integers, edges included). Only
//     faces the moving sphere can't reach are dropped; the others keep their order. (JFG_COLLISION_EXACT=1 tests every
//     level so, =0 none.)
// A stock level gets exactly the game's results, overflows aside. "Pack level" = its track is a mod's
// (JFG_COLLISION_PACK=1 treats every level as one, =0 none).
//
// The loader unpacks a track in place in a fixed 0x9F000-byte block (the packed track read to its end), so the packed
// and unpacked track together must fit it (the tables after the track have more room, jfg_memory.cpp). Each load
// logs both sizes, and warns when they come near the limit.
//
// JFG_COLLISION_CHECK=1 runs the game's own trackMakePolylist and trackGetHeights next to the port's on every call
// and logs where they differ (stderr), with a summary at each track load.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "jfg_assets.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" {
void hitMakePolylist(uint8_t* rdram, recomp_context* ctx);
void mathXZInTri(uint8_t* rdram, recomp_context* ctx);
}

namespace jfg {
bool mod_entry(int data_asset, int entry); // jfg_assets.cpp
}

namespace {
using namespace jfg;

constexpr uint32_t ADDR_track = 0x800A0D60;     // Track*
constexpr uint32_t ADDR_PLlist = 0x800F33B8;    // s32* poly list: segment markers (> 0) and poly info addresses (< 0)
constexpr uint32_t ADDR_PLgrps = 0x800F33BC;    // s16*: the group of each face entry
constexpr uint32_t ADDR_PLpolys = 0x800F33C0;   // s16*: the face (in its segment) of each face entry
constexpr uint32_t ADDR_PLno = 0x800F33C4;      // s32: entries in the list
constexpr uint32_t ADDR_heights = 0x800F2FE8;   // trackGetHeights' results: 20 x 20 bytes
constexpr uint32_t ADDR_heightPtrs = 0x800F3178; // ... and 20 pointers to them, highest first
constexpr int HEIGHT_RESULTS = 20, HEIGHT_SIZE = 20;
constexpr int TRACK_TABLE = 0x24, TRACK_DATA = 0x25; // the tracks' offset table and data assets                // the tracks' data asset

constexpr int STOCK_SEGMENTS = 10, PACK_SEGMENTS = 64;
constexpr int STOCK_HEIGHT_SEGMENTS = 32, PACK_HEIGHT_SEGMENTS = 256;
constexpr int BLOCK_LIST_CAP = 32;              // trackPolyHeight's list
constexpr int16_t PACK_POLY_LIST = 512, PACK_EDGE_LIST = 256;
constexpr uint32_t ALWAYS_EXCLUDED = 0x880;     // group flags trackMakePolylist always leaves out
constexpr uint32_t GROUP_WATER = 0x2000;        // trackGetHeights: surface type 2

bool g_pack = false;   // the loaded track is a mod's
int g_check = -1;      // JFG_COLLISION_CHECK
struct CheckStats { long polylists = 0, polylist_diffs = 0, overflows = 0, dropped = 0, heights = 0, height_diffs = 0; } g_stats;

bool check_on() {
    if (g_check < 0) {
        const char* v = std::getenv("JFG_COLLISION_CHECK");
        g_check = v && v[0] == '1' ? 1 : 0;
    }
    return g_check == 1;
}

struct Box { int min_x, min_y, min_z, max_x, max_y, max_z; };
struct Bounds { int16_t min_x, min_y, min_z, max_x, max_y, max_z; };

Bounds read_bounds(uint32_t b) {
    return { TrackBounds::MinX::get(b), TrackBounds::MinY::get(b), TrackBounds::MinZ::get(b),
             TrackBounds::MaxX::get(b), TrackBounds::MaxY::get(b), TrackBounds::MaxZ::get(b) };
}

// The cells of [lo, hi] along one axis of a segment's bounds: `cells` cells of ((max - min) >> shift) + 1 units, each
// closed at both ends (a value on a boundary is in both). Cell edges are s16, as in the game.
uint32_t axis_cells(int lo, int hi, int16_t min, int16_t max, int cells, int shift) {
    lo = std::clamp(lo, (int)min, (int)max);
    hi = std::clamp(hi, (int)min, (int)max);
    const int step = ((max - min) >> shift) + 1;
    uint32_t bits = 0;
    for (int i = 0; i < cells; i++) {
        const int cell_lo = (int16_t)(min + i * step), cell_hi = (int16_t)(min + (i + 1) * step);
        if (cell_hi >= lo && cell_lo <= hi) bits |= 1u << i;
    }
    return bits;
}
// The game's getXZCompareMask: X cells in bits 0-15, Z cells in 16-31.
uint32_t xz_cells(const Bounds& b, int x1, int z1, int x2, int z2) {
    return axis_cells(x1, x2, b.min_x, b.max_x, 16, 4) | axis_cells(z1, z2, b.min_z, b.max_z, 16, 4) << 16;
}
// getYCompareMask: 8 cells.
uint32_t y_cells(const Bounds& b, int y1, int y2) { return axis_cells(y1, y2, b.min_y, b.max_y, 8, 3); }

// The face's x / z triangle meets the box [min_x, max_x] x [min_z, max_z] (closed): their bounding rectangles meet and
// no edge of the triangle has every corner of the box strictly on its far side.
bool triangle_meets_box(const int (&x)[3], const int (&z)[3], const Box& box) {
    if (std::max({ x[0], x[1], x[2] }) < box.min_x || std::min({ x[0], x[1], x[2] }) > box.max_x) return false;
    if (std::max({ z[0], z[1], z[2] }) < box.min_z || std::min({ z[0], z[1], z[2] }) > box.max_z) return false;
    const int64_t cx[4] = { box.min_x, box.max_x, box.max_x, box.min_x }, cz[4] = { box.min_z, box.min_z, box.max_z, box.max_z };
    for (int e = 0; e < 3; e++) {
        const int a = e, b = (e + 1) % 3, o = (e + 2) % 3;
        const int64_t dx = x[b] - x[a], dz = z[b] - z[a];
        auto side = [&](int64_t px, int64_t pz) { return dx * (pz - z[a]) - dz * (px - x[a]); };
        const int64_t inside = side(x[o], z[o]);
        if (inside == 0) continue; // no area: the rectangles decide
        bool separated = true;
        for (int c = 0; c < 4 && separated; c++) {
            const int64_t s = side(cx[c], cz[c]);
            if (inside > 0 ? s >= 0 : s <= 0) separated = false;
        }
        if (separated) return false;
    }
    return true;
}

bool cells_meet(uint32_t xz, uint32_t query) {
    const uint32_t m = xz & query;
    return (m & 0xFFFF) != 0 && (m >> 16) != 0;
}

// A group of a segment: its faces run to the next group's first face (a sentinel group closes the last).
struct Group { uint32_t flags; int first, end, vertex_base, texture; };
Group read_group(uint32_t seg, int g) {
    const uint32_t b = TrackSegment::Batches::get(seg) + g * TrackBatch::Size;
    return { TrackBatch::Flags::get(b), TrackBatch::FaceOffset::get(b), TrackBatch::FaceOffset::get(b + TrackBatch::Size),
             TrackBatch::VertOffset::get(b), TrackBatch::Texture::get(b) };
}

uint32_t vertex_addr(uint32_t seg, const Group& g, uint32_t face, int corner) {
    return TrackSegment::Verts::get(seg) + (g.vertex_base + TrackFace::Vertex::get(face, corner)) * TrackVertex::Size;
}

bool face_meets_box(uint32_t seg, const Group& g, int t, const Box& box) {
    const uint32_t face = TrackSegment::Faces::get(seg) + t * TrackFace::Size;
    int x[3], z[3];
    for (int c = 0; c < 3; c++) {
        const uint32_t v = vertex_addr(seg, g, face, c);
        x[c] = TrackVertex::X::get(v);
        z[c] = TrackVertex::Z::get(v);
    }
    return triangle_meets_box(x, z, box);
}

int g_exact_override = -2; // JFG_COLLISION_EXACT: -1 unset, 0 / 1
bool exact_on() {
    if (g_exact_override == -2) {
        const char* v = std::getenv("JFG_COLLISION_EXACT");
        g_exact_override = v && (v[0] == '0' || v[0] == '1') ? v[0] - '0' : -1;
    }
    return g_exact_override >= 0 ? g_exact_override == 1 : g_pack;
}

int poly_list_cap() {
    const uint32_t h = rd32(ADDR_levelHeader);
    return valid_ptr(h) ? LevelHeader::PolyList::get(h) : 0;
}

// --- trackMakePolylist ------------------------------------------------------------------------------------------------
struct Taken { uint32_t seg; uint32_t xz, y; };

Box g_last_box; // the last query's box, for JFG_COLLISION_CHECK

// The port's trackMakePolylist(n, starts, ends, radii, exclude, include, object). Returns 1 when the list filled up.
uint32_t make_polylist(recomp_context* ctx) {
    const int n = (int32_t)ctx->r4;
    const uint32_t starts = (uint32_t)ctx->r5, ends = (uint32_t)ctx->r6, radii = (uint32_t)ctx->r7;
    const uint32_t sp = (uint32_t)ctx->r29;
    const uint32_t exclude = rd32(sp + 0x10) | ALWAYS_EXCLUDED, include = rd32(sp + 0x14), object = rd32(sp + 0x18);

    // The query box: integer (truncated) start, end and radius, padded by 5.
    Box box = { 100000, 100000, 100000, -100000, -100000, -100000 };
    for (int i = 0; i < n; i++) {
        const int r = (int)rdf(radii + i * 4);
        int* mins[3] = { &box.min_x, &box.min_y, &box.min_z };
        int* maxs[3] = { &box.max_x, &box.max_y, &box.max_z };
        for (int a = 0; a < 3; a++) {
            const int s = (int)rdf(starts + i * 12 + a * 4), e = (int)rdf(ends + i * 12 + a * 4);
            *mins[a] = std::min({ *mins[a], s - r, e - r });
            *maxs[a] = std::max({ *maxs[a], s + r, e + r });
        }
    }
    if (n >= 1) {
        box.min_x -= 5; box.min_y -= 5; box.min_z -= 5;
        box.max_x += 5; box.max_y += 5; box.max_z += 5;
    }
    g_last_box = box;

    // The object-model list, which the game builds from the same box.
    const uint32_t minf = (sp - 0x40) & ~0x7u, maxf = minf + 12; // in the gap call_game leaves below $sp
    wrf(minf, (float)box.min_x); wrf(minf + 4, (float)box.min_y); wrf(minf + 8, (float)box.min_z);
    wrf(maxf, (float)box.max_x); wrf(maxf + 4, (float)box.max_y); wrf(maxf + 8, (float)box.max_z);
    call_game(hitMakePolylist, ctx, { minf, maxf, exclude, include, object });

    // The segments whose bounds (+-5) meet the box, in order, up to the cap.
    const uint32_t track = rd32(ADDR_track);
    const int seg_cap = g_pack ? PACK_SEGMENTS : STOCK_SEGMENTS;
    Taken taken[PACK_SEGMENTS];
    int taken_count = 0;
    const int segs = Track::SegmentCount::get(track);
    for (int i = 0; i < segs && taken_count < seg_cap; i++) {
        const Bounds b = read_bounds(Track::Bounds::get(track) + i * TrackBounds::Size);
        if (b.min_x - 5 > box.max_x || box.min_x > b.max_x + 5) continue;
        if (b.min_y - 5 > box.max_y || box.min_y > b.max_y + 5) continue;
        if (b.min_z - 5 > box.max_z || box.min_z > b.max_z + 5) continue;
        taken[taken_count++] = { Track::Segments::get(track) + i * TrackSegment::Size,
                                 xz_cells(b, box.min_x, box.min_z, box.max_x, box.max_z), y_cells(b, box.min_y, box.max_y) };
    }
    if (taken_count == 0) {
        wr32(ADDR_PLno, 0);
        return 0;
    }

    // The list: a marker per segment, then its faces whose cells meet the box's.
    const int cap = poly_list_cap();
    const uint32_t list = rd32(ADDR_PLlist), grps = rd32(ADDR_PLgrps), polys = rd32(ADDR_PLpolys);
    const bool exact = exact_on();
    int count = 0;
    auto full = [&]() { wr32(ADDR_PLno, (uint32_t)count); return 1u; };
    for (int k = 0; k < taken_count; k++) {
        const uint32_t seg = taken[k].seg;
        if (count >= cap) return full();
        wr32(list + count * 4, seg & 0x7FFFFFFFu);
        count++;
        const uint32_t xz_mask = TrackSegment::XZCells::get(seg), y_mask = TrackSegment::YCells::get(seg);
        const uint32_t info = TrackSegment::PolyInfo::get(seg);
        const int group_count = TrackSegment::BatchCount::get(seg);
        for (int g = 0; g < group_count; g++) {
            const Group group = read_group(seg, g);
            if ((group.flags & include) == 0 && (group.flags & exclude) != 0) continue;
            for (int t = group.first; t < group.end; t++) {
                if (!cells_meet(rd32(xz_mask + t * 4), taken[k].xz) || (rd8(y_mask + t) & taken[k].y) == 0) continue;
                if (exact && !face_meets_box(seg, group, t, box)) continue;
                if (count >= cap) return full();
                wr32(list + count * 4, info + t * TrackPolyInfo::Size);
                wr16(grps + count * 2, (uint16_t)g);
                wr16(polys + count * 2, (uint16_t)t);
                if (++count >= cap) return full();
            }
        }
    }
    wr32(ADDR_PLno, (uint32_t)count);
    return 0;
}

// JFG_COLLISION_CHECK: the game's list against the port's.
struct ListCopy { std::vector<uint32_t> list; std::vector<uint16_t> grps, polys; uint32_t ret; };
ListCopy copy_list(uint32_t ret) {
    ListCopy c;
    c.ret = ret;
    const int count = (int32_t)rd32(ADDR_PLno);
    const uint32_t list = rd32(ADDR_PLlist), grps = rd32(ADDR_PLgrps), polys = rd32(ADDR_PLpolys);
    for (int i = 0; i < count; i++) {
        c.list.push_back(rd32(list + i * 4));
        const bool face = (int32_t)c.list.back() < 0;
        c.grps.push_back(face ? rd16(grps + i * 2) : 0);
        c.polys.push_back(face ? rd16(polys + i * 2) : 0);
    }
    return c;
}

// The game's list without the faces the exact test drops.
ListCopy drop_far_faces(const ListCopy& game) {
    ListCopy c;
    c.ret = game.ret;
    uint32_t seg = 0;
    for (size_t i = 0; i < game.list.size(); i++) {
        const bool face = (int32_t)game.list[i] < 0;
        if (!face) seg = game.list[i] | 0x80000000u;
        else if (!face_meets_box(seg, read_group(seg, game.grps[i]), game.polys[i], g_last_box)) {
            g_stats.dropped++;
            continue;
        }
        c.list.push_back(game.list[i]);
        c.grps.push_back(game.grps[i]);
        c.polys.push_back(game.polys[i]);
    }
    return c;
}

void compare_lists(const ListCopy& game_list, const ListCopy& port) {
    g_stats.polylists++;
    const ListCopy game = exact_on() ? drop_far_faces(game_list) : game_list;
    const int cap = poly_list_cap();
    // Where the game overflowed (wrote past its list), only the entries before the end are comparable.
    if ((int)game_list.list.size() >= cap) {
        g_stats.overflows++;
        return;
    }
    if (game.list == port.list && game.grps == port.grps && game.polys == port.polys && game.ret == port.ret) return;
    if (g_stats.polylist_diffs++ < 20)
        std::fprintf(stderr, "[jfg] collision check: poly list differs (level %d): game %zu entries ret %u, port %zu ret %u\n",
            (int16_t)rd16(ADDR_currentLevel), game.list.size(), game.ret, port.list.size(), port.ret);
}

// --- trackGetBlockList and trackGetHeights ---------------------------------------------------------------------------
// The segments whose bounds hold (x, z) with a 4-unit margin (open), in order, at most `cap`.
int block_list(int x, int z, int16_t* out, int cap) {
    const uint32_t track = rd32(ADDR_track);
    const int segs = Track::SegmentCount::get(track);
    int n = 0;
    for (int i = 0; i < segs && n < cap; i++) {
        const Bounds b = read_bounds(Track::Bounds::get(track) + i * TrackBounds::Size);
        if (b.min_x - 4 < x && x < b.max_x + 4 && b.min_z - 4 < z && z < b.max_z + 4) out[n++] = (int16_t)i;
    }
    return n;
}

bool xz_in_triangle(recomp_context* ctx, int x, int z, uint32_t v0, uint32_t v1, uint32_t v2) {
    return call_game(mathXZInTri, ctx, { (uint32_t)x, (uint32_t)z, v0, v1, v2 }) != 0;
}

// The port's trackGetHeights(x, z, exclude, &out): the upward faces under (x, z), highest first, at most 20.
uint32_t get_heights(recomp_context* ctx) {
    const float x = ctx->f12.fl, z = ctx->f14.fl;
    const uint32_t exclude = (uint32_t)ctx->r6, out = (uint32_t)ctx->r7;
    wr32(out, 0);
    const int ix = (int)x, iz = (int)z;
    const int give_up = g_pack ? PACK_HEIGHT_SEGMENTS : STOCK_HEIGHT_SEGMENTS;
    std::vector<int16_t> blocks(give_up);
    const int n = block_list(ix, iz, blocks.data(), give_up);
    if (n == 0 || n >= give_up) return 0;

    const uint32_t track = rd32(ADDR_track), textures = Track::Textures::get(track);
    int found = 0;
    for (int k = 0; k < n && found < HEIGHT_RESULTS; k++) {
        const uint32_t seg = Track::Segments::get(track) + blocks[k] * TrackSegment::Size;
        const Bounds b = read_bounds(Track::Bounds::get(track) + blocks[k] * TrackBounds::Size);
        const uint32_t mask = xz_cells(b, ix, iz, ix, iz);
        const uint32_t xz_mask = TrackSegment::XZCells::get(seg), info = TrackSegment::PolyInfo::get(seg);
        const uint32_t planes = TrackSegment::Planes::get(seg), faces = TrackSegment::Faces::get(seg);
        const int group_count = TrackSegment::BatchCount::get(seg);
        for (int gi = 0; gi < group_count && found < HEIGHT_RESULTS; gi++) {
            const Group g = read_group(seg, gi);
            const uint8_t surface = (g.flags & GROUP_WATER) ? 2 : rd8(textures + g.texture * 8 + 7);
            if (g.flags & exclude) continue;
            for (int t = g.first; t < g.end && found < HEIGHT_RESULTS; t++) {
                if (!cells_meet(rd32(xz_mask + t * 4), mask)) continue;
                const uint32_t plane = planes + TrackPolyInfo::Plane::get(info + t * TrackPolyInfo::Size) * TrackPlane::Size;
                const float nx = TrackPlane::NX::get(plane), ny = TrackPlane::NY::get(plane);
                const float nz = TrackPlane::NZ::get(plane), d = TrackPlane::D::get(plane);
                if (!(ny > 0.0f)) continue;
                const uint32_t face = faces + t * TrackFace::Size;
                if (!xz_in_triangle(ctx, ix, iz, vertex_addr(seg, g, face, 0), vertex_addr(seg, g, face, 1),
                                    vertex_addr(seg, g, face, 2)))
                    continue;
                const uint32_t e = ADDR_heights + found * HEIGHT_SIZE;
                wrf(e + 0x00, -(nx * x + nz * z + d) / ny);
                wrf(e + 0x04, nx); wrf(e + 0x08, ny); wrf(e + 0x0C, nz);
                wr32(e + 0x10, (g.flags & ~0xFFu) | surface);
                found++;
            }
        }
    }

    // Pointers, highest first: swap neighbours while the first is lower, until a pass swaps none.
    for (int i = 0; i < found; i++) wr32(ADDR_heightPtrs + i * 4, ADDR_heights + i * HEIGHT_SIZE);
    for (bool swapped = true; swapped;) {
        swapped = false;
        for (int i = 0; i + 1 < found; i++) {
            const uint32_t a = rd32(ADDR_heightPtrs + i * 4), b = rd32(ADDR_heightPtrs + i * 4 + 4);
            if (rdf(a) < rdf(b)) {
                wr32(ADDR_heightPtrs + i * 4, b);
                wr32(ADDR_heightPtrs + i * 4 + 4, a);
                swapped = true;
            }
        }
    }
    wr32(out, ADDR_heightPtrs);
    return (uint32_t)found;
}

struct HeightsCopy { uint32_t count, out; std::vector<uint32_t> words; };
HeightsCopy copy_heights(uint32_t count, uint32_t out_ptr) {
    HeightsCopy c { count, rd32(out_ptr), {} };
    if (c.out != 0)
        for (uint32_t i = 0; i < count && i < (uint32_t)HEIGHT_RESULTS; i++) {
            const uint32_t e = rd32(c.out + i * 4);
            for (int w = 0; w < HEIGHT_SIZE / 4; w++) c.words.push_back(rd32(e + w * 4));
        }
    return c;
}

void log_check_summary() {
    if (!check_on() || (g_stats.polylists == 0 && g_stats.heights == 0)) return;
    std::fprintf(stderr, "[jfg] collision check: %ld poly lists (%ld differ, %ld game overflows, %ld far faces dropped), %ld height queries (%ld differ)\n",
        g_stats.polylists, g_stats.polylist_diffs, g_stats.overflows, g_stats.dropped, g_stats.heights, g_stats.height_diffs);
    g_stats = {};
}

constexpr uint32_t TRACK_UNPACK_ROOM = 0x9F000;
constexpr uint32_t TRACK_DATA_END = 0x2C; // Track: s32 the unpacked track's size (where the loader's tables start)

// The packed size of track `id` (its entry in the tracks' offset table), 0 if unknown.
uint32_t packed_track_size(int id) {
    const std::span<const uint8_t> table = assets::read(TRACK_TABLE);
    auto be32 = [&](size_t o) { return (uint32_t)table[o] << 24 | (uint32_t)table[o + 1] << 16 | (uint32_t)table[o + 2] << 8 | table[o + 3]; };
    const size_t o = (size_t)id * 4;
    if (id < 0 || o + 8 > table.size()) return 0;
    const uint32_t a = be32(o), b = be32(o + 4);
    return b > a && b != 0xFFFFFFFFu ? b - a : 0;
}

bool pack_override(bool* value) {
    const char* v = std::getenv("JFG_COLLISION_PACK");
    if (!v || (v[0] != '0' && v[0] != '1')) return false;
    *value = v[0] == '1';
    return true;
}

} // namespace

JFG_WRAP(trackMakePolylist) {
    g_rdram = rdram;
    if (check_on()) {
        const recomp_context saved = *ctx;
        trackMakePolylist_orig(rdram, ctx);
        const ListCopy game = copy_list((uint32_t)ctx->r2);
        *ctx = saved;
        const uint32_t ret = make_polylist(ctx);
        compare_lists(game, copy_list(ret));
        if (g_stats.polylists % 2000 == 0) log_check_summary();
        ctx->r2 = (gpr)(int32_t)ret;
        return;
    }
    ctx->r2 = (gpr)(int32_t)make_polylist(ctx);
}

JFG_WRAP(trackGetHeights) {
    g_rdram = rdram;
    if (check_on()) {
        const recomp_context saved = *ctx;
        const uint32_t out = (uint32_t)ctx->r7;
        trackGetHeights_orig(rdram, ctx);
        const HeightsCopy game = copy_heights((uint32_t)ctx->r2, out);
        *ctx = saved;
        const uint32_t count = get_heights(ctx);
        const HeightsCopy port = copy_heights(count, out);
        g_stats.heights++;
        if ((game.count != port.count || game.words != port.words) && g_stats.height_diffs++ < 20)
            std::fprintf(stderr, "[jfg] collision check: heights differ at (%.1f, %.1f): game %u, port %u\n",
                saved.f12.fl, saved.f14.fl, game.count, port.count);
        ctx->r2 = (gpr)(int32_t)count;
        return;
    }
    ctx->r2 = (gpr)(int32_t)get_heights(ctx);
}

// trackGetBlockList(x, z, out): its callers' lists hold 32 (trackPolyHeight); the game wrote on past them.
JFG_WRAP(trackGetBlockList) {
    g_rdram = rdram;
    int16_t blocks[BLOCK_LIST_CAP];
    const int n = block_list((int32_t)ctx->r4, (int32_t)ctx->r5, blocks, BLOCK_LIST_CAP);
    const uint32_t out = (uint32_t)ctx->r6;
    for (int i = 0; i < n; i++) wr16(out + i * 2, (uint16_t)blocks[i]);
    ctx->r2 = (gpr)n;
}

// The track loader (overlay 24): it allocates the poly and edge lists from the level header's +0xEA / +0xEC, so a
// pack level's are raised first.
JFG_WRAP(func_overlay_24_000001F8_1F41D28) {
    g_rdram = rdram;
    log_check_summary();
    const uint32_t header = rd32(ADDR_levelHeader);
    const int track_id = valid_ptr(header) ? LevelHeader::Track::get(header) : -1;
    g_pack = track_id >= 0 && mod_entry(TRACK_DATA, track_id);
    pack_override(&g_pack);
    if (g_pack && valid_ptr(header)) {
        if (LevelHeader::PolyList::get(header) < PACK_POLY_LIST) LevelHeader::PolyList::set(header, PACK_POLY_LIST);
        if (LevelHeader::EdgeList::get(header) < PACK_EDGE_LIST) LevelHeader::EdgeList::set(header, PACK_EDGE_LIST);
    }
    func_overlay_24_000001F8_1F41D28_orig(rdram, ctx);
    const uint32_t track = rd32(ADDR_track);
    std::fprintf(stderr, "[jfg] collision: track %d loaded (%s), %d segments, poly list %d, edge list %d\n", track_id,
        g_pack ? "pack level" : "stock rules", valid_ptr(track) ? (int)Track::SegmentCount::get(track) : -1,
        valid_ptr(header) ? (int)LevelHeader::PolyList::get(header) : -1,
        valid_ptr(header) ? (int)LevelHeader::EdgeList::get(header) : -1);
    if (valid_ptr(track)) {
        const uint32_t unpacked = rd32(track + TRACK_DATA_END), packed = packed_track_size(track_id);
        const bool tight = unpacked + packed > TRACK_UNPACK_ROOM / 10 * 9;
        std::fprintf(stderr, "[jfg] collision: track %d is 0x%X bytes unpacked, 0x%X packed: %u%% of the 0x%X the loader unpacks in%s\n",
            track_id, unpacked, packed, (unsigned)((uint64_t)(unpacked + packed) * 100 / TRACK_UNPACK_ROOM), TRACK_UNPACK_ROOM,
            tight ? " (near the limit)" : "");
    }
}
