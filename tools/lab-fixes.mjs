// Corrections to the HSL-108 model from people who know the room, applied by
// prep-lab.mjs while it bakes the lab. Everything here is in the model's
// feet (x from the storefront toward the back, y across from the printer wall,
// z up); prep-lab converts.
//
//   MOVES   furniture that stands somewhere else: every non-structure part whose
//           centre lies in the footprint moves by the offset, contents and all
//   CARVES  openings cut into box-shaped parts (a doorway through a wall, the
//           basement hatch through the floor)
//   extras  what the model does not have: the supply closet's doors, the
//           basement hatch plate, and the basement under it
export const FT = 0.3048;

export const MOVES = [
  // member storage is a U opening to the aisle: the lockers stand out from the
  // wall and meet the wire bins, closing the U's arm
  { why: 'lockers out to the wire bins', x: [62.85, 65.4], y: [0.45, 6.75], dx: 0, dy: 4.85 },
  // the lost and found shelves they meet take the lockers' old place by the wall
  { why: 'lost and found into the lockers’ old place', x: [62.25, 64.45], y: [8.55, 11.55], dx: 0.75, dy: -8.1 },
];

// the supply closet's double doors open east into the biolab at x 110.6 to 111.0, y 1.6 to 6.8
const DOOR = { x: [110.6, 111.0], y: [1.6, 6.8], h: 6.9 };
// the hatch: a hole through the floor just outside the doors, the steel plate
// over it pushed aside far enough for something small to drop through
export const HOLE = { x: [111.25, 113.0], y: [1.8, 6.6] };
const PLATE = { x: [111.15, 113.1], y: [3.0, 8.0] };
// the basement: about 400 square feet under the closet and the biolab, no stairs
const BASE = { x: [103.0, 123.0], y: [0.3, 20.3], floor: -8.0 };

export function moveFor(name, group, c) {
  if (group === 'structure' || group === 'slabs' || group === 'upper') return null;
  for (const m of MOVES) if (c[0] >= m.x[0] && c[0] <= m.x[1] && c[1] >= m.y[0] && c[1] <= m.y[1]) return m;
  return null;
}

// boxes (feet: [x0, y0, z0, x1, y1, z1]) to cut out of a part, or null
export function carveFor(name, group, mn, mx) {
  const over = (a0, a1, b0, b1) => a1 > b0 && a0 < b1;
  if (group === 'slabs' && over(mn[0], mx[0], HOLE.x[0], HOLE.x[1]) && over(mn[1], mx[1], HOLE.y[0], HOLE.y[1]))
    return [[HOLE.x[0], HOLE.y[0], -5, HOLE.x[1], HOLE.y[1], 5]];
  // the biolab's block wall runs across the closet doorway: open it to door height
  if (name === 'Biolab white block wall' && over(mn[1], mx[1], DOOR.y[0], DOOR.y[1]) && mn[0] < DOOR.x[1] + 0.4)
    return [[mn[0] - 1, DOOR.y[0], -1, mx[0] + 1, DOOR.y[1], 7.0]];
  return null;
}

// a box minus a box: up to six boxes covering what is left
export function subtract(b, h) {
  const out = [];
  const [x0, y0, z0, x1, y1, z1] = b;
  const hx0 = Math.max(x0, h[0]), hy0 = Math.max(y0, h[1]), hz0 = Math.max(z0, h[2]);
  const hx1 = Math.min(x1, h[3]), hy1 = Math.min(y1, h[4]), hz1 = Math.min(z1, h[5]);
  if (hx0 >= hx1 || hy0 >= hy1 || hz0 >= hz1) return [b];
  if (x0 < hx0) out.push([x0, y0, z0, hx0, y1, z1]);
  if (hx1 < x1) out.push([hx1, y0, z0, x1, y1, z1]);
  if (y0 < hy0) out.push([hx0, y0, z0, hx1, hy0, z1]);
  if (hy1 < y1) out.push([hx0, hy1, z0, hx1, y1, z1]);
  if (z0 < hz0) out.push([hx0, hy0, z0, hx1, hy1, hz0]);
  if (hz1 < z1) out.push([hx0, hy0, hz1, hx1, hy1, z1]);
  return out;
}

// a deterministic hash for the dirt's lumps
const rnd = (i, j) => { const s = Math.sin(i * 127.1 + j * 311.7) * 43758.5453; return s - Math.floor(s); };

