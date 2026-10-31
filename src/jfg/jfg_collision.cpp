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
//   - Seams: the file names the face across each edge only inside its own segment, so the loader makes every edge on
//     a segment seam a collision edge with an upright plane: an invisible wall or a lip along the seam, and a gap in
//     the faces' own edge planes. In a pack level, after the track loads, an open edge whose two corners match an open
//     edge of a face in another segment (in the opposite direction) is linked as the loader links edges inside a
//     segment: each side's edge plane is rebuilt in place from both faces, the collision edge is kept only where the
//     faces make a ridge (the first side), and ledges pair up across it. (JFG_COLLISION_SEAMS=0 leaves them, =1 links
//     them in every level.)
//   - Back-to-back faces (a thin wall with both sides modelled): the edge planes between them come out all zeros, and
//     the ray test shots use then finds the wall's plane far past its ends. A pack level's are rebuilt after the load
//     (repair_zero_edge_planes).
// A stock level gets exactly the game's results, overflows aside. "Pack level" = its track is a mod's
// (JFG_COLLISION_PACK=1 treats every level as one, =0 none).
//
// The loader unpacks a track in place in a fixed 0x9F000-byte block (the packed track read to its end), so the packed
// and unpacked track together must fit it (the tables after the track have more room, jfg_memory.cpp). Each load
// logs both sizes, and warns when they come near the limit.
//
// JFG_COLLISION_CHECK=1 runs the game's own trackMakePolylist and trackGetHeights next to the port's on every call
// and logs where they differ (stderr), with a summary at each track load. JFG_DEBUG_RAY and JFG_DEBUG_RAY_SWEEP test
// the game's ray test against the faces themselves (ray_probe, ray_sweep).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
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
constexpr int TRACK_TABLE = 0x24, TRACK_DATA = 0x25; // the tracks' offset table and data assets
constexpr uint32_t ADDR_nohitsflag = 0x800F2F9C; // s32: set, the loader makes no edge planes                // the tracks' data asset

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

// --- seams ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t GROUP_NO_EDGES = 0x880;      // group flags the loader makes no edge planes for
constexpr uint8_t FACE_NO_COLLISION = 0x80, FACE_LEDGE_WALL = 0x01, FACE_LEDGE_TOP = 0x02, FACE_STEEP_EDGE = 0x04;

// An edge of a face that the file gave no neighbour (its slot named the face itself).
struct OpenEdge { uint32_t seg; int t, e, g; int16_t a[3], b[3]; };
std::vector<OpenEdge> g_open_edges;

// Seams are linked in pack levels (JFG_COLLISION_SEAMS=0: never, =1: in every level).
bool seams_on() {
    const char* v = std::getenv("JFG_COLLISION_SEAMS");
    if (v && (v[0] == '0' || v[0] == '1')) return v[0] == '1';
    return g_pack;
}

void read_vertex(uint32_t v, int16_t (&out)[3]) { out[0] = TrackVertex::X::get(v); out[1] = TrackVertex::Y::get(v); out[2] = TrackVertex::Z::get(v); }

// Before the loader's plane pass on a segment (the slots still hold the file's neighbours).
void note_open_edges(uint32_t seg) {
    const uint32_t info = TrackSegment::PolyInfo::get(seg), faces = TrackSegment::Faces::get(seg);
    const int group_count = TrackSegment::BatchCount::get(seg);
    for (int gi = 0; gi < group_count; gi++) {
        const Group g = read_group(seg, gi);
        if (g.flags & GROUP_NO_EDGES) continue;
        for (int t = g.first; t < g.end; t++) {
            const uint32_t face = faces + t * TrackFace::Size;
            if (TrackFace::Flags::get(face) & FACE_NO_COLLISION) continue;
            const uint32_t pi = info + t * TrackPolyInfo::Size;
            const uint16_t own = TrackPolyInfo::Plane::get(pi);
            for (int e = 0; e < 3; e++) {
                if (TrackPolyInfo::Edge::get(pi, e) != own) continue;
                OpenEdge o { seg, t, e, gi, {}, {} };
                read_vertex(vertex_addr(seg, g, face, e), o.a);
                read_vertex(vertex_addr(seg, g, face, (e + 1) % 3), o.b);
                g_open_edges.push_back(o);
            }
        }
    }
}

