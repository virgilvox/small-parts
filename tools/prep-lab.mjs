// Turns the HeatSync Labs 108 model (HSL-108-full.glb, metres, y up, one mesh
// per part, colour-only materials) into the game's lab:
//
//   assets/models/lab.glb   the building at toy scale, merged into a few chunks:
//                           every opaque part is one vertex coloured mesh per
//                           band along the room, emissive and glass parts keep
//                           their own materials
//   src/gen/lab_areas.h     the model's 29 area tags as boxes in game units
//
//   node tools/prep-lab.mjs <HSL-108-full.glb>
//
// The Elegoo Centauri Carbon 2 printers in the model are converted from a
// GPL-3.0 CAD release, so they are dropped and plain printer boxes of the same
// size stand in their place. tools/lab-fixes.mjs holds corrections from people
// who know the room (moved furniture, the supply closet doors, the basement).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FT, moveFor, carveFor, subtract, extras } from './lab-fixes.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SCALE = 20;        // 1 m of lab is 20 game units: a 1.8 unit hero is 9 cm tall
const BAND = 4 * SCALE;  // chunk width along the room (x), in game units
const K = FT * SCALE;    // feet to game units
const src = process.argv[2];
if (!src) { console.error('usage: node tools/prep-lab.mjs <HSL-108-full.glb>'); process.exit(1); }

const glb = readFileSync(src);
const jlen = glb.readUInt32LE(12);
const J = JSON.parse(glb.subarray(20, 20 + jlen).toString());
const binStart = 20 + jlen + 8;
const BIN = glb.subarray(binStart, binStart + glb.readUInt32LE(20 + jlen));

function read(accIndex) {
  const a = J.accessors[accIndex], v = J.bufferViews[a.bufferView];
  const comps = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 }[a.type];
  const off = (v.byteOffset || 0) + (a.byteOffset || 0);
  const n = a.count * comps;
  const T = { 5126: Float32Array, 5125: Uint32Array, 5123: Uint16Array, 5121: Uint8Array }[a.componentType];
  const stride = v.byteStride || comps * T.BYTES_PER_ELEMENT;
  if (stride !== comps * T.BYTES_PER_ELEMENT) throw new Error('interleaved accessor');
  return new T(BIN.buffer.slice(BIN.byteOffset + off, BIN.byteOffset + off + n * T.BYTES_PER_ELEMENT));
}

const isPrinter = (name) => /centauri/i.test(name || '');

// chunks: key -> { mat, pos: [], col: [], idx: [] }
const chunks = new Map();
function chunk(key, mat) {
  if (!chunks.has(key)) chunks.set(key, { mat, pos: [], col: [], idx: [], nv: 0 });
  return chunks.get(key);
}
const areas = new Map();       // area tag -> [minx, miny, minz, maxx, maxy, maxz]
// named parts the missions use: each instance's box, in node order
const LANDMARKS = [
  ['LASER_LID', 'CO2 laser 80W lid'], ['SCRAP_BIN', 'Scrap offcut bin'], ['SCRAP_SHELF', 'Laser scrap shelves shelf'],
  ['MEMBER_BOX', 'Member banker box'], ['SEWING_TABLE', 'Industrial sewing table 23 top'], ['THREAD_RACK', 'Thread spool rack'],
  ['LEGO_TOWER', 'LEGO drawer tower frame top'], ['CRAFT_TABLE', 'Folding work table top'], ['PRINTER_RACK', '3D printer rack 1 shelf'],
  ['COWORK_TABLE', 'Coworking table top'], ['SOFA_SEAT', 'Sofa seat cushion'], ['COFFEE_TABLE', 'Coffee table ply top'],
  ['ESD_MAT', 'ESD mat'], ['SOLDER_STATION', 'Soldering iron station'], ['MCU_CABINET', 'Microcontrollers cabinet top'],
  ['WOOD_BENCH', 'Eight foot wood bench top'], ['BACK_BENCH', 'Back wall wood bench top'], ['LATHE_BED', 'Lathe bed'],
  ['PANEL', 'Electrical panel'], ['COUNTER_DRONE', 'Counter drone'],
  ['GALLERY_TIER', 'Gallery tier'], ['GALLERY_PEDESTAL', 'Gallery pedestal'], ['HACKER_DECK', 'Hacker bay display deck'],
  ['DUCK_RISER', 'Duck riser'], ['ROBOT_COUNTER', 'Display counter'], ['LED_CUBE', 'LED cube case'],
  ['KITCHEN_TABLE', 'Kitchen table top'], ['DARK_COUNTER', 'Darkroom dry counter top'], ['DARK_SINK', 'Darkroom sink basin'],
  ['SUPPLY_SHELF', 'Supply closet shelving shelf'], ['WELD_TABLE', 'Weld table plate'], ['ANVIL', 'Anvil face'],
];
const landmarks = new Map(LANDMARKS.map(([k]) => [k, []]));
const lmOf = new Map(LANDMARKS.map(([k, n]) => [n, k]));
const printers = new Map();    // printer name -> aabb
const grow = (b, x, y, z) => { b[0] = Math.min(b[0], x); b[1] = Math.min(b[1], y); b[2] = Math.min(b[2], z); b[3] = Math.max(b[3], x); b[4] = Math.max(b[4], y); b[5] = Math.max(b[5], z); };
const box0 = () => [1e9, 1e9, 1e9, -1e9, -1e9, -1e9];

