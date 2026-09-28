// align - computes the offset that puts the Noveske mesh on the HK416's lower receiver.
//
//   align <HK_416_279_v2.uexp> <noveske.obj> [movables.uexp]
//
// 1. Pulls vertex positions out of the cooked HK416 static mesh: FPositionVertexBuffer is
//    serialized as [stride=12][count][elementSize=12][count][count * float3].
// 2. Loads the Noveske OBJ with the same transform the GML loader applies (Blender OBJ axes:
//    UE = (x, z, y) * 100).
// 3. Crops both to the lower receiver (magwell, trigger guard, grip, fire-control area) and runs a
//    translation-only trimmed ICP: nearest neighbours, keep the best 70% of matches, move by their
//    median displacement, repeat. All AR-pattern lowers share the fire-control pin positions, so
//    this is what lines the HK416's trigger/selector/bolt catch up with the Noveske's lower.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

static std::vector<V> LoadObj(const char* p) {
    std::ifstream f(p);
    std::vector<V> pts;
    std::string line;
    while (std::getline(f, line)) {
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

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: align <HK_416_279_v2.uexp> <noveske.obj>\n"); return 1; }
    auto bufs = PositionBuffers(ReadAll(argv[1]));
    if (bufs.empty()) { printf("no position buffer found\n"); return 1; }
    std::vector<V> hk = bufs[0];
    std::vector<V> nov = LoadObj(argv[2]);
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
    return 0;
}
