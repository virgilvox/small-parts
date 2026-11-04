/* weapon.c: see weapon.h. The four weapons (docs/DESIGN.md), the player's
 * shots in flight, glue puddles and thrown LiPos.
 *
 * Every shot leaves the muzzle toward the point the aim ray finds (the lab,
 * a printer or an army man along the camera's ray), so it lands where the
 * reticle is even from the shoulder. Shots in flight test the lab
 * (world_raycast), the mission's objectives (mission_raycast, mission_hit:
 * the print farm) and the army (enemy_raycast) a step at a time. */
#include "weapon.h"
#include "enemy.h"
#include "mission.h"
#include "world.h"
#include "fx.h"
#include "audio.h"
#include "gen/lab_areas.h"
#include <string.h>

/* enemy.c extras the pierce and the veterans' sidestep need (not in enemy.h) */

#define BAND_DMG 14.f
#define BAND_GAP 0.2f          /* five a second */
#define BAND_SPEED 70.f
#define BAND_GRAV 6.f          /* a little drop */
#define GLUE_DMG 10.f
#define GLUE_GAP (1 / 3.f)
#define GLUE_SPEED 26.f
#define GLUE_GRAV 18.f
#define GLUE_SLOW 0.4f
#define PUDDLE_T 6.f
#define PUDDLE_R 1.3f
#define COIL_T 0.8f            /* full charge */
#define COIL_LO 40.f
#define COIL_HI 120.f
#define COIL_PIERCE 3
#define IRON_DMG 55.f
#define IRON_REACH 2.f
#define PUNCH_DMG 12.f         /* no iron yet: a shove */
#define LIPO_DMG 60.f
#define LIPO_R 4.f
#define LIPO_PUFF 1.2f
#define AIM_MAX 400.f
#define GRAV_LIPO 24.f

#define MAXS 48
#define MAXPUD 12
#define MAXL 4

static const char *const NAME[WP_COUNT] = {"RUBBER BAND REPEATER", "HOT GLUE GUN", "COILGUN", "SOLDERING IRON"};
static const int AMMO_START[WP_COUNT] = {-1, 40, 10, -1};
static const int AMMO_CAP[WP_COUNT] = {-1, 80, 20, -1};

enum { SH_BAND, SH_GLUE };
typedef struct { bool on; int kind; v3 p, v; float life; int owner; } shot_t;
typedef struct { bool on; v3 p; float r, life; } puddle_t;
typedef struct { bool on, touched, puffing; v3 p, v; float t, fuse, spin; t3_mesh *mesh; int owner; } lipo_t;

static t3_scene *scn;
/* one player's kit: what they carry and what their hands are doing. Shots,
 * puddles and LiPos in flight are shared pools, each shot and LiPo marked
 * with the player who let it go (whose HUD shows the hit) */
typedef struct {
  bool owned[WP_COUNT];
  int ammo[WP_COUNT], lipos, cur;
  float cd, swing_t, swing_cd, charge, throw_cd;
  bool swing_hit, charging;
} kit_t;
static kit_t KIT[2] = {{.cur = -1}, {.cur = -1}};
static kit_t *K = &KIT[0];
static int kp_id;
static shot_t S[MAXS];
static puddle_t PD[MAXPUD];
static lipo_t L[MAXL];
/* the coilgun's beam lingers a moment: seen from the shoulder it runs almost straight away from the camera */
static struct { v3 a, b; float life, w; } BM[4];
static t3_instanced_mesh *im_glob, *im_puddle;
static float wtime;

static t3_instanced_mesh *inst(t3_geometry *g, int cap) {
  t3_material *m = t3_mesh_standard_material_new(0xffffff);
  m->roughness = 0.35f; m->metalness = 0; m->version++;
  t3_instanced_mesh *im = t3_instanced_mesh_new(g, m, cap);
  t3_instanced_mesh_set_color_at(im, 0, t3_color_hex(C_GOO));
  t3_object *o = (t3_object *)im;
  o->frustum_culled = false; o->cast_shadow = false; o->receive_shadow = false;
  im->count = 0; o->visible = false;
  t3_object_add(&scn->base, im);
  t3_release(g); t3_release(m);
  return im;
}