let dropped = 0, kept = 0;
for (const n of J.nodes) {
  if (n.mesh === undefined) continue;
  for (const p of J.meshes[n.mesh].primitives) {
    const pos = read(p.attributes.POSITION);
    const idx = p.indices !== undefined ? read(p.indices) : Uint32Array.from({ length: pos.length / 3 }, (_, i) => i);
    if (isPrinter(n.name)) {
      if (!printers.has(n.name)) printers.set(n.name, box0());
      const b = printers.get(n.name);
      for (let i = 0; i < pos.length; i += 3) grow(b, pos[i] * SCALE, pos[i + 1] * SCALE, pos[i + 2] * SCALE);
      dropped++;
      continue;
    }
    kept++;
    /* the corrections: moved furniture, cut openings (lab-fixes.mjs, in feet) */
    const ex = n.extras || {};
    const mnF = [1e9, 1e9, 1e9], mxF = [-1e9, -1e9, -1e9];
    for (let i = 0; i < pos.length; i += 3) {
      const f = [pos[i] / FT, pos[i + 2] / FT, pos[i + 1] / FT];
      for (let k = 0; k < 3; k++) { mnF[k] = Math.min(mnF[k], f[k]); mxF[k] = Math.max(mxF[k], f[k]); }
    }
    const mv = moveFor(n.name, ex.group, [(mnF[0] + mxF[0]) / 2, (mnF[1] + mxF[1]) / 2]);
    if (mv) {
      for (let i = 0; i < pos.length; i += 3) { pos[i] += mv.dx * FT; pos[i + 2] += mv.dy * FT; }
      mnF[0] += mv.dx; mxF[0] += mv.dx; mnF[1] += mv.dy; mxF[1] += mv.dy;
    }
    const cuts = carveFor(n.name, ex.group, mnF, mxF);
    const m = J.materials[p.material] || {};
    const pbr = m.pbrMetallicRoughness || {};
    const c = pbr.baseColorFactor || [1, 1, 1, 1];
    const special = m.alphaMode === 'BLEND' || (m.emissiveFactor && m.emissiveFactor.some((e) => e > 0));
    let cx = 0;
    for (let i = 0; i < pos.length; i += 3) cx += pos[i];
    cx = (cx / (pos.length / 3)) * SCALE;
    const band = Math.max(0, Math.floor(cx / BAND));
    const ch = special ? chunk(`m${p.material}`, p.material) : chunk(`b${band}`, -1);
    const base = ch.nv;
    const area = n.extras && n.extras.area;
    if (area && !areas.has(area)) areas.set(area, box0());
    if (lmOf.has(n.name)) {
      const b = box0();
      for (let i = 0; i < pos.length; i += 3) grow(b, pos[i] * SCALE, pos[i + 1] * SCALE, pos[i + 2] * SCALE);
      landmarks.get(lmOf.get(n.name)).push(b);
    }
    if (cuts && isBox(pos)) {
      let pieces = [[mnF[0], mnF[1], mnF[2], mxF[0], mxF[1], mxF[2]]];
      for (const cut of cuts) pieces = pieces.flatMap((b) => subtract(b, cut));
      const rgb = c.slice(0, 3).map((v) => Math.round(Math.pow(v, 1 / 2.2) * 255));
      const bc = chunk(`b${band}`, -1);
      for (const b of pieces) { addBox(bc, ftBox(b), rgb); if (area) { grow(areas.get(area), b[0] * K, b[2] * K, b[1] * K); grow(areas.get(area), b[3] * K, b[5] * K, b[4] * K); } }
      continue;
    }
    if (cuts) {
      /* not a box (the ground slab is the building's outline, extruded): clip its
       * triangles against each cut's footprint, which runs the full height */
      const bc = chunk(`b${band}`, -1), col = c.slice(0, 3);
      for (let t = 0; t < idx.length; t += 3) {
        let polys = [[0, 1, 2].map((j) => { const q = idx[t + j] * 3; return [pos[q] * SCALE, pos[q + 1] * SCALE, pos[q + 2] * SCALE]; })];
        for (const cut of cuts) polys = polys.flatMap((pl) => outsideRect(pl, cut[0] * K, cut[1] * K, cut[3] * K, cut[4] * K));
        for (const pl of polys) {
          const b0 = bc.nv;
          for (const v of pl) { bc.pos.push(...v); bc.col.push(...col); if (area) grow(areas.get(area), ...v); }
          bc.nv += pl.length;
          for (let k = 1; k + 1 < pl.length; k++) bc.idx.push(b0, b0 + k, b0 + k + 1);
        }
      }
      continue;
    }
    for (let i = 0; i < pos.length; i += 3) {
      const x = pos[i] * SCALE, y = pos[i + 1] * SCALE, z = pos[i + 2] * SCALE;
      ch.pos.push(x, y, z);
      if (area) grow(areas.get(area), x, y, z);
      if (!special) ch.col.push(c[0], c[1], c[2]);   // glTF vertex colours are linear, like the factors
    }
    ch.nv += pos.length / 3;
    for (const k of idx) ch.idx.push(base + k);
  }
}