struct V3 { float x, y, z; };
V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 vec(const int16_t (&v)[3]) { return { (float)v[0], (float)v[1], (float)v[2] }; }

struct Plane { V3 n; float d; };
Plane read_plane(uint32_t seg, int index) {
    const uint32_t p = TrackSegment::Planes::get(seg) + index * TrackPlane::Size;
    return { { TrackPlane::NX::get(p), TrackPlane::NY::get(p), TrackPlane::NZ::get(p) }, TrackPlane::D::get(p) };
}
void write_plane(uint32_t seg, int index, const Plane& pl) {
    const uint32_t p = TrackSegment::Planes::get(seg) + index * TrackPlane::Size;
    TrackPlane::NX::set(p, pl.n.x); TrackPlane::NY::set(p, pl.n.y); TrackPlane::NZ::set(p, pl.n.z); TrackPlane::D::set(p, pl.d);
}
Plane face_plane(const OpenEdge& o) {
    return read_plane(o.seg, TrackPolyInfo::Plane::get(TrackSegment::PolyInfo::get(o.seg) + o.t * TrackPolyInfo::Size));
}
uint32_t face_addr(const OpenEdge& o) { return TrackSegment::Faces::get(o.seg) + o.t * TrackFace::Size; }
uint32_t edge_bits_addr(const OpenEdge& o) { return TrackSegment::EdgeBits::get(o.seg) + o.t; }

// The edge plane of `side`'s edge, from both faces: through the edge and the faces' bisector.
void rebuild_edge_plane(const OpenEdge& side, const Plane& own, const Plane& other) {
    const V3 a = vec(side.a), b = vec(side.b);
    const V3 sum = { 5.0f * (own.n.x + other.n.x), 5.0f * (own.n.y + other.n.y), 5.0f * (own.n.z + other.n.z) };
    V3 m = cross(sub(b, a), sum);
    const float len = std::sqrt(dot(m, m));
    if (len > 0.0f) m = { m.x / len, m.y / len, m.z / len };
    const uint32_t pi = TrackSegment::PolyInfo::get(side.seg) + side.t * TrackPolyInfo::Size;
    write_plane(side.seg, TrackPolyInfo::Edge::get(pi, side.e) & 0x7FFF, { m, -dot(m, a) });
}

// Links `first` (the side the loader would have built the edge from) and `second`.
void link_seam(const OpenEdge& first, const OpenEdge& second) {
    const Plane pa = face_plane(first), pb = face_plane(second);
    rebuild_edge_plane(first, pa, pb);
    rebuild_edge_plane(second, pb, pa);

    // Ledges: a ledge wall next to a ledge top makes the top's edge a ledge edge.
    const uint32_t fa = face_addr(first), fb = face_addr(second);
    const uint8_t flags_a = TrackFace::Flags::get(fa), flags_b = TrackFace::Flags::get(fb);
    if ((flags_a & FACE_LEDGE_WALL) && (flags_b & FACE_LEDGE_TOP)) {
        TrackFace::Flags::set(fa, (uint8_t)(flags_a & ~FACE_LEDGE_WALL));
        wr8(edge_bits_addr(second), (uint8_t)(rd8(edge_bits_addr(second)) | (1u << (second.e + 3))));
    } else if ((flags_a & FACE_LEDGE_TOP) && (flags_b & FACE_LEDGE_WALL)) {
        TrackFace::Flags::set(fb, (uint8_t)(flags_b & ~FACE_LEDGE_WALL));
        wr8(edge_bits_addr(first), (uint8_t)(rd8(edge_bits_addr(first)) | (1u << (first.e + 3))));
    }

    // The collision edge stays on the first side only where the faces make a ridge (its far corner below the other
    // face); the second side has none, as for an edge inside a segment.
    const Group g = read_group(first.seg, first.g);
    int16_t far[3];
    read_vertex(vertex_addr(first.seg, g, fa, (first.e + 2) % 3), far);
    uint8_t bits_a = (uint8_t)(rd8(edge_bits_addr(first)) & ~(1u << first.e));
    if (dot(pb.n, vec(far)) + pb.d < 0.0f) bits_a |= (uint8_t)(1u << first.e);
    wr8(edge_bits_addr(first), bits_a);
    wr8(edge_bits_addr(second), (uint8_t)(rd8(edge_bits_addr(second)) & ~(1u << second.e)));

    // A steep edge: more than 90 degrees between the faces, and the edge no steeper than 45.
    const float rise = std::fabs((float)first.b[1] - (float)first.a[1]);
    const float run = std::hypot((float)first.b[0] - (float)first.a[0], (float)first.b[2] - (float)first.a[2]);
    if (dot(pa.n, pb.n) < 0.0f && rise <= run)
        TrackFace::Flags::set(fa, (uint8_t)(TrackFace::Flags::get(fa) | (FACE_STEEP_EDGE << first.e)));
}