void weapon_init(t3_scene *scene) {
  scn = scene;
  im_glob = inst(t3_icosahedron_geometry_new(0.5f, 0), MAXS);
  t3_geometry *disc = t3_circle_geometry_new(1, 14);
  t3_geometry_rotate_x(disc, -DS_PI / 2);
  im_puddle = inst(disc, MAXPUD);
  for (int i = 0; i < MAXL; i++) {
    /* a LiPo pack: a blue soft pouch with a yellow band across it */
    t3_material *m = t3_mesh_standard_material_new(0x2f5fa8);
    m->roughness = 0.5f; m->metalness = 0; m->version++;
    t3_geometry *g = t3_box_geometry_new(0.3f, 0.1f, 0.2f, 1, 1, 1);
    L[i].mesh = t3_mesh_new(g, m);
    t3_release(m); t3_release(g);
    t3_material *lm = t3_mesh_standard_material_new(0xe8c43a);
    lm->roughness = 0.5f; lm->metalness = 0; lm->version++;
    t3_geometry *bg = t3_box_geometry_new(0.08f, 0.104f, 0.204f, 1, 1, 1);
    t3_mesh *band = t3_mesh_new(bg, lm);
    t3_release(lm); t3_release(bg);
    t3_object_add((t3_object *)L[i].mesh, band);
    t3_release(band);
    t3_object *o = (t3_object *)L[i].mesh;
    o->frustum_culled = false; o->visible = false; o->rotation.order = T3_YXZ;
    t3_object_add(&scene->base, o);
  }
}

static int first_owned(void) { for (int w = 0; w < WP_COUNT; w++) if (K->owned[w]) return w; return -1; }

void weapon_set_player(int p) { kp_id = p == 1 ? 1 : 0; K = &KIT[kp_id]; }

/* each of these is for both players: the kits start the same and every find and pickup goes to both */
#define BOTH(body) do { kit_t *keep = K; for (int pi_ = 0; pi_ < 2; pi_++) { K = &KIT[pi_]; body; } K = keep; } while (0)

static void reset_kit(int level) {
  memset(K->owned, 0, sizeof K->owned);
  /* the repeater from the start (the prologue hands it over); what the earlier levels found: the glue gun in 2,
   * the coilgun at the end of 4, the iron in 5 */
  unsigned have = mission_weapons_at_start(level);
  for (int w = 0; w < WP_COUNT; w++) K->owned[w] = (have >> w) & 1;
  K->owned[WP_BANDS] = true;
#ifdef SP_TEST_ARMY
  for (int w = 0; w < WP_COUNT; w++) K->owned[w] = true;
#endif
  for (int w = 0; w < WP_COUNT; w++) K->ammo[w] = K->owned[w] ? AMMO_START[w] : (AMMO_START[w] < 0 ? -1 : 0);
  K->lipos = 3;
  K->cur = first_owned();
  K->cd = K->swing_t = K->swing_cd = K->charge = K->throw_cd = 0; K->charging = false;
}
void weapon_reset(int level) { BOTH(reset_kit(level)); }

void weapon_clear(void) {
  memset(S, 0, sizeof S);
  memset(PD, 0, sizeof PD);
  memset(BM, 0, sizeof BM);
  for (int i = 0; i < MAXL; i++) { L[i].on = false; if (L[i].mesh) ((t3_object *)L[i].mesh)->visible = false; }
  if (im_glob) { im_glob->count = 0; ((t3_object *)im_glob)->visible = false; }
  if (im_puddle) { im_puddle->count = 0; ((t3_object *)im_puddle)->visible = false; }
  BOTH(K->charging = false; K->charge = 0; K->swing_t = 0);
}
void weapon_cancel(void) { K->charging = false; K->charge = 0; K->swing_t = 0; }