// feet (x, y across, z up) to game units (x, y up, z across)
function ftBox(b) { return [b[0] * K, b[2] * K, b[1] * K, b[3] * K, b[5] * K, b[4] * K]; }
// a part that is one axis-aligned box: every vertex sits at its min or max on each axis
function isBox(pos) {
  const mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
  for (let i = 0; i < pos.length; i += 3) for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], pos[i + k]); mx[k] = Math.max(mx[k], pos[i + k]); }
  for (let i = 0; i < pos.length; i += 3) for (let k = 0; k < 3; k++) {
    const v = pos[i + k];
    if (Math.abs(v - mn[k]) > 1e-4 && Math.abs(v - mx[k]) > 1e-4) return false;
  }
  return true;
}
const lin = (rgb) => rgb.map((x) => Math.pow(x / 255, 2.2));
// Sutherland-Hodgman against one half plane: keep where sign * (v[axis] - at) >= 0
function clipHalf(pl, axis, at, sign) {
  const out = [];
  for (let i = 0; i < pl.length; i++) {
    const a = pl[i], b = pl[(i + 1) % pl.length];
    const da = sign * (a[axis] - at), db = sign * (b[axis] - at);
    if (da >= 0) out.push(a);
    if ((da >= 0) !== (db >= 0)) { const t = da / (da - db); out.push(a.map((v, k) => v + (b[k] - v) * t)); }
  }
  return out;
}
// a polygon (game units) minus the rectangle x0..x1 by z0..z1: up to four convex pieces
function outsideRect(pl, x0, z0, x1, z1) {
  const keep = (p) => p.length >= 3;
  const left = clipHalf(pl, 0, x0, -1), right = clipHalf(pl, 0, x1, 1);
  const mid = clipHalf(clipHalf(pl, 0, x0, 1), 0, x1, -1);
  const near = clipHalf(mid, 2, z0, -1), far = clipHalf(mid, 2, z1, 1);
  return [left, right, near, far].filter(keep);
}
function bandOf(xGame) { return chunk(`b${Math.max(0, Math.floor(xGame / BAND))}`, -1); }

// a plain enclosed printer for each dropped one: a dark cabinet with a lighter door
function addBox(ch, b, rgb) {
  const [x0, y0, z0, x1, y1, z1] = b, base = ch.nv;
  const v = [[x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0], [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]];
  for (const p of v) { ch.pos.push(...p); ch.col.push(...rgb.map((x) => Math.pow(x / 255, 2.2))); }
  ch.nv += 8;
  const f = [0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5];
  for (const k of f) ch.idx.push(base + k);
}
for (const b of printers.values()) {
  const band = Math.max(0, Math.floor(((b[0] + b[3]) / 2) / BAND));
  const ch = chunk(`b${band}`, -1);
  addBox(ch, b, [46, 48, 52]);
  const inset = 0.06 * SCALE;
  addBox(ch, [b[0] + inset, b[1] + (b[4] - b[1]) * 0.25, b[5] - 0.02, b[3] - inset, b[4] - inset, b[5] + 0.01], [150, 160, 170]);   // the door faces the room (+z)
}

