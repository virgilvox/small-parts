/* world.c: the lab mesh (assets/models/lab.glb, made by tools/prep-lab.mjs)
 * and collision against its triangles. Every triangle of the model goes in a
 * grid of columns over the floor plan; spheres push out of the triangles in
 * the columns they touch, rays walk the columns they cross. */
#include "world.h"
#include <stdlib.h>
#include <string.h>

#define CELL 6.f
#define GX ((int)((WORLD_MAX_X + 12) / CELL) + 1)
#define GZ ((int)((WORLD_MAX_Z + 12) / CELL) + 1)
#define GX0 (-6.f)
#define GZ0 (-6.f)

typedef struct { v3 a, b, c, n; float ymin, ymax; } tri_t;

static t3_object *lab;
static tri_t *tris; static int ntri;
static int *cell_start, *cell_list;   /* CSR: cell i holds cell_list[cell_start[i] .. cell_start[i+1]) */
static uint32_t *stamp, stamp_now;

/* ── loading ── */
typedef struct { int n, cap; } grow_t;

static void count_tris(t3_object *o, void *ctx) {
  if (o->type != T3_MESH) return;
  t3_geometry *g = ((t3_mesh *)o)->geometry;
  int *n = ctx;
  *n += g->index ? g->index->count / 3 : g->attributes[T3_ATTR_POSITION]->count / 3;
}

static void take_tris(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type != T3_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  t3_geometry *g = m->geometry;
  t3_attribute *P = g->attributes[T3_ATTR_POSITION];
  const float *pos = t3_attribute_f32(P);
  int n = g->index ? g->index->count : P->count;
  for (int i = 0; i + 2 < n; i += 3) {
    int k[3];
    for (int j = 0; j < 3; j++) {
      if (!g->index) k[j] = i + j;
      else if (g->index->type == T3_UINT32) k[j] = (int)((uint32_t *)g->index->array)[i + j];
      else k[j] = ((uint16_t *)g->index->array)[i + j];
    }
    tri_t *t = &tris[ntri];
    t->a = v3_(pos[k[0] * 3], pos[k[0] * 3 + 1], pos[k[0] * 3 + 2]);
    t->b = v3_(pos[k[1] * 3], pos[k[1] * 3 + 1], pos[k[1] * 3 + 2]);
    t->c = v3_(pos[k[2] * 3], pos[k[2] * 3 + 1], pos[k[2] * 3 + 2]);
    v3 nn = v3_cross(v3_sub(t->b, t->a), v3_sub(t->c, t->a));
    if (v3_dot(nn, nn) < 1e-10f) continue;   /* degenerate */
    t->n = v3_norm(nn);
    t->ymin = fminf(t->a.y, fminf(t->b.y, t->c.y));
    t->ymax = fmaxf(t->a.y, fmaxf(t->b.y, t->c.y));
    ntri++;
  }
  /* PS1 look: the model is flat shaded by having no normals; keep it cheap */
  for (int i = 0; i < m->material_count; i++) {
    t3_material *M = m->materials[i];
    if (!M) continue;
    M->roughness = 0.9f; M->metalness = 0;  M->version++;
  }
  t3_geometry_compute_bounding_sphere(g);
  o->frustum_culled = true;
  o->receive_shadow = false; o->cast_shadow = false;
}

static inline int cx_of(float x) { int i = (int)floorf((x - GX0) / CELL); return i < 0 ? 0 : i >= GX ? GX - 1 : i; }
static inline int cz_of(float z) { int i = (int)floorf((z - GZ0) / CELL); return i < 0 ? 0 : i >= GZ ? GZ - 1 : i; }

static void build_grid(void) {
  cell_start = calloc((size_t)GX * GZ + 1, sizeof *cell_start);
  for (int pass = 0; pass < 2; pass++) {
    int *fill = pass ? calloc((size_t)GX * GZ, sizeof *fill) : NULL;
    for (int i = 0; i < ntri; i++) {
      tri_t *t = &tris[i];
      int x0 = cx_of(fminf(t->a.x, fminf(t->b.x, t->c.x))), x1 = cx_of(fmaxf(t->a.x, fmaxf(t->b.x, t->c.x)));
      int z0 = cz_of(fminf(t->a.z, fminf(t->b.z, t->c.z))), z1 = cz_of(fmaxf(t->a.z, fmaxf(t->b.z, t->c.z)));
      for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
          int c = z * GX + x;
          if (!pass) cell_start[c + 1]++;
          else cell_list[cell_start[c] + fill[c]++] = i;
        }
    }
    if (!pass) {
      for (int c = 0; c < GX * GZ; c++) cell_start[c + 1] += cell_start[c];
      cell_list = malloc((size_t)cell_start[GX * GZ] * sizeof *cell_list);
    }
    free(fill);
  }
  stamp = calloc((size_t)ntri, sizeof *stamp);
}