// After the track loaded: link the open edges that meet an open edge of another segment.
void link_seams() {
    struct Key {
        int16_t v[6];
        bool operator<(const Key& o) const { return std::lexicographical_compare(v, v + 6, o.v, o.v + 6); }
    };
    std::map<Key, std::vector<size_t>> by_edge;
    for (size_t i = 0; i < g_open_edges.size(); i++) {
        const OpenEdge& o = g_open_edges[i];
        const bool ab = std::lexicographical_compare(o.a, o.a + 3, o.b, o.b + 3);
        Key k;
        std::copy(ab ? o.a : o.b, (ab ? o.a : o.b) + 3, k.v);
        std::copy(ab ? o.b : o.a, (ab ? o.b : o.a) + 3, k.v + 3);
        by_edge[k].push_back(i);
    }
    int linked = 0, unmatched = 0, skipped = 0;
    for (const auto& [key, list] : by_edge) {
        if (list.size() == 1) { unmatched++; continue; }
        const OpenEdge& p = g_open_edges[list[0]];
        const OpenEdge& q = g_open_edges[list[1]];
        // Two faces of different segments, along the edge in opposite directions (both facing the same way).
        if (list.size() != 2 || p.seg == q.seg || std::memcmp(p.a, q.b, sizeof(p.a)) != 0) { skipped++; continue; }
        if (p.seg < q.seg) link_seam(p, q); else link_seam(q, p);
        linked++;
    }
    std::fprintf(stderr, "[jfg] collision: seams: %d edges linked across segments, %d open edges left, %d not linked (more "
        "than two faces, one segment or facing apart)\n", linked, unmatched, skipped);
    g_open_edges.clear();
}

// --- edges between back-to-back faces --------------------------------------------------------------------------------
// The loader builds an edge's plane from the sum of the two faces' normals. Two faces back to back along an edge (a
// thin wall modelled with both sides) have opposite normals, so the plane comes out all zeros, and a zero plane never
// puts a point outside the face: the face reaches on for ever past that edge. The hero's list drops such far faces
// (the exact test), but the ray test that shots and the crosshair use (trackNearestIntersection) finds the face's
// plane far from the face, and a shot stops at a wall that isn't there. In a pack level such a plane is rebuilt as the
// face's own boundary (upright to the face, through the edge, as for an edge with no neighbour). The other face, which
// uses the plane negated (bit 0x8000), gets the same boundary of its own: for two faces back to back it is the plane
// itself, so its 0x8000 is cleared. (JFG_COLLISION_EDGES=0 leaves them, =1 rebuilds them in every level.)
bool edges_on() {
    const char* v = std::getenv("JFG_COLLISION_EDGES");
    if (v && (v[0] == '0' || v[0] == '1')) return v[0] == '1';
    return g_pack;
}