// what the model lacks (lab-fixes.mjs): the supply closet doors, the hatch, the basement
extras({
  box(b, rgb) { const g = ftBox(b); addBox(bandOf((g[0] + g[3]) / 2), g, rgb); },
  cyl(a, b, r, rgb) {   /* an eight sided pipe from a to b (feet) */
    const A = [a[0] * K, a[2] * K, a[1] * K], B = [b[0] * K, b[2] * K, b[1] * K], R = r * K;
    const d = [B[0] - A[0], B[1] - A[1], B[2] - A[2]], dl = Math.hypot(...d), u = d.map((v) => v / dl);
    const t = Math.abs(u[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
    const cr = (p, q) => [p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0]];
    let e1 = cr(u, t); const l1 = Math.hypot(...e1); e1 = e1.map((v) => v / l1);
    const e2 = cr(u, e1);
    const ch = bandOf((A[0] + B[0]) / 2), base = ch.nv, col = lin(rgb);
    for (const P of [A, B]) for (let i = 0; i < 8; i++) {
      const an = (i / 8) * Math.PI * 2, cs = Math.cos(an) * R, sn = Math.sin(an) * R;
      ch.pos.push(P[0] + e1[0] * cs + e2[0] * sn, P[1] + e1[1] * cs + e2[1] * sn, P[2] + e1[2] * cs + e2[2] * sn);
      ch.col.push(...col);
    }
    ch.nv += 16;
    for (let i = 0; i < 8; i++) { const j = (i + 1) % 8; ch.idx.push(base + i, base + j, base + 8 + j, base + i, base + 8 + j, base + 8 + i); }
  },
  grid(x0, y0, x1, y1, nx, ny, zf, colf) {   /* a heightfield over feet x, y; z from zf */
    const ch = bandOf(((x0 + x1) / 2) * K), base = ch.nv;
    for (let j = 0; j <= ny; j++) for (let i = 0; i <= nx; i++) {
      const x = x0 + ((x1 - x0) * i) / nx, y = y0 + ((y1 - y0) * j) / ny;
      ch.pos.push(x * K, zf(x, y) * K, y * K);
      ch.col.push(...lin(colf(x, y)));
    }
    ch.nv += (nx + 1) * (ny + 1);
    for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) {
      const a = base + j * (nx + 1) + i, b = a + 1, c2 = a + nx + 1, d = c2 + 1;
      ch.idx.push(a, c2, b, b, c2, d);
    }
  },
});

// write the merged GLB
const views = [], accessors = [], meshes = [], nodes = [], bins = [];
let off = 0;
function push(typed, target) {
  const buf = Buffer.from(typed.buffer, typed.byteOffset, typed.byteLength);
  const pad = (4 - (buf.length % 4)) % 4;
  views.push({ buffer: 0, byteOffset: off, byteLength: buf.length, target });
  bins.push(buf, Buffer.alloc(pad));
  off += buf.length + pad;
  return views.length - 1;
}
const materials = [{ name: 'lab', pbrMetallicRoughness: { baseColorFactor: [1, 1, 1, 1], metallicFactor: 0, roughnessFactor: 0.85 }, doubleSided: true }];
const matMap = new Map();
for (const [key, ch] of [...chunks].sort()) {
  let mi = 0;
  if (ch.mat >= 0) {
    if (!matMap.has(ch.mat)) { matMap.set(ch.mat, materials.length); materials.push(J.materials[ch.mat]); }
    mi = matMap.get(ch.mat);
  }
  const P = Float32Array.from(ch.pos);
  const mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
  for (let i = 0; i < P.length; i += 3) for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], P[i + k]); mx[k] = Math.max(mx[k], P[i + k]); }
  const big = ch.nv > 65535;
  const I = big ? Uint32Array.from(ch.idx) : Uint16Array.from(ch.idx);
  const attributes = { POSITION: accessors.length };
  accessors.push({ bufferView: push(P, 34962), componentType: 5126, count: ch.nv, type: 'VEC3', min: mn, max: mx });
  if (ch.col.length) {
    attributes.COLOR_0 = accessors.length;
    accessors.push({ bufferView: push(Float32Array.from(ch.col), 34962), componentType: 5126, count: ch.nv, type: 'VEC3' });
  }
  const ia = accessors.length;
  accessors.push({ bufferView: push(I, 34963), componentType: big ? 5125 : 5123, count: I.length, type: 'SCALAR' });
  meshes.push({ name: key, primitives: [{ attributes, indices: ia, material: mi }] });
  nodes.push({ name: key, mesh: meshes.length - 1 });
}
const out = { asset: { version: '2.0', generator: 'small-parts prep-lab' }, scene: 0, scenes: [{ nodes: nodes.map((_, i) => i) }], nodes, meshes, materials, accessors, bufferViews: views, buffers: [{ byteLength: off }] };
let js = Buffer.from(JSON.stringify(out));
js = Buffer.concat([js, Buffer.alloc((4 - (js.length % 4)) % 4, 0x20)]);
const bin = Buffer.concat(bins);
const head = Buffer.alloc(12);
head.write('glTF', 0); head.writeUInt32LE(2, 4); head.writeUInt32LE(12 + 8 + js.length + 8 + bin.length, 8);
const ch0 = Buffer.alloc(8); ch0.writeUInt32LE(js.length, 0); ch0.writeUInt32LE(0x4e4f534a, 4);
const ch1 = Buffer.alloc(8); ch1.writeUInt32LE(bin.length, 0); ch1.writeUInt32LE(0x004e4942, 4);
mkdirSync(join(ROOT, 'assets/models'), { recursive: true });
writeFileSync(join(ROOT, 'assets/models/lab.glb'), Buffer.concat([head, ch0, js, ch1, bin]));