static void unlock_kit(int w) {
  if (!K->owned[w]) {
    K->owned[w] = true;
    if (AMMO_START[w] >= 0) K->ammo[w] = K->ammo[w] > AMMO_START[w] ? K->ammo[w] : AMMO_START[w];
  }
  K->cur = w; K->charging = false; K->charge = 0;
}
void weapon_unlock(int w) {
  if (w < 0 || w >= WP_COUNT) return;
  BOTH(unlock_kit(w));
}
bool weapon_unlocked(int w) { return w >= 0 && w < WP_COUNT && K->owned[w]; }
int weapon_current(void) { return K->cur; }
int weapon_ammo(int w) { return w >= 0 && w < WP_COUNT ? K->ammo[w] : -1; }
int weapon_lipos(void) { return K->lipos; }
const char *weapon_name(int w) { return w >= 0 && w < WP_COUNT ? NAME[w] : ""; }

static void give_kit(int pickup) {
  switch (pickup) {
  case PK_GLUE: K->ammo[WP_GLUE] = K->ammo[WP_GLUE] < 0 ? 15 : K->ammo[WP_GLUE] + 15; if (K->ammo[WP_GLUE] > AMMO_CAP[WP_GLUE]) K->ammo[WP_GLUE] = AMMO_CAP[WP_GLUE]; break;
  case PK_CELLS: K->ammo[WP_COIL] = K->ammo[WP_COIL] < 0 ? 4 : K->ammo[WP_COIL] + 4; if (K->ammo[WP_COIL] > AMMO_CAP[WP_COIL]) K->ammo[WP_COIL] = AMMO_CAP[WP_COIL]; break;
  case PK_LIPO: if (K->lipos < 5) K->lipos++; break;
  default: break;
  }
}
void weapon_give(int pickup) { BOTH(give_kit(pickup)); }

float weapon_glue_slow(v3 at) {
  for (int i = 0; i < MAXPUD; i++) {
    const puddle_t *p = &PD[i];
    if (!p->on) continue;
    if (at.y < p->p.y - 0.4f || at.y > p->p.y + 0.8f) continue;
    float dx = at.x - p->p.x, dz = at.z - p->p.z;
    if (dx * dx + dz * dz < p->r * p->r) return GLUE_SLOW;
  }
  return 1;
}

/* ── tracing ── */
enum { HIT_NONE, HIT_WORLD, HIT_MISSION, HIT_ENEMY };
typedef struct { int what, id; float t; bool weak; v3 n; } hit_t;

static hit_t trace(v3 o, v3 d, float len) {
  hit_t h = {HIT_NONE, -1, len, false, v3_(0, 1, 0)};
  float t; v3 n;
  if (world_raycast(o, d, len, &t, &n)) { h.what = HIT_WORLD; h.t = t; h.n = n; }
  if (mission_raycast(o, d, h.t, &t) && t < h.t) { h.what = HIT_MISSION; h.t = t; h.n = v3_scale(d, -1); }
  int id; bool weak;
  if (enemy_raycast(o, d, h.t, &id, &t, &weak) && t <= h.t) { h.what = HIT_ENEMY; h.t = t; h.id = id; h.weak = weak; h.n = v3_scale(d, -1); }
  return h;
}

/* where the reticle is: the first thing along the aim ray */
static v3 aim_point(const weapon_in_t *in) {
  v3 d = v3_norm(in->dir);
  hit_t h = trace(in->eye, d, AIM_MAX);
  return v3_add(in->eye, v3_scale(d, h.t));
}

/* a shot lands on something: damage, the hit marker, the sparks */
static void land(hit_t h, v3 at, float dmg, int kind, weapon_out_t *out) {
  if (h.what == HIT_ENEMY) {
    if (enemy_damage(h.id, dmg, at, h.weak, kind)) out->killed = true;
    out->hit = true; out->hit_weak |= h.weak;
  } else if (h.what == HIT_MISSION) {
    if (mission_hit(at, dmg)) out->hit = true;
    fx_sparks(at, h.n, 0xffd36a, 4);
  } else if (h.what == HIT_WORLD && kind == WP_BANDS) fx_sparks(at, h.n, 0xd8503a, 2);
}

