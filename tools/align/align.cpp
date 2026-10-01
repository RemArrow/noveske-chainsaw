// align - computes the offset that puts the Noveske mesh on the HK416's lower receiver.
//
//   align <HK_416_279_v2.uexp> <noveske.obj> [offsetX offsetY offsetZ]
//
// 1. Pulls vertex positions out of the cooked HK416 static mesh: FPositionVertexBuffer is
//    serialized as [stride=12][count][elementSize=12][count][count * float3].
// 2. Loads the Noveske OBJ with the same transform the GML loader applies (Blender OBJ axes:
//    UE = (x, z, y) * 100).
// 3. Crops both to the lower receiver (magwell, trigger guard, grip, fire-control area) and runs a
//    translation-only trimmed ICP: nearest neighbours, keep the best 70% of matches, move by their
//    median displacement, repeat. All AR-pattern lowers share the fire-control pin positions, so
//    this is what lines the HK416's trigger/selector/bolt catch up with the Noveske's lower.
// 4. With that offset, finds where the Noveske's own rails are: for each of the HK416's attachment
//    splines, rays from the Noveske's bore axis in the spline's direction give its rail / M-LOK
//    surface. Prints the plugin's kAttachPoints table (spline start points on the Noveske) and the
//    muzzle attach point (the front of the Noveske's barrel, where its flash hider begins).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using V = std::array<double, 3>;

static std::vector<char> ReadAll(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// All position buffers in a cooked mesh file (LOD0 is the first, largest one).
static std::vector<std::vector<V>> PositionBuffers(const std::vector<char>& b) {
    std::vector<std::vector<V>> out;
    for (size_t i = 0; i + 16 < b.size(); i += 4) {
        int32_t s1, n1, s2, n2;
        memcpy(&s1, &b[i], 4); memcpy(&n1, &b[i + 4], 4); memcpy(&s2, &b[i + 8], 4); memcpy(&n2, &b[i + 12], 4);
        if (s1 != 12 || s2 != 12 || n1 != n2 || n1 < 100 || n1 > 2000000) continue;
        size_t start = i + 16, bytes = (size_t)n1 * 12;
        if (start + bytes > b.size()) continue;
        std::vector<V> pts(n1);
        bool sane = true;
        for (int k = 0; k < n1 && sane; k++) {
            float f[3];
            memcpy(f, &b[start + k * 12], 12);
            for (float x : f) sane &= std::isfinite(x) && std::fabs(x) < 500;
            pts[k] = {f[0], f[1], f[2]};
        }
        if (sane) { out.push_back(std::move(pts)); i = start + bytes - 4; }
    }
    return out;
}

struct Tri { int a, b, c; std::string part; };

static std::vector<V> LoadObj(const char* p, std::vector<Tri>* tris = nullptr) {
    std::ifstream f(p);
    std::vector<V> pts;
    std::string line, part;
    while (std::getline(f, line)) {
        if (line.rfind("usemtl ", 0) == 0) part = line.substr(7);
        if (tris && line.rfind("f ", 0) == 0) {
            std::istringstream ss(line.substr(2));
            std::string tok;
            std::vector<int> idx;
            while (ss >> tok) {
                int i = std::stoi(tok.substr(0, tok.find('/')));
                idx.push_back(i < 0 ? (int)pts.size() + i : i - 1);
            }
            for (size_t k = 1; k + 1 < idx.size(); k++) tris->push_back({idx[0], idx[k], idx[k + 1], part});
        }
        if (line.size() < 2 || line[0] != 'v' || line[1] != ' ') continue;
        std::istringstream ss(line.substr(2));
        double x, y, z;
        ss >> x >> y >> z;
        pts.push_back({x * 100, z * 100, y * 100});  // GML_AXIS_BLENDER_OBJ: UE = (x, z, y) * 100
    }
    return pts;
}

static void Bounds(const char* what, const std::vector<V>& p) {
    V lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    for (auto& v : p) for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], v[a]); hi[a] = std::max(hi[a], v[a]); }
    printf("%-22s %7zu pts  X[%8.3f..%8.3f] Y[%8.3f..%8.3f] Z[%8.3f..%8.3f]\n", what, p.size(), lo[0], hi[0], lo[1],
           hi[1], lo[2], hi[2]);
}