void repair_zero_edge_planes() {
    const uint32_t track = rd32(ADDR_track);
    struct Shared { uint32_t pi; int e; int plane; V3 m; float d; };
    std::vector<Shared> shared, owned;
    int rebuilt = 0, unflipped = 0;
    for (int s = 0; s < Track::SegmentCount::get(track); s++) {
        const uint32_t seg = Track::Segments::get(track) + s * TrackSegment::Size;
        const uint32_t info = TrackSegment::PolyInfo::get(seg), faces = TrackSegment::Faces::get(seg);
        // All of the segment's zero planes first (writing one as it is found would hide it from its other face).
        for (int gi = 0; gi < TrackSegment::BatchCount::get(seg); gi++) {
            const Group g = read_group(seg, gi);
            if (g.flags & GROUP_NO_EDGES) continue;
            for (int t = g.first; t < g.end; t++) {
                const uint32_t face = faces + t * TrackFace::Size;
                if (TrackFace::Flags::get(face) & FACE_NO_COLLISION) continue;
                const uint32_t pi = info + t * TrackPolyInfo::Size;
                const Plane own = read_plane(seg, TrackPolyInfo::Plane::get(pi));
                for (int e = 0; e < 3; e++) {
                    const uint16_t slot = TrackPolyInfo::Edge::get(pi, e);
                    const int index = slot & 0x7FFF;
                    const Plane p = read_plane(seg, index);
                    if (dot(p.n, p.n) > 0.25f) continue; // a real plane
                    int16_t va[3], vb[3];
                    read_vertex(vertex_addr(seg, g, face, e), va);
                    read_vertex(vertex_addr(seg, g, face, (e + 1) % 3), vb);
                    V3 m = cross(sub(vec(vb), vec(va)), own.n);
                    const float len = std::sqrt(dot(m, m));
                    if (len <= 0.0f) continue;
                    m = { m.x / len, m.y / len, m.z / len };
                    const float d = -dot(m, vec(va));
                    (slot & 0x8000 ? shared : owned).push_back({ pi, e, index, m, d });
                }
            }
        }
        for (const Shared& u : owned) {
            write_plane(seg, u.plane, { u.m, u.d });
            rebuilt++;
        }
        owned.clear();
        // The faces that use another face's plane negated: now that it is rebuilt, use it as it is where it is
        // their own boundary too; rebuild it for them if nothing did.
        for (const Shared& u : shared) {
            const Plane p = read_plane(seg, u.plane);
            if (dot(p.n, p.n) < 0.25f) {
                write_plane(seg, u.plane, { u.m, u.d });
                rebuilt++;
            } else if (dot(p.n, u.m) < 0.5f) {
                continue; // already its boundary negated
            }
            TrackPolyInfo::Edge::set(u.pi, u.e, (uint16_t)u.plane);
            unflipped++;
        }
        shared.clear();
    }
    if (rebuilt + unflipped > 0)
        std::fprintf(stderr, "[jfg] collision: %d zero edge planes rebuilt (faces back to back), %d shared ones used unnegated\n",
            rebuilt, unflipped);
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

// JFG_DEBUG_RAY="x1,y1,z1,x2,y2,z2": once a level has run a few seconds, the game's ray test (trackNearestIntersection,
// what shots and the crosshair use) along that line at 10, 20 ... 60 above it, each next to the nearest real face on
// the same line (every colliding face of the track, tested directly), so a hit with no face there shows.
struct RayProbe { bool on = false; float a[3], b[3]; int calls = -1; } g_ray;

void read_ray_probe() {
    const char* v = std::getenv("JFG_DEBUG_RAY");
    if (!v) return;
    g_ray.on = std::sscanf(v, "%f,%f,%f,%f,%f,%f", &g_ray.a[0], &g_ray.a[1], &g_ray.a[2], &g_ray.b[0], &g_ray.b[1], &g_ray.b[2]) == 6;
}

// The nearest colliding face on a -> b (0..1 along it), -1 if none; seg / face of it.
float nearest_face(const V3& a, const V3& b, int* seg_out, int* face_out) {
    const uint32_t track = rd32(ADDR_track);
    const V3 d = sub(b, a);
    float best = -1.0f;
    for (int s = 0; s < Track::SegmentCount::get(track); s++) {
        const uint32_t seg = Track::Segments::get(track) + s * TrackSegment::Size;
        for (int gi = 0; gi < TrackSegment::BatchCount::get(seg); gi++) {
            const Group g = read_group(seg, gi);
            for (int t = g.first; t < g.end; t++) {
                const uint32_t face = TrackSegment::Faces::get(seg) + t * TrackFace::Size;
                if (TrackFace::Flags::get(face) & FACE_NO_COLLISION) continue;
                int16_t v[3][3];
                for (int c = 0; c < 3; c++) read_vertex(vertex_addr(seg, g, face, c), v[c]);
                const V3 p0 = vec(v[0]), e1 = sub(vec(v[1]), p0), e2 = sub(vec(v[2]), p0);
                const V3 h = cross(d, e2);
                const float det = dot(e1, h);
                if (std::fabs(det) < 1e-6f) continue;
                const V3 sv = sub(a, p0);
                const float u = dot(sv, h) / det;
                if (u < 0.0f || u > 1.0f) continue;
                const V3 q = cross(sv, e1);
                const float w = dot(d, q) / det;
                if (w < 0.0f || u + w > 1.0f) continue;
                const float k = dot(e2, q) / det;
                if (k < 0.0f || k > 1.0f || (best >= 0.0f && k >= best)) continue;
                best = k;
                *seg_out = s;
                *face_out = t;
            }
        }
    }
    return best;
}

// JFG_DEBUG_RAY_SWEEP=n: n x n level rays across the track's bounds (horizontal, along x and along z, at heights over
// its floor range), each against the faces themselves; logs how many the game stops where no face is (phantom), and
// how many pass a face (missed, closer than 2 units to its edge excluded).
void ray_sweep(recomp_context* ctx) {
    static const int n = [] { const char* v = std::getenv("JFG_DEBUG_RAY_SWEEP"); return v ? std::atoi(v) : 0; }();
    if (n <= 0) return;
    const uint32_t track = rd32(ADDR_track);
    int lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
    for (int s = 0; s < Track::SegmentCount::get(track); s++) {
        const Bounds b = read_bounds(Track::Bounds::get(track) + s * TrackBounds::Size);
        const int mn[3] = { b.min_x, b.min_y, b.min_z }, mx[3] = { b.max_x, b.max_y, b.max_z };
        for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], mn[a]); hi[a] = std::max(hi[a], mx[a]); }
    }
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u;
    int rays = 0, phantom = 0, missed = 0, water = 0, agree = 0;
    for (int dir = 0; dir < 2; dir++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                const float u = lo[dir == 0 ? 2 : 0] + (hi[dir == 0 ? 2 : 0] - lo[dir == 0 ? 2 : 0]) * (i + 0.5f) / n;
                const float y = lo[1] + (hi[1] - lo[1]) * (j + 0.5f) / n;
                const V3 a = dir == 0 ? V3{ (float)lo[0], y, u } : V3{ u, y, (float)lo[2] };
                const V3 b = dir == 0 ? V3{ (float)hi[0], y, u } : V3{ u, y, (float)hi[2] };
                wrf(buf + 0, a.x); wrf(buf + 4, a.y); wrf(buf + 8, a.z);
                wrf(buf + 12, b.x); wrf(buf + 16, b.y); wrf(buf + 20, b.z);
                const recomp_context saved = *ctx;
                const bool hit = (call_game(trackNearestIntersection, ctx, { 0u, buf, buf + 12, buf + 24, 0u, 0u }) & 0xFF) != 0;
                *ctx = saved;
                int seg = -1, face = -1;
                const float k = nearest_face(a, b, &seg, &face);
                const float len = std::sqrt(dot(sub(b, a), sub(b, a)));
                rays++;
                if (hit) {
                    const V3 h = { rdf(buf + 28), rdf(buf + 32), rdf(buf + 36) };
                    const float at = std::sqrt(dot(sub(h, a), sub(h, a)));
                    if (k < 0.0f || at + 2.0f < k * len) {
                        if (phantom++ < 8)
                            std::fprintf(stderr, "[jfg] ray sweep: phantom hit at (%.0f, %.0f, %.0f), first face at %.0f of %.0f\n", h.x, h.y,
                                h.z, k < 0.0f ? -1.0f : k * len, len);
                    } else agree++;
                } else if (k >= 0.0f) {
                    // What the face is: water (group flag 0x2000) or the texture's surface type.
                    const uint32_t sg = Track::Segments::get(track) + seg * TrackSegment::Size;
                    uint32_t flags = 0;
                    int surface = -1;
                    for (int gi = 0; gi < TrackSegment::BatchCount::get(sg); gi++) {
                        const Group g = read_group(sg, gi);
                        if (face >= g.first && face < g.end) {
                            flags = g.flags;
                            surface = (int8_t)rd8(Track::Textures::get(track) + g.texture * 8 + 7);
                        }
                    }
                    if (flags & GROUP_WATER) water++;
                    else {
                        missed++;
                        if (missed <= 6)
                            std::fprintf(stderr, "[jfg] ray sweep: passed a face (segment %d face %d, group flags 0x%X, surface %d) at %.0f along\n",
                                seg, face, flags, surface, k * len);
                    }
                } else agree++;
            }
    std::fprintf(stderr, "[jfg] ray sweep: %d rays, %d agree, %d phantom hits, %d water passed, %d other faces passed\n", rays,
        agree, phantom, water, missed);
}