static shot_t *shot_new(int kind, v3 p, v3 v) {
  for (int i = 0; i < MAXS; i++) if (!S[i].on) { S[i] = (shot_t){true, kind, p, v, kind == SH_BAND ? 1.6f : 3, kp_id}; return &S[i]; }
  return NULL;
}

/* launch from the muzzle so the arc passes through the aim point */
static v3 launch(v3 from, v3 to, float speed, float grav) {
  v3 d = v3_sub(to, from);
  float L0 = v3_len(d);
  if (L0 < 1e-3f) return v3_(0, 0, speed);
  float tf = L0 / speed;
  v3 v = v3_scale(d, 1 / tf);
  v.y += 0.5f * grav * tf;
  return v;
}

static void puddle_at(v3 p) {
  int best = 0; float least = 1e9f;
  for (int i = 0; i < MAXPUD; i++) {
    if (!PD[i].on) { best = i; least = -1; break; }
    if (PD[i].life < least) { least = PD[i].life; best = i; }
  }
  /* a glob on an old puddle tops it up */
  for (int i = 0; i < MAXPUD; i++) if (PD[i].on && hypotf(PD[i].p.x - p.x, PD[i].p.z - p.z) < 0.8f && fabsf(PD[i].p.y - p.y) < 0.3f) {
    PD[i].life = PUDDLE_T; PD[i].r = fminf(PUDDLE_R * 1.5f, PD[i].r + 0.15f); return;
  }
  PD[best] = (puddle_t){true, v3_(p.x, p.y + 0.03f, p.z), PUDDLE_R, PUDDLE_T};
}

static void shots_update(float dt, weapon_out_t *outs) {
  for (int i = 0; i < MAXS; i++) {
    shot_t *s = &S[i];
    if (!s->on) continue;
    weapon_out_t *out = &outs[s->owner];
    s->life -= dt;
    s->v.y -= (s->kind == SH_BAND ? BAND_GRAV : GLUE_GRAV) * dt;
    v3 step = v3_scale(s->v, dt);
    float sl = v3_len(step);
    if (sl < 1e-5f) continue;
    v3 d = v3_scale(step, 1 / sl);
    hit_t h = trace(s->p, d, sl);
    v3 at = v3_add(s->p, v3_scale(d, h.t));
    if (s->kind == SH_BAND) fx_beam(at, v3_sub(at, v3_scale(d, 1.3f)), 0xd8503a, 0.05f);
    else fx_glow(at, 0.45f, C_GOO);
    if (h.what != HIT_NONE) {
      if (s->kind == SH_BAND) land(h, at, BAND_DMG, WP_BANDS, out);
      else {
        land(h, at, GLUE_DMG, WP_GLUE, out);
        fx_glue(at, 7);
        sfx_at(SFX_GLUE, at.x, at.y, at.z, 0.4f);
        if (h.what == HIT_ENEMY) {
          enemy_slow(h.id, GLUE_SLOW, 2);
          /* the splash catches whoever is beside him */
          int ids[8], n = enemy_in_sphere(at, 1.1f, ids, 8);
          for (int k = 0; k < n; k++) if (ids[k] != h.id) enemy_slow(ids[k], GLUE_SLOW, 1.5f);
          v3 feet = enemy_pos(h.id);
          float fy = world_floor_y(v3_(feet.x, feet.y + 0.3f, feet.z));
          if (fy > -1e8f && feet.y - fy < 0.5f) puddle_at(v3_(feet.x, fy, feet.z));
        } else if (h.what == HIT_WORLD) {
          if (h.n.y > 0.6f) puddle_at(at);
          else {
            /* off a wall: the puddle is on the floor under it */
            float fy = world_floor_y(v3_add(at, v3_scale(h.n, 0.3f)));
            if (fy > -1e8f && at.y - fy < 3) puddle_at(v3_(at.x + h.n.x * 0.4f, fy, at.z + h.n.z * 0.4f));
          }
        }
      }
      s->on = false;
      continue;
    }
    s->p = at;
    if (s->life <= 0) s->on = false;
  }
}