// what the model lacks. box(b, rgb), cyl(a, b, r, rgb) and grid(x0, y0, x1, y1, nx, ny, zf, colf)
// draw in feet; rgb is 0..255 sRGB
export function extras({ box, cyl, grid }) {
  /* the supply closet doors: two painted leaves in the opening, a kick plate, handles and a sign */
  const DOORC = [98, 70, 44], METAL = [150, 156, 162];
  const mid = (DOOR.y[0] + DOOR.y[1]) / 2, xf = DOOR.x[1] - 0.06;
  for (const [y0, y1] of [[DOOR.y[0] + 0.05, mid - 0.02], [mid + 0.02, DOOR.y[1] - 0.05]]) {
    box([DOOR.x[0] + 0.1, y0, 0.02, DOOR.x[1] - 0.1, y1, DOOR.h], DOORC);
    box([xf, y0 + 0.1, 0.05, xf + 0.05, y1 - 0.1, 0.8], METAL);   /* kick plate */
  }
  for (const y of [mid - 0.35, mid + 0.25]) box([xf, y, 3.1, xf + 0.12, y + 0.1, 3.9], [40, 42, 46]);   /* pull handles */
  box([xf, mid - 0.7, 5.3, xf + 0.04, mid + 0.7, 5.9], [235, 235, 228]);   /* the SUPPLY sign */
  box([xf + 0.01, mid - 0.55, 5.5, xf + 0.05, mid + 0.55, 5.7], [30, 32, 36]);
  box([DOOR.x[0], DOOR.y[0] - 0.08, DOOR.h, DOOR.x[1], DOOR.y[1] + 0.08, DOOR.h + 0.12], [80, 82, 86]);   /* the frame head */

  /* the hatch plate: a steel tread plate on the floor, shoved off the hole */
  box([PLATE.x[0], PLATE.y[0], 0.0, PLATE.x[1], PLATE.y[1], 0.06], [128, 132, 138]);
  for (let i = 0; i < 9; i++) for (let j = 0; j < 20; j++) {   /* the raised treads */
    const x = PLATE.x[0] + 0.12 + i * 0.2, y = PLATE.y[0] + 0.12 + j * 0.24 + (i % 2) * 0.12;
    if (y < PLATE.y[1] - 0.1) box([x, y, 0.06, x + 0.1, y + 0.03, 0.085], [160, 164, 170]);
  }
  /* the hole's rough edges and a dark throat down to the basement */
  const T = 0.49;
  box([HOLE.x[0] - 0.05, HOLE.y[0] - 0.05, -T, HOLE.x[0], HOLE.y[1] + 0.05, 0], [70, 66, 60]);
  box([HOLE.x[1], HOLE.y[0] - 0.05, -T, HOLE.x[1] + 0.05, HOLE.y[1] + 0.05, 0], [70, 66, 60]);
  box([HOLE.x[0], HOLE.y[0] - 0.05, -T, HOLE.x[1], HOLE.y[0], 0], [70, 66, 60]);
  box([HOLE.x[0], HOLE.y[1], -T, HOLE.x[1], HOLE.y[1] + 0.05, 0], [70, 66, 60]);

  /* the basement: block walls, a concrete floor, the slab's underside */
  const { x: [bx0, bx1], y: [by0, by1], floor } = BASE, W = 0.5, CON = [118, 116, 110], UNDER = [92, 90, 86];
  box([bx0 - W, by0, floor, bx0, by1, -T], CON);
  box([bx1, by0, floor, bx1 + W, by1, -T], CON);
  box([bx0 - W, by0 - W, floor, bx1 + W, by0, -T], CON);
  box([bx0 - W, by1, floor, bx1 + W, by1 + W, -T], CON);
  box([bx0, by0, floor - 0.3, bx1, by1, floor], [96, 94, 90]);
  /* the dirt: deepest under the hatch, where it was shovelled in, thinning to bare floor in one corner */
  const cx = (HOLE.x[0] + HOLE.x[1]) / 2, cy = (HOLE.y[0] + HOLE.y[1]) / 2;
  grid(bx0, by0, bx1, by1, 28, 28, (x, y) => {
    const d = Math.hypot(x - cx, y - cy), far = Math.hypot(x - bx1, y - by1);
    const pile = 3.6 * Math.exp(-(d * d) / 40) + 1.6 * Math.exp(-((x - 108) ** 2 + (y - 15) ** 2) / 18);
    const lump = 0.35 * rnd(Math.round(x * 2), Math.round(y * 2));
    return floor + Math.max(0, 1.2 + pile + lump - (far < 7 ? (7 - far) * 0.35 : 0));
  }, (x, y) => { const k = rnd(Math.round(x * 3), Math.round(y * 3)); return [96 + k * 26, 72 + k * 18, 48 + k * 12]; });
  /* the plumbing, hung under the slab: a cast iron drain from the restrooms, a PVC
   * vent and branch, copper supply lines, a cleanout dropping into the dirt */
  const IRON = [58, 60, 66], PVC = [228, 228, 222], CU = [184, 115, 60];
  cyl([bx0, 9.0, -1.3], [bx1, 9.0, -1.3], 0.2, IRON);
  cyl([bx0 + 0.6, 9.0, -1.3], [bx0 + 0.6, 9.0, -T], 0.2, IRON);              /* up to the restrooms */
  cyl([118.0, 9.0, -1.3], [118.0, 9.0, floor + 2.0], 0.18, IRON);            /* the cleanout */
  box([117.75, 8.75, floor + 1.8, 118.25, 9.25, floor + 2.05], [80, 82, 88]);
  for (const x of [109.0, 116.5, 121.0]) cyl([x, by0 + 0.5, -1.0], [x, by1 - 0.5, -1.0], 0.1, PVC);
  cyl([116.5, 4.0, -1.0], [116.5, 4.0, -T], 0.1, PVC);                       /* up to the biolab sink */
  for (const y of [2.2, 2.6]) cyl([bx0 + 0.3, y, -0.85], [bx1 - 0.3, y, -0.85], 0.04, CU);
  for (let x = bx0 + 2; x < bx1; x += 4) box([x, 8.6, -1.0, x + 0.12, 9.4, -T], [70, 72, 78]);   /* hanger straps */
}