// the area boxes, for spawns, objectives and the map
const id = (s) => s.toUpperCase().replace(/[^A-Z0-9]+/g, '_').replace(/^_|_$/g, '');
let h = '/* generated by tools/prep-lab.mjs from the HSL-108 model: do not edit */\n#ifndef LAB_AREAS_H\n#define LAB_AREAS_H\n';
h += `#define LAB_SCALE ${SCALE}.0f\n#define LAB_AREA_N ${areas.size}\n`;
h += 'typedef struct { const char *name; float min[3], max[3]; } lab_area_t;\n';
h += 'static const lab_area_t LAB_AREAS[LAB_AREA_N] = {\n';
const names = [...areas.keys()].sort();
for (const a of names) { const b = areas.get(a); h += `  {"${a}", {${b.slice(0, 3).map((v) => v.toFixed(2) + 'f').join(', ')}}, {${b.slice(3).map((v) => v.toFixed(2) + 'f').join(', ')}}},\n`; }
h += '};\nenum {\n';
names.forEach((a, i) => { h += `  AREA_${id(a)} = ${i},\n`; });
h += '};\n';
// the basement and its hatch (lab-fixes.mjs), for the missions
landmarks.set('HATCH', [ftBox([111.25, 1.8, -0.5, 113.0, 6.6, 0.06])]);
landmarks.set('BASEMENT', [ftBox([103.0, 0.3, -8.0, 123.0, 20.3, -0.49])]);
landmarks.set('CLOSET_DOORS', [ftBox([110.6, 1.6, 0, 111.0, 6.8, 6.9])]);
// the printers that were boxed are landmarks too: the print farm's four
landmarks.set('PRINTER', [...printers.values()]);
const f2 = (b) => `{${b.slice(0, 3).map((v) => v.toFixed(2) + 'f').join(', ')}}, {${b.slice(3).map((v) => v.toFixed(2) + 'f').join(', ')}}`;
h += 'typedef struct { float min[3], max[3]; } lab_box_t;\n';
for (const [k, list] of landmarks) {
  h += `#define LM_${k}_N ${list.length}\nstatic const lab_box_t LM_${k}[${Math.max(1, list.length)}] = {${list.length ? list.map((b) => `{${f2(b)}}`).join(', ') : '{{0}}'}};\n`;
}
h += '#endif\n';
mkdirSync(join(ROOT, 'src/gen'), { recursive: true });
writeFileSync(join(ROOT, 'src/gen/lab_areas.h'), h);

let tris = 0, verts = 0;
for (const ch of chunks.values()) { tris += ch.idx.length / 3; verts += ch.nv; }
console.log(`parts kept ${kept}, printer parts dropped ${dropped} (${printers.size} printers boxed); ${chunks.size} chunks, ${verts} verts, ${tris} tris, lab.glb ${(off / 1e6).toFixed(2)} MB`);