// Uniform grid for nearest-neighbour queries.
struct Grid {
    double cell;
    std::unordered_map<long long, std::vector<int>> cells;
    const std::vector<V>* pts;
    static long long Key(int x, int y, int z) { return ((long long)(x + 4096) << 26) | ((long long)(y + 4096) << 13) | (z + 4096); }
    Grid(const std::vector<V>& p, double c) : cell(c), pts(&p) {
        for (int i = 0; i < (int)p.size(); i++)
            cells[Key((int)std::floor(p[i][0] / c), (int)std::floor(p[i][1] / c), (int)std::floor(p[i][2] / c))].push_back(i);
    }
    double Nearest(const V& q, V& best) const {
        int cx = (int)std::floor(q[0] / cell), cy = (int)std::floor(q[1] / cell), cz = (int)std::floor(q[2] / cell);
        double bd = 1e30;
        for (int r = 1; r <= 3 && bd > (r - 1) * cell * (r - 1) * cell; r++)
            for (int dx = -r; dx <= r; dx++) for (int dy = -r; dy <= r; dy++) for (int dz = -r; dz <= r; dz++) {
                auto it = cells.find(Key(cx + dx, cy + dy, cz + dz));
                if (it == cells.end()) continue;
                for (int i : it->second) {
                    const V& p = (*pts)[i];
                    double d = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
                    if (d < bd) { bd = d; best = p; }
                }
            }
        return bd;
    }
};

static std::vector<V> Crop(const std::vector<V>& p, const V& lo, const V& hi, const V& shift = {0, 0, 0}) {
    std::vector<V> o;
    for (auto& v : p) {
        V s{v[0] + shift[0], v[1] + shift[1], v[2] + shift[2]};
        if (s[0] >= lo[0] && s[0] <= hi[0] && s[1] >= lo[1] && s[1] <= hi[1] && s[2] >= lo[2] && s[2] <= hi[2]) o.push_back(v);
    }
    return o;
}

static double Median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

