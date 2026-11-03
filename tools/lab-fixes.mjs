// Gameplay changes to the HSL-108 model, applied by prep-lab.mjs while it
// bakes the lab. Everything here is in the model's feet (x from the storefront
// toward the back, y across from the printer wall, z up); prep-lab converts.
//
// Revision 22 of the model has the room right: the member storage U, the
// supply closet's double doors, the hatch plate over the steel framed hole in
// the closet floor and the basement under it. A nine centimetre hero still
// needs a way in, so the game props one closet door open and leaves the plate
// shoved aside, as if someone small had been through already.
//
//   MOVES   parts that stand somewhere else in the game: a translation (dx, dy)
//           or a swing about a hinge (a plan rotation in degrees)
//   CARVES  openings cut into parts: the hole through the floor slabs
//   extras  geometry the model lacks: a way back out of the basement
export const FT = 0.3048;

// the hole under the hatch, and where the plate leaves its gap (the hole's near end, y 3.1 to 4.8;
// any further and the plate runs into the closet's south wall)
export const HOLE = { x: [108.0, 110.6], y: [3.1, 6.5] };
const PLATE_SLIDE = 1.7;

export const MOVES = [
  // the plate and everything on it (treads, lift rings): slid along the hole toward the closet's back wall
  { why: 'hatch plate shoved aside', match: (name, group) => group === 'hatch', dx: 0, dy: PLATE_SLIDE },
  // door B (the leaf by the sign) swung open into the biolab about its hinge at the jamb
  { why: 'closet door B propped open', match: (name, group, c) => group === 'doors' && (name === 'Supply closet door B closed leaf' ||
      (name === 'Supply closet door handle' && c[1] > 4.2)), hinge: [110.85, 6.7], deg: 80 },
];

export function moveFor(name, group, c) {
  for (const m of MOVES) if (m.match(name, group, c)) return m;
  return null;
}

// apply a move to one point in feet (x, y): a translation or a swing about the hinge
export function movePoint(m, x, y) {
  if (!m.hinge) return [x + m.dx, y + m.dy];
  const a = (m.deg * Math.PI) / 180, dx = x - m.hinge[0], dy = y - m.hinge[1];
  return [m.hinge[0] + dx * Math.cos(a) - dy * Math.sin(a), m.hinge[1] + dx * Math.sin(a) + dy * Math.cos(a)];
}

// the model frames the hole but leaves the floor slabs whole over it (its viewer hides the
// slabs to look into the basement): cut the hole through every slab, inside the frame
export function carveFor(name, group, mn, mx) {
  const over = (a0, a1, b0, b1) => a1 > b0 && a0 < b1;
  if (group === 'slabs' && over(mn[0], mx[0], HOLE.x[0], HOLE.x[1]) && over(mn[1], mx[1], HOLE.y[0], HOLE.y[1]))
    return [[HOLE.x[0], HOLE.y[0], -5, HOLE.x[1], HOLE.y[1], 5]];
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

// the way back out: the dirt is banked low under the hatch, about 2.2 m down, more than a
// jetpack can lift, so an old steel shelving unit lies dumped under the plate beside the gap.
// Its shelves are a nine centimetre climb each; the top one sits just under the slab, its front
// edge under the opening, so a jump and a burst of jetpack from there comes up through the gap.
export function extras({ box }) {
  const STEEL = [92, 98, 106], SHELF = [118, 124, 132];
  const x0 = 108.2, x1 = 110.4, y0 = 4.3, y1 = 6.4, floor = -7.71;   /* its front reaches under the gap */
  for (const [x, y] of [[x0, y0], [x1 - 0.12, y0], [x0, y1 - 0.12], [x1 - 0.12, y1 - 0.12]]) box([x, y, floor, x + 0.12, y + 0.12, -0.62], STEEL);
  for (const z of [-7.2, -5.4, -3.6, -1.75]) box([x0, y0, z - 0.08, x1, y1, z], SHELF);
  box([x0 + 0.3, y0 + 0.3, -3.6, x0 + 1.1, y0 + 1.0, -3.0], [150, 110, 70]);   /* a carton left on it */
}