/* ── the coilgun ── */
static void coil_fire(const weapon_in_t *in, v3 target, weapon_out_t *out) {
  float dmg = ds_lerp(COIL_LO, COIL_HI, K->charge);
  v3 o = in->muzzle, d = v3_norm(v3_sub(target, o));
  float wt = AIM_MAX, t; v3 n = v3_scale(d, -1);
  bool hw = world_raycast(o, d, AIM_MAX, &wt, &n);
  if (mission_raycast(o, d, wt, &t) && t < wt) {
    wt = t; hw = true;
    if (mission_hit(v3_add(o, v3_scale(d, t)), dmg)) out->hit = true;
  }
  /* through up to three army men, stopping on the lab */
  int skip[COIL_PIERCE], ns = 0;
  for (int k = 0; k < COIL_PIERCE; k++) {
    int id; bool weak;
    if (!enemy_raycast_skip(o, d, wt, skip, ns, &id, &t, &weak)) break;
    v3 at = v3_add(o, v3_scale(d, t));
    if (enemy_damage(id, weak ? dmg * 2 : dmg, at, weak, WP_COIL)) out->killed = true;
    out->hit = true; out->hit_weak |= weak;
    fx_sparks(at, v3_scale(d, -1), C_TEAL, 8);
    skip[ns++] = id;
  }
  v3 end = v3_add(o, v3_scale(d, hw ? wt : 120));
  for (int i = 0; i < 4; i++) if (BM[i].life <= 0) { BM[i].a = o; BM[i].b = end; BM[i].life = 0.3f; BM[i].w = 0.16f + 0.16f * K->charge; break; }
  fx_muzzle(o, d);
  fx_glow(o, 1.2f + K->charge, C_TEAL);
  if (hw) { fx_sparks(end, n, C_TEAL, 6 + (int)(K->charge * 8)); fx_glow(end, 1.5f + 1.5f * K->charge, 0xbff8f0); }
  sfx(SFX_COIL, 0, 0.6f + 0.4f * K->charge);
  out->recoil = 0.25f + 0.5f * K->charge;
  K->ammo[WP_COIL]--;
}

/* ── melee: the iron (or a shove before the iron is found) ── */
static void melee_resolve(const weapon_in_t *in, weapon_out_t *out) {
  bool iron = K->owned[WP_IRON];
  float reach = iron ? IRON_REACH : 1.4f, dmg = iron ? IRON_DMG : PUNCH_DMG;
  v3 f = v3_(sinf(in->yaw), 0, cosf(in->yaw));
  v3 c = v3_add(in->body, v3_scale(f, reach * 0.5f));
  int ids[12], n = enemy_in_sphere(c, reach * 0.75f, ids, 12);
  for (int k = 0; k < n; k++) {
    v3 p = enemy_pos(ids[k]);
    v3 to = v3_(p.x - in->body.x, 0, p.z - in->body.z);
    float l = v3_len(to);
    /* a 90 degree arc in front */
    if (l > 0.2f && v3_dot(v3_scale(to, 1 / l), f) < 0.7071f) continue;
    v3 at = v3_add(in->body, v3_scale(f, fminf(l, reach)));
    if (enemy_damage(ids[k], dmg, at, false, iron ? WP_IRON : -1)) out->killed = true;
    out->hit = true;
    fx_sparks(at, v3_scale(f, -1), iron ? 0xff6a2a : 0xfff0c8, iron ? 8 : 3);
    if (iron) fx_spores(at, 0x9a9a8a, 4);   /* a curl of smoke off the plastic */
  }
  if (mission_hit(v3_add(in->body, v3_scale(f, reach * 0.8f)), dmg)) out->hit = true;
}