static V Sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
static V Cross(V a, V b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
static double Dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Farthest hit (Moller-Trumbore) on the given parts along a ray, within maxDist; -1 if none.
static double Outermost(const std::vector<V>& vs, const std::vector<Tri>& tris, V o, V d, double maxDist) {
    double best = -1;
    for (auto& t : tris) {
        if (t.part != "MI_Body" && t.part != "MI_Handguard") continue;
        V e1 = Sub(vs[t.b], vs[t.a]), e2 = Sub(vs[t.c], vs[t.a]), p = Cross(d, e2);
        double det = Dot(e1, p);
        if (std::fabs(det) < 1e-12) continue;
        V s = Sub(o, vs[t.a]), q = Cross(s, e1);
        double u = Dot(s, p) / det, v = Dot(d, q) / det, dist = Dot(e2, q) / det;
        if (u >= 0 && v >= 0 && u + v <= 1 && dist > 0 && dist < maxDist) best = std::max(best, dist);
    }
    return best;
}

// The HK416 receiver's attachment splines (Firearm_Rifle_HK416A5_279mm_C), in actor space (cm), as
// logged by tests/NoveskeDev ("ATTACH" lines). theta: direction from the bore, degrees from +Z
// (up) toward +Y. All run along +X from x0 to x1.
struct HkSpline { const char* name; double theta, y, z, x0, x1; };
static const HkSpline kHkSplines[] = {
    {"ReceiverAttachmentSpline", 0, 0.000, 6.510, -4.825, 9.175},  // Picatinny, upper receiver
    {"ReceiverAttachmentSplineHG12", 0, 0.000, 6.510, 10.709, 30.709},  // Picatinny, handguard top
    {"ReceiverAttachmentSplineHG45", 45, 1.592, 4.156, 15.523, 30.523},  // M-LOK from here on
    {"ReceiverAttachmentSplineHG9", 90, 2.100, 2.926, 15.523, 30.023},
    {"ReceiverAttachmentSplineHG135", 135, 1.592, 1.675, 15.523, 30.523},
    {"ReceiverAttachmentSplineHG6", 180, 0.000, 0.414, 15.523, 30.023},
    {"ReceiverAttachmentSplineHG-135", -135, -1.592, 1.675, 15.523, 30.523},
    {"ReceiverAttachmentSplineHG-90", -90, -2.100, 2.926, 15.523, 30.023},
    {"ReceiverAttachmentSplineHG-45", -45, -1.592, 4.156, 15.523, 30.523},
};
static const V kHkFireLocation{35.899, 0.000, 2.929};  // "Fire Location", tag MuzzleAttachLocation

static void AttachPoints(const std::vector<V>& nov, const std::vector<Tri>& tris) {
    // The Noveske's bore: centre of its muzzle's front ring; the barrel starts where the muzzle does.
    double mx0 = 1e9, mx1 = -1e9;
    std::vector<char> muzzle(nov.size(), 0);
    for (auto& t : tris)
        if (t.part == "MI_Muzzle") muzzle[t.a] = muzzle[t.b] = muzzle[t.c] = 1;
    for (size_t i = 0; i < nov.size(); i++)
        if (muzzle[i]) mx0 = std::min(mx0, nov[i][0]), mx1 = std::max(mx1, nov[i][0]);
    V lo{0, 1e9, 1e9}, hi{0, -1e9, -1e9};
    for (size_t i = 0; i < nov.size(); i++)
        if (muzzle[i] && nov[i][0] > mx1 - 1)
            for (int a = 1; a < 3; a++) lo[a] = std::min(lo[a], nov[i][a]), hi[a] = std::max(hi[a], nov[i][a]);
    const double by = (lo[1] + hi[1]) / 2, bz = (lo[2] + hi[2]) / 2;
    printf("\nNoveske bore axis y=%.3f z=%.3f, muzzle device x %.3f..%.3f\n", by, bz, mx0, mx1);

    printf("\nstatic const AttachPoint kAttachPoints[] = {  // spline start (point 0) on the Noveske\n");
    for (auto& s : kHkSplines) {
        double t = s.theta * 3.14159265358979 / 180, dy = std::sin(t), dz = std::cos(t);
        std::vector<double> r;  // surface distance per x sample; slots and cuts give no hit
        for (double x = s.x0; x <= s.x1; x += 0.1) {
            double h = Outermost(nov, tris, {x, by, bz}, {0, dy, dz}, 5);
            if (h > 0) r.push_back(h);
        }
        if (r.size() < 10) { printf("    // %s: no Noveske surface found\n", s.name); continue; }
        // Rail tooth tops / M-LOK faces: the 90th percentile (grooves and slots sit below or miss).
        std::sort(r.begin(), r.end());
        double face = r[r.size() * 9 / 10];
        V p{s.x0, by + dy * face, bz + dz * face};
        printf("    {\"%s\", %.3f, %.3f, %.3f},  // HK (%.3f, %.3f), %.0f%% of rays hit\n", s.name, p[0], p[1], p[2], s.y,
               s.z, 100.0 * r.size() / ((s.x1 - s.x0) / 0.1 + 1));
    }
    printf("    {\"Fire Location\", %.3f, %.3f, %.3f},  // HK (%.3f, %.3f, %.3f)\n};\n", mx0, by, bz, kHkFireLocation[0],
           kHkFireLocation[1], kHkFireLocation[2]);
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: align <HK_416_279_v2.uexp> <noveske.obj> [offsetX offsetY offsetZ]\n"); return 1; }
    auto bufs = PositionBuffers(ReadAll(argv[1]));
    if (bufs.empty()) { printf("no position buffer found\n"); return 1; }
    std::vector<V> hk = bufs[0];
    std::vector<Tri> tris;
    std::vector<V> nov = LoadObj(argv[2], &tris);
    // The flash hider is its own mesh (noveske_muzzle.obj next to it); the bore and muzzle point come from it.
    std::string muzzlePath = std::string(argv[2]);
    muzzlePath = muzzlePath.substr(0, muzzlePath.find_last_of("/\\") + 1) + "noveske_muzzle.obj";
    std::vector<Tri> mtris;
    std::vector<V> mverts = LoadObj(muzzlePath.c_str(), &mtris);
    for (auto& t : mtris) tris.push_back({t.a + (int)nov.size(), t.b + (int)nov.size(), t.c + (int)nov.size(), t.part});
    nov.insert(nov.end(), mverts.begin(), mverts.end());
    printf("%zu position buffer(s) in the HK file (using LOD0)\n", bufs.size());
    Bounds("HK416 static (LOD0)", hk);
    Bounds("Noveske (as rendered)", nov);

    // Lower receiver region in the HK416's actor space (cm): behind the handguard, in front of the
    // buffer tube, below the upper/lower split. Covers magwell, trigger guard, grip, FCG pins.
    const V lo{-16, -6, -14}, hi{10, 6, 1.5};
    std::vector<V> target = Crop(hk, lo, hi);
    Grid grid(target, 0.5);
    V t{0, 0, 0};
    double rms = 0;
    for (int it = 0; it < 60; it++) {
        std::vector<V> src = Crop(nov, {lo[0] - 3, lo[1] - 3, lo[2] - 3}, {hi[0] + 3, hi[1] + 3, hi[2] + 3}, t);
        std::vector<std::pair<double, V>> m;  // (distance^2, displacement)
        for (auto& s : src) {
            V q{s[0] + t[0], s[1] + t[1], s[2] + t[2]}, nn;
            double d = grid.Nearest(q, nn);
            if (d < 9.0) m.push_back({d, V{nn[0] - q[0], nn[1] - q[1], nn[2] - q[2]}});
        }
        if (m.size() < 50) { printf("too few matches (%zu)\n", m.size()); return 1; }
        std::sort(m.begin(), m.end(), [](auto& a, auto& b) { return a.first < b.first; });
        m.resize(m.size() * 7 / 10);  // trimmed: best 70%
        std::vector<double> dx, dy, dz;
        double sum = 0;
        for (auto& [d, v] : m) { dx.push_back(v[0]); dy.push_back(v[1]); dz.push_back(v[2]); sum += d; }
        V step{Median(dx), Median(dy), Median(dz)};
        t = {t[0] + step[0], t[1] + step[1], t[2] + step[2]};
        rms = std::sqrt(sum / m.size());
        if (it % 10 == 0 || std::fabs(step[0]) + std::fabs(step[1]) + std::fabs(step[2]) < 1e-4)
            printf("iter %2d  offset (%7.3f, %7.3f, %7.3f) cm   rms %.3f cm over %zu matches\n", it, t[0], t[1], t[2], rms, m.size());
        if (std::fabs(step[0]) + std::fabs(step[1]) + std::fabs(step[2]) < 1e-4) break;
    }
    printf("\nRESULT offset X=%.3f Y=%.3f Z=%.3f cm (rms %.3f cm)\n", t[0], t[1], t[2], rms);

    // The attach points below are for the plugin's default offset; the plugin shifts them by any
    // difference to the configured one. Pass that default as argv[3..5] to reproduce the table.
    V use = t;
    if (argc >= 6) use = {atof(argv[3]), atof(argv[4]), atof(argv[5])};
    printf("attach points computed with offset (%.3f, %.3f, %.3f)\n", use[0], use[1], use[2]);
    for (auto& v : nov) v = {v[0] + use[0], v[1] + use[1], v[2] + use[2]};
    AttachPoints(nov, tris);
    return 0;
}