/* the model is open topped: a ceiling over the main room (and higher over the
 * attic end) so the room reads as a room and a jetpack cannot leave it, with
 * a grid of fluorescent troffers. Built in world space so take_tris can use it. */
static t3_mesh *quads(const float *q, int nq, t3_material *m) {
  float *p = malloc((size_t)nq * 12 * sizeof *p);
  uint16_t *ix = malloc((size_t)nq * 6 * sizeof *ix);
  for (int i = 0; i < nq; i++) {
    memcpy(p + i * 12, q + i * 12, 12 * sizeof *p);
    uint16_t b = (uint16_t)(i * 4);
    uint16_t f[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
    memcpy(ix + i * 6, f, sizeof f);
  }
  t3_geometry *g = t3_geometry_new();
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, p, nq * 4, 3));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT16, ix, nq * 6, 1));
  free(p); free(ix);
  m->side = T3_DOUBLE_SIDE;
  return t3_mesh_new(g, m);
}
#define QH(x0, z0, x1, z1, y) x0, y, z0, x1, y, z0, x1, y, z1, x0, y, z1
static t3_object *build_ceiling(void) {
  t3_object *root = t3_object_new();
  const float SPLIT = 625, LOW = 61, HIGH = 90;
  float c[] = {
    QH(-2.f, -2.f, SPLIT, 190.f, LOW),
    QH(SPLIT, -2.f, 812.f, 190.f, HIGH),
    SPLIT, LOW, -2.f, SPLIT, HIGH, -2.f, SPLIT, HIGH, 190.f, SPLIT, LOW, 190.f,
  };
  t3_material *cm = t3_mesh_standard_material_new(0x6b6e74);
  cm->emissive = t3_color_hex(0x2e3036); cm->emissive_intensity = 1;   /* lit from below only: keep it from reading as a void */
  cm->roughness = 1; cm->metalness = 0; cm->flat_shading = true;   /* no normals, like the model */
  t3_object_add(root, quads(c, 3, cm));
  float L[96 * 12]; int n = 0;
  for (float x = 30; x < SPLIT - 10 && n < 96; x += 48)
    for (int k = 0; k < 3 && n < 96; k++) {
      float z = 40 + k * 54;
      float q[12] = {QH(x - 6, z - 12, x + 6, z + 12, LOW - 0.15f)};
      memcpy(L + n * 12, q, sizeof q); n++;
    }
  t3_material *lm = t3_mesh_basic_material_new(0xf4f6ff);
  t3_object_add(root, quads(L, n, lm));
  return root;
}

static void build_leds(t3_scene *scene);   /* the LED cube, below */

void world_init(t3_scene *scene) {
  size_t n; uint8_t *b = ds_asset("lab.glb", &n);
  if (!b) { ds_log("world: lab.glb missing"); return; }
  t3_gltf *g = t3_gltf_parse(b, n, NULL, NULL);
  free(b);
  if (!g) { ds_log("world: bad lab.glb"); return; }
  lab = g->scene;
  t3_object_add(lab, build_ceiling());
  int count = 0;
  t3_object_traverse(lab, count_tris, &count);
  tris = malloc((size_t)count * sizeof *tris);
  t3_object_traverse(lab, take_tris, NULL);
  build_grid();
  t3_object_add(&scene->base, lab);
  build_leds(scene);
  ds_log("world: lab %d tris, %d cell refs", ntri, cell_start[GX * GZ]);
}

void world_debug_counts(int *t, int *c) { *t = ntri; *c = cell_start ? cell_start[GX * GZ] : 0; }