/* ── LiPos ── */
static void lipo_pop(lipo_t *l, weapon_out_t *out) {
  int before = enemy_alive();
  int ids[4];
  if (enemy_in_sphere(l->p, LIPO_R, ids, 4) > 0) out->hit = true;
  enemy_blast(l->p, LIPO_R, LIPO_DMG);
  if (enemy_alive() < before) out->killed = true;
  /* a printer inside the blast: the nearest point of its box takes a full objective hit */
  for (int i = 0; i < LM_PRINTER_N; i++) {
    const lab_box_t *b = &LM_PRINTER[i];
    v3 q = v3_(ds_clamp(l->p.x, b->min[0], b->max[0]), ds_clamp(l->p.y, b->min[1], b->max[1]), ds_clamp(l->p.z, b->min[2], b->max[2]));
    if (v3_len(v3_sub(q, l->p)) < LIPO_R && mission_hit(q, 150)) out->hit = true;
  }
  mission_hit(l->p, LIPO_DMG);
  fx_explosion(l->p, 1.4f);
  sfx_at(SFX_BOOM, l->p.x, l->p.y, l->p.z, 1);
  out->recoil = fmaxf(out->recoil, 0.35f);
  l->on = false;
  ((t3_object *)l->mesh)->visible = false;
}

static void lipos_update(float dt, weapon_out_t *outs) {
  for (int i = 0; i < MAXL; i++) {
    lipo_t *l = &L[i];
    if (!l->on) continue;
    weapon_out_t *out = &outs[l->owner];
    l->t += dt;
    if (!l->puffing) {
      l->v.y -= GRAV_LIPO * dt;
      v3 np = v3_add(l->p, v3_scale(l->v, dt));
      v3 n;
      if (world_push_sphere(&np, 0.14f, &n)) {
        float vn = v3_dot(l->v, n);
        if (vn < 0) l->v = v3_sub(l->v, v3_scale(n, 1.4f * vn));
        l->v = v3_scale(l->v, n.y > 0.6f ? 0.55f : 0.8f);
        if (!l->touched) { l->touched = true; sfx_at(SFX_PLINK, np.x, np.y, np.z, 0.5f); }
        if (n.y > 0.6f && v3_len(l->v) < 1.5f) l->v = v3_(0, 0, 0);
      }
      l->p = np;
      l->spin += dt * v3_len(l->v) * 2;
      /* first contact (or a moment in the air) starts the puff */
      if (l->touched || l->t > 0.9f) {
        l->puffing = true; l->fuse = LIPO_PUFF;
        sfx_at(SFX_LIPO_PUFF, l->p.x, l->p.y, l->p.z, 0.9f);
      }
    } else {
      /* it keeps settling while it swells */
      l->v.y -= GRAV_LIPO * dt;
      l->v.x *= expf(-4 * dt); l->v.z *= expf(-4 * dt);
      v3 np = v3_add(l->p, v3_scale(l->v, dt)), n;
      if (world_push_sphere(&np, 0.14f, &n)) { float vn = v3_dot(l->v, n); if (vn < 0) l->v = v3_sub(l->v, v3_scale(n, vn)); }
      l->p = np;
      l->fuse -= dt;
      float k = 1 - l->fuse / LIPO_PUFF;
      if ((int)(l->fuse * 9) != (int)((l->fuse + dt) * 9)) fx_spores(v3_add(l->p, v3_(0, 0.1f, 0)), 0xb8b8c0, 2);
      fx_glow(l->p, 0.4f + k * 0.9f * (0.8f + 0.2f * sinf(wtime * 40)), k > 0.7f ? 0xff7a1a : 0xffd36a);
      if (l->fuse <= 0) { lipo_pop(l, out); continue; }
    }
    float swell = l->puffing ? 1 + 0.9f * (1 - l->fuse / LIPO_PUFF) : 1;
    t3_object_set_position(l->mesh, l->p.x, l->p.y, l->p.z);
    t3_object_set_rotation(l->mesh, l->puffing ? 0 : l->spin, l->spin * 0.3f, 0);
    t3_object_set_scale(l->mesh, 1 + (swell - 1) * 0.2f, swell * swell, 1 + (swell - 1) * 0.3f);
  }
}

