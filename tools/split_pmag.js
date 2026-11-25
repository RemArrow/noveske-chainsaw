// Cuts the cartridges the pack modelled at the top of its PMAG out of Assets\noveske_pmag.obj, so the plugin can
// show as many as the magazine really holds (an empty magazine showed four rounds).
//   node tools\split_pmag.js            (after tools\prepare_runtime.py, before build.bat)
// Writes Assets\noveske_pmag.obj (the body alone) and Assets\noveske_pmag_round1..4.obj (round 1 the top one,
// at the feed lips), all in the same frame. Each round is a case and a bullet: loose parts, paired by where they
// sit across the magazine (the stack is staggered). Run again on a split file it changes nothing.
const fs = require("fs");
const path = require("path");
const dir = path.join(__dirname, "..", "Assets");
const src = path.join(dir, "noveske_pmag.obj");
const lines = fs.readFileSync(src, "utf8").split(/\r?\n/);

const v = [], vt = [], vn = [], faces = [];
let mtllib = "", usemtl = "";
for (const l of lines) {
    const p = l.trim().split(/\s+/);
    if (p[0] === "v") v.push(l.trim());
    else if (p[0] === "vt") vt.push(l.trim());
    else if (p[0] === "vn") vn.push(l.trim());
    else if (p[0] === "mtllib") mtllib = l.trim();
    else if (p[0] === "usemtl") usemtl = l.trim();
    else if (p[0] === "f") faces.push(p.slice(1).map(s => s.split("/").map(x => (x ? Number(x) : 0))));
}
const pos = v.map(s => s.split(/\s+/).slice(1, 4).map(Number));

// Loose parts: faces joined by shared vertices (or vertices at the same place).
const parent = pos.map((_, i) => i);
const find = x => (parent[x] === x ? x : (parent[x] = find(parent[x])));
const join = (a, b) => { parent[find(a)] = find(b); };
for (const f of faces) for (let i = 1; i < f.length; i++) join(f[0][0] - 1, f[i][0] - 1);
const at = new Map();
pos.forEach((p, i) => { const k = p.map(x => x.toFixed(5)).join(); at.has(k) ? join(i, at.get(k)) : at.set(k, i); });
const parts = new Map();
faces.forEach(f => { const r = find(f[0][0] - 1); if (!parts.has(r)) parts.set(r, []); parts.get(r).push(f); });

const info = [...parts.values()].map(fs_ => {
    let ymax = -1e9, zlo = 1e9, zhi = -1e9, xlo = 1e9;
    for (const f of fs_) for (const c of f) { const p = pos[c[0] - 1]; ymax = Math.max(ymax, p[1]); xlo = Math.min(xlo, p[0]); zlo = Math.min(zlo, p[2]); zhi = Math.max(zhi, p[2]); }
    return { faces: fs_, ymax, xlo, z: (zlo + zhi) / 2 };
}).sort((a, b) => b.faces.length - a.faces.length);
const body = info[0], loose = info.slice(1);
if (loose.length === 0) { console.log("noveske_pmag.obj has no loose rounds - already split"); process.exit(0); }
if (loose.length !== 8) { console.error(`expected 8 loose parts (4 cases + 4 bullets), found ${loose.length}`); process.exit(1); }

// A case and its bullet: the two parts whose centres across the magazine (Z) and heights are nearest.
const rounds = [];
const left = loose.slice().sort((a, b) => a.xlo - b.xlo);  // cases (from the base, X 0) first, then the bullets ahead of them
for (const c of left.slice(0, 4)) {
    let best = -1, bd = 1e9;
    left.slice(4).forEach((b, i) => {
        if (b.used) return;
        const d = Math.abs(b.z - c.z) + Math.abs(b.ymax - c.ymax);
        if (d < bd) { bd = d; best = i; }
    });
    const b = left[4 + best];
    b.used = true;
    rounds.push({ faces: c.faces.concat(b.faces), ymax: Math.max(c.ymax, b.ymax) });
}
rounds.sort((a, b) => b.ymax - a.ymax);  // top round first

function write(file, fs_, name) {
    const mv = new Map(), mt = new Map(), mn = new Map();
    const out = { v: [], vt: [], vn: [] };
    const idx = (m, arr, key, src) => { if (!key) return ""; if (!m.has(key)) { arr.push(src[key - 1]); m.set(key, arr.length); } return m.get(key); };
    const fl = fs_.map(f => "f " + f.map(c => [idx(mv, out.v, c[0], v), idx(mt, out.vt, c[1], vt), idx(mn, out.vn, c[2], vn)].join("/")).join(" "));
    const text = ["# split from the pack's PMAG by tools/split_pmag.js", mtllib, "o " + name, ...out.v, ...out.vt, ...out.vn, "s 1", usemtl, ...fl, ""]
        .filter(x => x !== "").join("\n") + "\n";
    fs.writeFileSync(path.join(dir, file), text);
    console.log(`${file}: ${fs_.length} faces`);
}
rounds.forEach((r, i) => write(`noveske_pmag_round${i + 1}.obj`, r.faces, `ChainSaw_PMag_Round${i + 1}`));
write("noveske_pmag.obj", body.faces, "ChainSaw_PMag");