/* ── queries ── */
static v3 closest_on_tri(v3 p, const tri_t *t) {
  /* Ericson, Real-Time Collision Detection 5.1.5 */
  v3 a = t->a, b = t->b, c = t->c;
  v3 ab = v3_sub(b, a), ac = v3_sub(c, a), ap = v3_sub(p, a);
  float d1 = v3_dot(ab, ap), d2 = v3_dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return a;
  v3 bp = v3_sub(p, b);
  float d3 = v3_dot(ab, bp), d4 = v3_dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return b;
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return v3_add(a, v3_scale(ab, d1 / (d1 - d3)));
  v3 cp = v3_sub(p, c);
  float d5 = v3_dot(ab, cp), d6 = v3_dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return c;
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return v3_add(a, v3_scale(ac, d2 / (d2 - d6)));
  float va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return v3_add(b, v3_scale(v3_sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
  float den = 1 / (va + vb + vc);
  return v3_add(a, v3_add(v3_scale(ab, vb * den), v3_scale(ac, vc * den)));
}

bool world_push_sphere(v3 *c, float r, v3 *normal) {
  if (!tris) return false;
  bool hit = false;
  v3 acc = v3_(0, 0, 0);
  if (++stamp_now == 0) { memset(stamp, 0, (size_t)ntri * sizeof *stamp); stamp_now = 1; }
  int x0 = cx_of(c->x - r), x1 = cx_of(c->x + r), z0 = cz_of(c->z - r), z1 = cz_of(c->z + r);
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      int cell = z * GX + x;
      for (int k = cell_start[cell]; k < cell_start[cell + 1]; k++) {
        int i = cell_list[k];
        if (stamp[i] == stamp_now) continue;
        stamp[i] = stamp_now;
        const tri_t *t = &tris[i];
        if (c->y + r < t->ymin || c->y - r > t->ymax) continue;
        v3 q = closest_on_tri(*c, t);
        v3 d = v3_sub(*c, q);
        float l2 = v3_dot(d, d);
        if (l2 >= r * r) continue;
        float l = sqrtf(l2);
        /* push away from the triangle; a centre on the plane goes out along its normal */
        v3 dir = l > 1e-5f ? v3_scale(d, 1 / l) : t->n;
        if (l <= 1e-5f && v3_dot(dir, v3_sub(*c, t->a)) < 0) dir = v3_scale(dir, -1);
        *c = v3_add(*c, v3_scale(dir, r - l));
        acc = v3_add(acc, dir);
        hit = true;
      }
    }
  if (normal) *normal = hit ? v3_norm(acc) : v3_(0, 1, 0);
  return hit;
}

/* Moller-Trumbore, both faces (the model is double sided) */
static bool ray_tri(v3 o, v3 d, const tri_t *t, float *out) {
  v3 e1 = v3_sub(t->b, t->a), e2 = v3_sub(t->c, t->a);
  v3 p = v3_cross(d, e2);
  float det = v3_dot(e1, p);
  if (fabsf(det) < 1e-9f) return false;
  float inv = 1 / det;
  v3 s = v3_sub(o, t->a);
  float u = v3_dot(s, p) * inv;
  if (u < 0 || u > 1) return false;
  v3 q = v3_cross(s, e1);
  float v = v3_dot(d, q) * inv;
  if (v < 0 || u + v > 1) return false;
  float tt = v3_dot(e2, q) * inv;
  if (tt < 0) return false;
  *out = tt;
  return true;
}

bool world_raycast(v3 o, v3 d, float max_dist, float *t_out, v3 *normal) {
  if (!tris) return false;
  d = v3_norm(d);
  if (++stamp_now == 0) { memset(stamp, 0, (size_t)ntri * sizeof *stamp); stamp_now = 1; }
  float best = max_dist; int best_i = -1;
  /* a 2D DDA over the columns */
  int x = cx_of(o.x), z = cz_of(o.z);
  int sx = d.x > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
  float tdx = fabsf(d.x) > 1e-9f ? CELL / fabsf(d.x) : 1e30f, tdz = fabsf(d.z) > 1e-9f ? CELL / fabsf(d.z) : 1e30f;
  float bx = GX0 + (x + (sx > 0)) * CELL, bz = GZ0 + (z + (sz > 0)) * CELL;
  float tmx = fabsf(d.x) > 1e-9f ? (bx - o.x) / d.x : 1e30f, tmz = fabsf(d.z) > 1e-9f ? (bz - o.z) / d.z : 1e30f;
  float t_cell = 0;
  for (int guard = 0; guard < 4096; guard++) {
    int cell = z * GX + x;
    for (int k = cell_start[cell]; k < cell_start[cell + 1]; k++) {
      int i = cell_list[k];
      if (stamp[i] == stamp_now) continue;
      stamp[i] = stamp_now;
      float tt;
      if (ray_tri(o, d, &tris[i], &tt) && tt < best) { best = tt; best_i = i; }
    }
    float t_next = fminf(tmx, tmz);
    if (best_i >= 0 && best <= t_next) break;
    if (t_next > max_dist) break;
    t_cell = t_next;
    if (tmx < tmz) { x += sx; tmx += tdx; if (x < 0 || x >= GX) break; }
    else { z += sz; tmz += tdz; if (z < 0 || z >= GZ) break; }
  }
  (void)t_cell;
  if (best_i < 0) return false;
  *t_out = best;
  if (normal) { v3 n = tris[best_i].n; *normal = v3_dot(n, d) > 0 ? v3_scale(n, -1) : n; }
  return true;
}

bool world_visible(v3 a, v3 b) {
  v3 d = v3_sub(b, a);
  float l = v3_len(d), t;
  if (l < 1e-4f) return true;
  return !world_raycast(a, v3_scale(d, 1 / l), l - 0.05f, &t, NULL);
}

float world_floor_y(v3 p) {
  float t; v3 n;
  if (world_raycast(v3_(p.x, p.y + 0.01f, p.z), v3_(0, -1, 0), 200, &t, &n)) return p.y + 0.01f - t;
  return -1e9f;
}

/* the highest walkable surface under a disc of radius r around p, no higher than p.y + up */
static float ground_under(v3 p, float r, float up, float down) {
  static const float OFF[5][2] = {{0, 0}, {0.6f, 0}, {-0.6f, 0}, {0, 0.6f}, {0, -0.6f}};
  float best = -1e9f;
  for (int i = 0; i < 5; i++) {
    v3 o = v3_(p.x + OFF[i][0] * r, p.y + up, p.z + OFF[i][1] * r);
    float t; v3 n;
    if (world_raycast(o, v3_(0, -1, 0), up + down, &t, &n) && n.y > 0.55f) best = fmaxf(best, o.y - t);
  }
  return best;
}

void world_move(v3 *p, v3 *vel, float r, float h, float dt, bool *grounded) {
  bool was = *grounded;
  *grounded = false;
  v3 step = v3_scale(*vel, dt);
  float len = v3_len(step);
  int n = (int)ceilf(len / (r * 0.5f));
  if (n < 1) n = 1;
  if (n > 24) n = 24;
  v3 s = v3_scale(step, 1.f / n);
  for (int k = 0; k < n; k++) {
    p->x += s.x; p->z += s.z; p->y += s.y;
    /* the body: spheres from just above a step to the head */
    float ys[3] = {WORLD_STEP + r, h * 0.5f, h - r};
    for (int it = 0; it < 2; it++)
      for (int j = 0; j < 3; j++) {
        if (ys[j] < r) continue;
        v3 c = v3_(p->x, p->y + ys[j], p->z), nn;
        if (world_push_sphere(&c, r, &nn)) {
          v3 push = v3_(c.x - p->x, 0, c.z - p->z);
          p->x = c.x; p->z = c.z;
          (void)push;
          /* ceilings: the head sphere pushed down while rising */
          if (j == 2 && nn.y < -0.5f && vel->y > 0) { vel->y = 0; s.y = fminf(s.y, 0); }
          /* cancel the velocity into what was hit */
          v3 hn = v3_norm(v3_(nn.x, 0, nn.z));
          float into = vel->x * hn.x + vel->z * hn.z;
          if (into < 0 && fabsf(nn.y) < 0.7f) { vel->x -= hn.x * into; vel->z -= hn.z * into; }
        }
      }
    /* the floor: land, or step up onto anything below WORLD_STEP */
    float g = ground_under(*p, r * 0.8f, WORLD_STEP + 0.05f, (was || *grounded) ? WORLD_STEP : fmaxf(0.05f, -s.y + 0.05f));
    if (g > -1e8f && vel->y <= 0.01f && p->y <= g + 0.02f + ((was || *grounded) ? WORLD_STEP : 0)) {
      p->y = g; vel->y = 0; *grounded = true;
    }
  }
  if (p->y < -80) { p->y = -80; vel->y = 0; }   /* under the basement floor */
}

/* ── areas ── */
v3 world_area_center(int a) {
  if (a < 0 || a >= LAB_AREA_N) return v3_(400, 0, 94);
  const lab_area_t *A = &LAB_AREAS[a];
  v3 c = v3_((A->min[0] + A->max[0]) * 0.5f, 0, (A->min[2] + A->max[2]) * 0.5f);
  float f = world_floor_y(v3_(c.x, 2, c.z));
  c.y = f > -1e8f ? f : 0;
  return c;
}

int world_area_at(v3 p) {
  int best = -1; float bv = 1e30f;
  for (int i = 0; i < LAB_AREA_N; i++) {
    const lab_area_t *A = &LAB_AREAS[i];
    if (p.x < A->min[0] || p.x > A->max[0] || p.z < A->min[2] || p.z > A->max[2]) continue;
    float v = (A->max[0] - A->min[0]) * (A->max[2] - A->min[2]);
    if (v < bv && strcmp(A->name, "envelope") && strcmp(A->name, "walls and signage") && strcmp(A->name, "floor")) { bv = v; best = i; }
  }
  return best;
}

const char *world_area_name(int a) { return a >= 0 && a < LAB_AREA_N ? LAB_AREAS[a].name : ""; }

/* ── the LED cube on the robot counter ──
 * The model's lights sat on top of the case (prep-lab drops them); here a 6 x 6 x 6
 * grid of lit LEDs stands inside the glass, one instanced mesh, running through a
 * few classic cube patterns: a rainbow wave, rain, a pulse from the centre, a
 * sweeping plane. */
#define LED_N 6
static t3_instanced_mesh *leds;
static float led_t;

static void build_leds(t3_scene *scene) {
  const lab_box_t *C = &LM_LED_CUBE[0];
  float inset = 0.45f, cell = (fminf(C->max[0] - C->min[0], C->max[2] - C->min[2]) - 2 * inset) / (LED_N - 1);
  float ch = (C->max[1] - C->min[1] - 2 * inset) / (LED_N - 1);
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  leds = t3_instanced_mesh_new(t3_box_geometry_new(0.16f, 0.16f, 0.16f, 1, 1, 1), m, LED_N * LED_N * LED_N);
  for (int i = 0; i < LED_N * LED_N * LED_N; i++) {
    int x = i % LED_N, y = (i / LED_N) % LED_N, z = i / (LED_N * LED_N);
    t3_mat4 M;
    t3_mat4_compose(&M, t3_v3(C->min[0] + inset + x * cell, C->min[1] + inset + y * ch, C->min[2] + inset + z * cell), t3_quat_identity(), t3_v3(1, 1, 1));
    t3_instanced_mesh_set_matrix_at(leds, i, &M);
    t3_instanced_mesh_set_color_at(leds, i, t3_color_hex(0x101420));
  }
  ((t3_object *)leds)->frustum_culled = false;
  t3_object_add(&scene->base, leds);
}

static t3_color hue(float h, float v) {
  h -= floorf(h);
  float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
  t3_color c = {ds_clamp(r, 0, 1) * v, ds_clamp(g, 0, 1) * v, ds_clamp(b, 0, 1) * v};
  return c;
}

void world_update(float dt, v3 player) {
  (void)player;
  if (!leds) return;
  led_t += dt;
  int pattern = (int)(led_t / 7.f) % 4;
  float t = led_t;
  for (int i = 0; i < LED_N * LED_N * LED_N; i++) {
    int x = i % LED_N, y = (i / LED_N) % LED_N, z = i / (LED_N * LED_N);
    float v = 0, h = 0.6f;
    switch (pattern) {
      case 0: v = 0.9f; h = (x + y + z) / 15.f - t * 0.25f; break;                       /* rainbow wave */
      case 1: {                                                                            /* rain */
        float drop = fmodf(t * 3.f + (float)((x * 7 + z * 13) % 11) * 0.53f, 7.f);
        v = (LED_N - 1 - y) == (int)drop ? 1.f : (LED_N - 1 - y) == (int)drop - 1 ? 0.35f : 0;
        h = 0.55f; break; }
      case 2: {                                                                            /* a pulse from the centre */
        float d = sqrtf((x - 2.5f) * (x - 2.5f) + (y - 2.5f) * (y - 2.5f) + (z - 2.5f) * (z - 2.5f));
        float r = fmodf(t * 2.2f, 5.f);
        v = fmaxf(0, 1 - fabsf(d - r) * 1.4f); h = 0.08f + d * 0.04f; break; }
      default: {                                                                           /* a plane sweeping back and forth */
        float p = (sinf(t * 1.6f) * 0.5f + 0.5f) * (LED_N - 1);
        v = fmaxf(0, 1 - fabsf(x - p) * 1.2f); h = 0.3f + y * 0.05f; break; }
    }
    t3_color c = hue(h, v);
    c.r = fmaxf(c.r, 0.05f); c.g = fmaxf(c.g, 0.06f); c.b = fmaxf(c.b, 0.09f);   /* an unlit LED still shows */
    t3_instanced_mesh_set_color_at(leds, i, c);
  }
  if (leds->instance_color) leds->instance_color->version++;
}