static void throw_lipo(const weapon_in_t *in, weapon_out_t *out) {
  lipo_t *l = NULL;
  for (int i = 0; i < MAXL; i++) if (!L[i].on) { l = &L[i]; break; }
  if (!l) return;
  v3 d = v3_norm(in->dir);
  v3 f = v3_norm(v3_(d.x, 0, d.z));
  l->on = true; l->touched = l->puffing = false; l->t = 0; l->spin = 0; l->owner = kp_id;
  l->p = v3_add(v3_add(in->body, v3_scale(f, 0.35f)), v3_(0, 0.3f, 0));
  l->v = v3_add(v3_scale(d, 12), v3_(0, 4.5f, 0));
  ((t3_object *)l->mesh)->visible = true;
  K->lipos--;
  out->threw = true;
  sfx(SFX_LIPO_THROW, 0, 0.8f);
}

static void beams_update(float dt) {
  for (int i = 0; i < 4; i++) {
    if (BM[i].life <= 0) continue;
    float k = BM[i].life / 0.3f;
    fx_beam(BM[i].a, BM[i].b, C_TEAL, BM[i].w * (0.4f + 0.6f * k));
    fx_beam(BM[i].a, BM[i].b, 0xffffff, BM[i].w * 0.35f * k);
    /* rings of glow down its length so it reads end on */
    float L0 = v3_len(v3_sub(BM[i].b, BM[i].a));
    int n = (int)fminf(24, L0 / 2.5f);
    for (int j = 1; j <= n; j++) fx_glow(v3_lerp(BM[i].a, BM[i].b, (float)j / (float)(n + 1)), 0.8f * k, C_TEAL);
    BM[i].life -= dt;
  }
}

static void draw_kit(void) {
  int n = 0;
  for (int i = 0; i < MAXS; i++) if (S[i].on && S[i].kind == SH_GLUE) {
    t3_mat4 m;
    t3_mat4_compose(&m, t3_v3(S[i].p.x, S[i].p.y, S[i].p.z), (t3_quat){0, 0, 0, 1}, t3_v3(0.2f, 0.2f, 0.2f));
    t3_instanced_mesh_set_matrix_at(im_glob, n, &m);
    t3_instanced_mesh_set_color_at(im_glob, n, t3_color_hex(C_GOO));
    n++;
  }
  im_glob->count = n; ((t3_object *)im_glob)->visible = n > 0;
  im_glob->instance_matrix->version++;
  if (im_glob->instance_color) im_glob->instance_color->version++;
  n = 0;
  for (int i = 0; i < MAXPUD; i++) if (PD[i].on) {
    float age = PUDDLE_T - PD[i].life;
    float s = PD[i].r * fminf(1, age * 5) * fminf(1, PD[i].life);
    t3_mat4 m;
    t3_mat4_compose(&m, t3_v3(PD[i].p.x, PD[i].p.y, PD[i].p.z), (t3_quat){0, 0, 0, 1}, t3_v3(s, 1, s * 0.85f));
    t3_instanced_mesh_set_matrix_at(im_puddle, n, &m);
    t3_instanced_mesh_set_color_at(im_puddle, n, t3_color_hex(0xe8d898));
    n++;
  }
  im_puddle->count = n; ((t3_object *)im_puddle)->visible = n > 0;
  im_puddle->instance_matrix->version++;
  if (im_puddle->instance_color) im_puddle->instance_color->version++;
}

static void pick(int w) {
  if (w == K->cur || w < 0 || w >= WP_COUNT || !K->owned[w]) return;
  K->cur = w; K->charging = false; K->charge = 0; K->cd = fmaxf(K->cd, 0.15f);
  sfx(SFX_UI, 0, 0.5f);
}

void weapon_tick(float dt) {
  wtime += dt;
  for (int i = 0; i < MAXPUD; i++) if (PD[i].on && (PD[i].life -= dt) <= 0) PD[i].on = false;
}