void ray_probe(recomp_context* ctx) {
    if (g_ray.calls >= 0 && g_ray.calls == 599) ray_sweep(ctx);
    if (!g_ray.on || g_ray.calls < 0 || ++g_ray.calls != 600) { if (!g_ray.on && g_ray.calls >= 0) g_ray.calls++; return; }
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; // in the gap call_game leaves below $sp
    for (int k = 1; k <= 6; k++) {
        const float up = 10.0f * k;
        const V3 a = { g_ray.a[0], g_ray.a[1] + up, g_ray.a[2] }, b = { g_ray.b[0], g_ray.b[1] + up, g_ray.b[2] };
        wrf(buf + 0, a.x); wrf(buf + 4, a.y); wrf(buf + 8, a.z);
        wrf(buf + 12, b.x); wrf(buf + 16, b.y); wrf(buf + 20, b.z);
        const recomp_context saved = *ctx;
        const bool hit = (call_game(trackNearestIntersection, ctx, { 0u, buf, buf + 12, buf + 24, 0u, 0u }) & 0xFF) != 0;
        *ctx = saved;
        int seg = -1, face = -1;
        const float real = nearest_face(a, b, &seg, &face);
        const V3 r = { a.x + (b.x - a.x) * real, a.y + (b.y - a.y) * real, a.z + (b.z - a.z) * real };
        std::fprintf(stderr, "[jfg] ray probe +%.0f: game %s (%.1f, %.1f, %.1f); faces: %s (%.1f, %.1f, %.1f) segment %d face %d\n", up,
            hit ? "hits" : "clear", hit ? rdf(buf + 28) : 0.0f, hit ? rdf(buf + 32) : 0.0f, hit ? rdf(buf + 36) : 0.0f,
            real >= 0.0f ? "hit" : "clear", real >= 0.0f ? r.x : 0.0f, real >= 0.0f ? r.y : 0.0f, real >= 0.0f ? r.z : 0.0f, seg, face);
        if (k == 1 && hit) {
            std::string words;
            for (int w = 0; w < 12; w++) { char t[16]; std::snprintf(t, sizeof(t), " %08X", rd32(buf + 24 + w * 4)); words += t; }
            std::fprintf(stderr, "[jfg] ray probe: result words%s\n", words.c_str());
            // Every plane the hit point lies on (two heights, so the plane is the wall's), with what owns it.
            const V3 p1 = { rdf(buf + 28), rdf(buf + 32), rdf(buf + 36) }, p2 = { p1.x, p1.y + 40.0f, p1.z };
            const uint32_t track = rd32(ADDR_track);
            for (int s = 0; s < Track::SegmentCount::get(track); s++) {
                const uint32_t sg = Track::Segments::get(track) + s * TrackSegment::Size;
                const Bounds bd = read_bounds(Track::Bounds::get(track) + s * TrackBounds::Size);
                if (p1.x < bd.min_x - 50 || p1.x > bd.max_x + 50 || p1.z < bd.min_z - 50 || p1.z > bd.max_z + 50) continue;
                // Faces in plane order: which face owns each face plane, and the slots naming each plane.
                const int faces = TrackSegment::FaceCount::get(sg);
                int colliding = 0, first_bad = -1;
                bool seen_nocollide = false;
                for (int gi = 0; gi < TrackSegment::BatchCount::get(sg); gi++) {
                    const Group g = read_group(sg, gi);
                    for (int t = g.first; t < g.end; t++) {
                        const bool nc = TrackFace::Flags::get(TrackSegment::Faces::get(sg) + t * TrackFace::Size) & FACE_NO_COLLISION;
                        if (nc) seen_nocollide = true;
                        else { if (seen_nocollide && first_bad < 0) first_bad = t; colliding++; }
                    }
                }
                std::fprintf(stderr, "[jfg] ray probe: segment %d bounds x %d..%d z %d..%d: %d faces, %d colliding, first colliding face after a no-collision one: %d\n",
                    s, bd.min_x, bd.max_x, bd.min_z, bd.max_z, faces, colliding, first_bad);
                for (int pl = 0; pl < faces * 4; pl++) {
                    const Plane q = read_plane(sg, pl);
                    if (!(std::fabs(dot(q.n, p1) + q.d) < 1.0f && std::fabs(dot(q.n, p2) + q.d) < 1.0f)) continue;
                    if (!std::isfinite(q.d) || std::fabs(dot(q.n, q.n) - 1.0f) > 0.01f) continue;
                    std::string users;
                    const uint32_t info = TrackSegment::PolyInfo::get(sg);
                    for (int t = 0; t < faces; t++) {
                        const uint32_t pi = info + t * TrackPolyInfo::Size;
                        if (TrackPolyInfo::Plane::get(pi) == pl) users += " own:" + std::to_string(t);
                        for (int e = 0; e < 3; e++)
                            if ((TrackPolyInfo::Edge::get(pi, e) & 0x7FFF) == pl) users += " edge:" + std::to_string(t) + "/" + std::to_string(e);
                    }
                    std::fprintf(stderr, "[jfg] ray probe:   plane %d (%.3f, %.3f, %.3f, %.1f) used by%s\n", pl, q.n.x, q.n.y, q.n.z, q.d,
                        users.empty() ? " nothing" : users.c_str());
                }
            }
        }
    }
}

} // namespace

JFG_WRAP(trackMakePolylist) {
    g_rdram = rdram;
    ray_probe(ctx);
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

// The loader's plane pass on one segment (overlay 24): in a pack level, its open edges are noted first.
JFG_WRAP(func_overlay_24_00000CE4_1F42814) {
    g_rdram = rdram;
    if (seams_on() && rd32(ADDR_nohitsflag) == 0) note_open_edges((uint32_t)ctx->r4);
    func_overlay_24_00000CE4_1F42814_orig(rdram, ctx);
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
    g_open_edges.clear();
    func_overlay_24_000001F8_1F41D28_orig(rdram, ctx);
    const uint32_t track = rd32(ADDR_track);
    if (seams_on() && valid_ptr(track) && rd32(ADDR_nohitsflag) == 0) link_seams();
    if (edges_on() && valid_ptr(track) && rd32(ADDR_nohitsflag) == 0) repair_zero_edge_planes();
    g_open_edges.clear();
    read_ray_probe();
    g_ray.calls = 0;
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