void weapon_update(float dt, const weapon_in_t *in, weapon_out_t *out) {
  memset(out, 0, sizeof *out);
  K->cd -= dt; K->swing_cd -= dt; K->throw_cd -= dt;
  enemy_notice_aim(in->eye, in->dir);
  /* switching */
  if (!K->charging) {
    if (in->select >= 0) pick(in->select);
    if (in->next || in->prev) {
      int step = in->next ? 1 : WP_COUNT - 1;
      for (int k = 1; k < WP_COUNT && K->cur >= 0; k++) { int w = (K->cur + step * k) % WP_COUNT; if (K->owned[w]) { pick(w); break; } }
    }
  }
  /* melee: its own button for any weapon, or the fire button with the iron out */
  if (K->swing_t > 0) {
    float before = K->swing_t;
    K->swing_t -= dt;
    if (!K->swing_hit && before > 0.22f && K->swing_t <= 0.22f) { K->swing_hit = true; melee_resolve(in, out); }
    if (K->owned[WP_IRON]) {
      v3 f = v3_(sinf(in->yaw), 0, cosf(in->yaw));
      fx_glow(v3_add(in->body, v3_add(v3_scale(f, 0.9f), v3_(0, 0.1f, 0))), 0.35f, 0xff6a2a);
    }
  }
  bool want_swing = in->melee || (K->cur == WP_IRON && in->fire);
  if (want_swing && K->swing_cd <= 0 && !K->charging) {
    K->swing_t = 0.32f; K->swing_cd = 0.45f; K->swing_hit = false;
    out->swing = true;
    sfx(SFX_IRON, 0, K->owned[WP_IRON] ? 0.8f : 0.4f);
  }
  /* fire */
  if (K->cur == WP_BANDS && (in->fire || in->fire_held) && K->cd <= 0 && K->swing_t <= 0) {   /* holding the trigger repeats at the fire rate */
    v3 tgt = aim_point(in);
    shot_new(SH_BAND, in->muzzle, launch(in->muzzle, tgt, BAND_SPEED, BAND_GRAV));
    fx_muzzle(in->muzzle, v3_sub(tgt, in->muzzle));
    sfx(SFX_BAND, 0, 0.8f);
    out->recoil = 0.12f;
    K->cd = BAND_GAP;
  } else if (K->cur == WP_GLUE && (in->fire || in->fire_held) && K->cd <= 0 && K->swing_t <= 0) {
    if (K->ammo[WP_GLUE] > 0) {
      v3 tgt = aim_point(in);
      shot_new(SH_GLUE, in->muzzle, launch(in->muzzle, tgt, GLUE_SPEED, GLUE_GRAV));
      sfx(SFX_GLUE, 0, 0.7f);
      out->recoil = 0.08f;
      K->ammo[WP_GLUE]--;
      K->cd = GLUE_GAP;
    } else if (in->fire) { sfx(SFX_UI, 0, 0.4f); K->cd = 0.3f; }
  } else if (K->cur == WP_COIL) {
    if (in->fire && K->ammo[WP_COIL] <= 0) sfx(SFX_UI, 0, 0.4f);
    if (in->fire_held && K->ammo[WP_COIL] > 0 && K->cd <= 0 && K->swing_t <= 0) {
      K->charging = true;
      K->charge = fminf(1, K->charge + dt / COIL_T);
      sfx(SFX_COIL_CHARGE, 0, 0.2f + 0.8f * K->charge);
      fx_glow(in->muzzle, 0.2f + 0.5f * K->charge * (0.85f + 0.15f * sinf(wtime * 50)), C_TEAL);
    }
    if (K->charging && (in->fire_released || !in->fire_held)) {
      coil_fire(in, aim_point(in), out);
      K->charging = false; K->charge = 0; K->cd = 0.35f;
    }
    out->charge = K->charge;
  }
  out->scope = in->aim && K->cur == WP_COIL;
  /* throw */
  if (in->throw_lipo && K->lipos > 0 && K->throw_cd <= 0) { throw_lipo(in, out); K->throw_cd = 0.5f; }
}

void weapon_step(float dt, weapon_out_t outs[2]) {
  shots_update(dt, outs);
  lipos_update(dt, outs);
  beams_update(dt);
  draw_kit();
}
