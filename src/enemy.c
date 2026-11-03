/* enemy.c: the army. Plastic army men (soldier.glb), their squads, pellets,
 * grenades and shockwaves, the Sergeant and the General.
 * Yaw is a rotation about +y with forward = (sin yaw, 0, cos yaw), as game.c.
 *
 * three.c has no skeleton clone, so every army man is his own parse of the
 * GLB (the bytes are cached) with his own material and AnimationMixer. Those
 * actors are the expensive part: at most MAX_ACTORS exist, they are built as
 * the player comes near (a couple a frame) and recycled through a spare list.
 * Everything else an army man carries (the rifle, a parachute, a gunner's
 * sandbags, the pellets in the air) is one instanced mesh per kind, so the
 * whole army costs its skinned meshes plus a handful of draws. */
#include "enemy.h"
#include "weapon.h"
#include "fx.h"
#include "world.h"
#include "audio.h"
#include "gen/lab_areas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXE 72            /* records: a level places 20 to 45, the bosses call in more */
#define MAX_ACTORS 40      /* built models; each has its own skeleton and mixer */
#define STREAM_R 110.f     /* a model is built when the player comes this close */
#define SLEEP_R 100.f      /* beyond this an unalerted army man is neither updated nor drawn */
#define DROP_R 150.f       /* ...and beyond this he gives his model back */
#define BUILD_PER_FRAME 2
#define MAXP 112
#define MAXQ 12            /* squads */
#define MAXW 4             /* weak points */
#define MAXV 8             /* shockwaves */
#define MAXB 8             /* blasts */
#define FLASH_T 0.1f
#define SOLDIER_H 326.5f   /* soldier.glb as authored: 3.27 in the mesh, under a 100x node scale */
#define ARMY_H 1.5f
#define GRAV 24.f
#define SHOOTERS 3         /* army men allowed to be winding up a shot at once */
#define SIGHT 40.f

/* ── actors ── */
typedef struct {
  t3_gltf *gltf;
  t3_object *root, *model;
  t3_animation_mixer *mixer;
  t3_animation_action *cur, *prev;
  float fade, dur;
  char name[24];
  bool frozen;
  t3_material *mat;
  t3_object *b_hand, *b_lhand, *b_torso, *b_abd, *b_knee[2];
} actor_t;

static t3_scene *scn;
static uint8_t *glb; static size_t glb_len;
static actor_t *spare[MAX_ACTORS + 4]; static int nspare;
static int actors_out;

static void collect_mat(t3_object *o, void *ctx) {
  actor_t *a = ctx;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  o->frustum_culled = false; o->cast_shadow = false; o->receive_shadow = false;
  t3_mesh *m = (t3_mesh *)o;
  if (!a->mat && m->material_count > 0 && m->materials[0]) a->mat = m->materials[0];
}

static actor_t *actor_new(void) {
  if (!glb) glb = ds_asset("soldier.glb", &glb_len);
  if (!glb) return NULL;
  actor_t *a = calloc(1, sizeof *a);
  a->gltf = t3_gltf_parse(glb, glb_len, NULL, NULL);
  if (!a->gltf) { free(a); return NULL; }
  a->root = t3_object_new(); a->root->rotation.order = T3_YXZ;
  a->model = a->gltf->scene;
  t3_object_add(a->root, a->model);
  t3_object_traverse(a->model, collect_mat, a);
  a->mixer = t3_animation_mixer_new(a->model);
  a->b_hand = t3_object_get_by_name(a->model, "FistR");
  a->b_lhand = t3_object_get_by_name(a->model, "FistL");
  a->b_torso = t3_object_get_by_name(a->model, "Torso");
  a->b_abd = t3_object_get_by_name(a->model, "Abdomen");
  a->b_knee[0] = t3_object_get_by_name(a->model, "LowerLegL");
  a->b_knee[1] = t3_object_get_by_name(a->model, "LowerLegR");
  return a;
}

/* a model for an army man h units tall in plastic of colour col */
static actor_t *actor_get(float h, uint32_t col) {
  actor_t *a = nspare ? spare[--nspare] : actor_new();
  if (!a) return NULL;
  float s = h / SOLDIER_H;
  t3_object_set_scale(a->model, s, s, s);
  a->cur = a->prev = NULL; a->frozen = false; a->name[0] = 0;
  if (a->mat) {
    a->mat->color = t3_color_hex(col); a->mat->emissive = t3_color_hex(0); a->mat->emissive_intensity = 1;
    a->mat->roughness = 0.4f; a->mat->metalness = 0; a->mat->version++;
  }
  a->root->visible = true;
  t3_object_add(&scn->base, a->root);
  actors_out++;
  return a;
}

static void actor_put(actor_t *a) {
  if (!a) return;
  if (a->cur) t3_animation_action_stop(a->cur);
  if (a->prev) t3_animation_action_stop(a->prev);
  a->cur = a->prev = NULL;
  t3_object_remove(&scn->base, a->root);
  actors_out--;
  if (nspare < (int)(sizeof spare / sizeof spare[0])) spare[nspare++] = a;
}

/* clip names come as "CharacterArmature|Walk": match the part after the bar */
static t3_animation_clip *actor_clip(actor_t *a, const char *name) {
  for (int i = 0; i < a->gltf->animation_count; i++) {
    const char *n = a->gltf->animations[i]->name, *bar = strrchr(n, '|');
    if (!strcmp(bar ? bar + 1 : n, name)) return a->gltf->animations[i];
  }
  return NULL;
}

/* restart: a one-shot starts from its first frame */
static void actor_play(actor_t *a, const char *name, float speed, bool restart) {
  if (!a) return;
  if (!restart && a->cur && !strcmp(a->name, name)) { if (!a->frozen) t3_animation_action_set_effective_time_scale(a->cur, speed); return; }
  t3_animation_clip *c = actor_clip(a, name);
  if (!c) return;
  t3_animation_action *act = t3_animation_mixer_clip_action(a->mixer, c);
  if (act == a->cur && !restart) return;
  if (a->prev && a->prev != act) t3_animation_action_stop(a->prev);
  a->prev = a->cur != act ? a->cur : NULL;
  a->cur = act; a->dur = c->duration; a->frozen = false;
  snprintf(a->name, sizeof a->name, "%s", name);
  if (restart) act->time = 0;
  t3_animation_action_set_effective_time_scale(act, speed);
  t3_animation_action_play(act);
  t3_animation_action_set_effective_weight(act, a->prev ? 0 : 1);
  a->fade = a->prev ? 0 : 1;
}

static void actor_update(actor_t *a, float dt, bool hold_end) {
  if (!a || !a->cur) return;
  a->fade = fminf(1, a->fade + dt * 9);
  t3_animation_action_set_effective_weight(a->cur, a->fade);
  if (a->prev) {
    t3_animation_action_set_effective_weight(a->prev, 1 - a->fade);
    if (a->fade >= 1) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  }
  if (a->frozen) { t3_animation_mixer_update(a->mixer, 0); return; }
  t3_animation_mixer_update(a->mixer, dt);
  if (hold_end && a->cur->time >= a->dur - 0.04f) { a->cur->time = a->dur - 0.03f; t3_animation_action_set_effective_time_scale(a->cur, 0); a->frozen = true; }
}

/* hold a clip at one moment: k 0..1 is how far through */
static void actor_pose(actor_t *a, const char *name, float k) {
  if (!a) return;
  actor_play(a, name, 0, false);
  if (!a->cur) return;
  if (a->prev) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  a->fade = 1; t3_animation_action_set_effective_weight(a->cur, 1);
  a->cur->time = ds_clamp(k, 0, 1) * (a->dur - 0.04f);
  a->frozen = true;
  t3_animation_mixer_update(a->mixer, 0);
}

static v3 bone_pos(t3_object *b, v3 fallback) {
  if (!b) return fallback;
  t3_vec3 w = t3_object_get_world_position(b);
  return v3_(w.x, w.y, w.z);
}

/* ── records ── */
enum { S_GUARD, S_PATROL, S_DROP, S_ALERT, S_COMBAT, S_WIND, S_BURST, S_FLINCH, S_CHARGE, S_STAGGER, S_RISE, S_DORMANT, S_DEAD };
enum { A_NONE, A_SHOOT, A_THROW, A_SPREAD, A_SHOUT, A_CALL, A_CHARGE, A_STOMP, A_SWEEP };

typedef struct { bool alive; float hp, r; v3 p; t3_mesh *mesh; } wp_t;

typedef struct {
  bool used, seen, los, grounded, melted, minion, boss, built_once, melee_on, melee_done;
  int type, state, atk, squad, slot, shots, phase, tgt;
  v3 pos, vel, kb, goal, last_seen, muzzle, home;
  float yaw, pitch, hp, maxhp, H, r, sc;    /* sc: size against a 1.5 unit army man */
  float st, cd, los_t, lost_t, dead_t, flash, hit_cd, stag, goal_t, stuck_t, slow, slow_t, burn_t, dodge_cd, chute, lean, aim, bob, wave_t;
  uint32_t col;
  actor_t *a;
  wp_t wp[MAXW]; int nwp;
  t3_mesh *prop;                           /* the megaphone, the pull-string box */
} en_t;

typedef struct {
  bool used, alert, awake;
  int n;
  v3 p, a, b;                              /* the marker, and the two ends of the patrol */
  float heading, clock, wait, born;
  int leg;                                 /* heading toward b (1) or a (0) */
} squad_t;

static en_t E[MAXE];
static squad_t SQ[MAXQ];
static enemy_target_t T[2];
static float gtime, callout_cd;
static int score_acc, boss_id = -1;
static float hurt_acc[2]; static v3 hurt_from[2];

typedef struct { float hp; int score; float H, r_frac, speed; uint32_t col; } info_t;
static const info_t INFO[EN_TYPE_COUNT] = {
  {30, 100, ARMY_H, 0.2f, 2.6f, C_ARMY},     /* rifle */
  {30, 120, ARMY_H, 0.2f, 2.6f, C_ARMY},     /* grenadier */
  {70, 200, ARMY_H, 0.22f, 0, C_TAN},        /* gunner */
  {30, 100, ARMY_H, 0.2f, 2.6f, C_ARMY},     /* para */
  {60, 200, ARMY_H, 0.2f, 3.8f, C_TAN},      /* veteran */
  {900, 3000, ARMY_H * 2, 0.2f, 3.2f, C_TAN},  /* the Sergeant */
  {2400, 10000, 12.f, 0.17f, 2.4f, 0x5d6034},  /* the General, olive drab */
};
#define WP_HP 260.f

static v3 fwd(float yaw) { return v3_(sinf(yaw), 0, cosf(yaw)); }
static v3 rgt(float yaw) { return v3_(cosf(yaw), 0, -sinf(yaw)); }
static float flat_dist(v3 a, v3 b) { return hypotf(a.x - b.x, a.z - b.z); }
static float yaw_to(v3 from, v3 to) { return atan2f(to.x - from.x, to.z - from.z); }
static v3 tchest(int t) { return v3_(T[t].pos.x, T[t].pos.y + T[t].h * 0.6f, T[t].pos.z); }
static v3 eye_of(const en_t *e) { return v3_(e->pos.x, e->pos.y + e->H * 0.85f, e->pos.z); }
static v3 en_local(const en_t *e, v3 o) { return v3_add(e->pos, v3_add(v3_add(v3_scale(rgt(e->yaw), o.x), v3_(0, o.y, 0)), v3_scale(fwd(e->yaw), o.z))); }
static bool targetable(const en_t *e) { return e->used && e->state != S_DEAD && e->state != S_DORMANT && e->a; }
static bool is_boss(int type) { return type == EN_SERGEANT || type == EN_GENERAL; }

/* ── shouts ── */
static struct { const char *spk, *line; } SH[6];
static int nsh;
static void shout(const char *spk, const char *line) {
  if (nsh < 6) { SH[nsh].spk = spk; SH[nsh].line = line; nsh++; }
}
/* squad chatter, rationed so the walkie is not wall to wall */
static void callout(const char *line) {
  if (callout_cd > 0) return;
  callout_cd = 9;
  shout("ARMY MAN", line);
}
const char *enemy_take_shout(const char **speaker) {
  if (!nsh) return NULL;
  const char *l = SH[0].line;
  if (speaker) *speaker = SH[0].spk;
  memmove(SH, SH + 1, sizeof SH[0] * (size_t)(nsh - 1));
  nsh--;
  return l;
}

/* ── baked props: a few primitives moved into place and merged into one geometry ── */
typedef struct { float *p, *n; int count, cap; } bake_t;

static void bake(bake_t *b, t3_geometry *g, float rx, float ry, float x, float y, float z) {
  if (rx != 0) t3_geometry_rotate_x(g, rx);
  if (ry != 0) t3_geometry_rotate_y(g, ry);
  t3_geometry_translate(g, x, y, z);
  t3_geometry *ni = t3_geometry_to_non_indexed(g);
  t3_attribute *P = ni->attributes[T3_ATTR_POSITION], *N = ni->attributes[T3_ATTR_NORMAL];
  int c = P->count;
  if (b->count + c > b->cap) {
    b->cap = (b->count + c) * 2;
    b->p = realloc(b->p, sizeof(float) * 3 * (size_t)b->cap);
    b->n = realloc(b->n, sizeof(float) * 3 * (size_t)b->cap);
  }
  memcpy(b->p + b->count * 3, P->array, sizeof(float) * 3 * (size_t)c);
  if (N) memcpy(b->n + b->count * 3, N->array, sizeof(float) * 3 * (size_t)c);
  else for (int i = 0; i < c * 3; i++) b->n[b->count * 3 + i] = i % 3 == 1;
  b->count += c;
  t3_release(ni); t3_release(g);
}
static void bake_box(bake_t *b, float w, float h, float d, float x, float y, float z) { bake(b, t3_box_geometry_new(w, h, d, 1, 1, 1), 0, 0, x, y, z); }

static t3_geometry *bake_done(bake_t *b) {
  t3_geometry *g = t3_geometry_new();
  t3_attribute *P = t3_attribute_new(T3_FLOAT32, b->p, b->count, 3), *N = t3_attribute_new(T3_FLOAT32, b->n, b->count, 3);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, P);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, N);
  t3_release(P); t3_release(N);
  t3_geometry_compute_bounding_sphere(g);
  free(b->p); free(b->n);
  memset(b, 0, sizeof *b);
  return g;
}

/* the moulded rifle, for a 1.5 unit army man: the grip at the origin, the barrel down +z */
static t3_geometry *geo_rifle(void) {
  bake_t b = {0};
  /* chunky, as a moulded rifle is: thin parts would vanish at 480 x 270 */
  bake_box(&b, 0.075f, 0.1f, 0.34f, 0, 0.02f, 0.06f);       /* receiver */
  bake_box(&b, 0.065f, 0.13f, 0.22f, 0, -0.02f, -0.2f);     /* stock */
  bake_box(&b, 0.05f, 0.11f, 0.06f, 0, -0.07f, 0.1f);       /* magazine */
  bake_box(&b, 0.065f, 0.075f, 0.14f, 0, 0.025f, 0.29f);    /* hand guard */
  bake(&b, t3_cylinder_geometry_new(0.022f, 0.022f, 0.22f, 5, 1, false), DS_PI / 2, 0, 0, 0.04f, 0.45f);   /* barrel */
  return bake_done(&b);
}

/* a parachute over a 1.5 unit army man, feet at the origin: a dome and four lines to the shoulders */
static t3_geometry *geo_chute(void) {
  bake_t b = {0};
  bake(&b, t3_sphere_geometry_new_ex(1.05f, 10, 4, 0, DS_PI * 2, 0, DS_PI * 0.38f), 0, 0, 0, 2.15f, 0);
  float rim = 1.05f * sinf(DS_PI * 0.38f), ry = 2.15f + 1.05f * cosf(DS_PI * 0.38f);
  for (int i = 0; i < 4; i++) {
    float a = DS_PI * 0.25f + i * DS_PI * 0.5f;
    v3 top = v3_(cosf(a) * rim, ry, sinf(a) * rim), bot = v3_(cosf(a) * 0.14f, 1.3f, sinf(a) * 0.08f);
    v3 d = v3_sub(top, bot), m = v3_scale(v3_add(top, bot), 0.5f);
    float L = v3_len(d);
    t3_geometry *g = t3_box_geometry_new(0.018f, L, 0.018f, 1, 1, 1);
    /* tilt the line from +y toward d: about x by the z lean, then about y */
    float lean = acosf(ds_clamp(d.y / L, -1, 1));
    t3_geometry_rotate_x(g, lean);
    t3_geometry_rotate_y(g, atan2f(d.x, d.z));
    bake(&b, g, 0, 0, m.x, m.y, m.z);
  }
  return bake_done(&b);
}

/* a gunner's nest: a horseshoe of sandbags round the feet, open at the back */
static t3_geometry *geo_bags(void) {
  bake_t b = {0};
  for (int i = 0; i < 7; i++) {
    float a = (i - 3) * 0.42f;
    t3_geometry *g = t3_box_geometry_new(0.42f, 0.2f, 0.24f, 1, 1, 1);
    bake(&b, g, 0, a, sinf(a) * 0.72f, 0.1f, cosf(a) * 0.72f);
  }
  for (int i = 0; i < 4; i++) {
    float a = (i - 1.5f) * 0.42f;
    bake(&b, t3_box_geometry_new(0.4f, 0.18f, 0.22f, 1, 1, 1), 0, a, sinf(a) * 0.72f, 0.29f, cosf(a) * 0.72f);
  }
  return bake_done(&b);
}

/* the Sergeant's megaphone (1.5 unit frame, the handle at the origin, the bell down +z) */
static t3_geometry *geo_megaphone(void) {
  bake_t b = {0};
  bake(&b, t3_cylinder_geometry_new(0.16f, 0.05f, 0.34f, 10, 1, true), DS_PI / 2, 0, 0, 0.08f, 0.2f);
  bake(&b, t3_cylinder_geometry_new(0.05f, 0.05f, 0.08f, 8, 1, false), DS_PI / 2, 0, 0, 0.08f, 0.0f);
  bake_box(&b, 0.04f, 0.12f, 0.05f, 0, 0.0f, 0.02f);
  return bake_done(&b);
}

/* the General's talking box, worn on the back (1.5 unit frame: the box's front face at z 0) with its ring on a string */
static t3_geometry *geo_pullbox(void) {
  bake_t b = {0};
  bake_box(&b, 0.36f, 0.4f, 0.14f, 0, 0, -0.07f);
  bake_box(&b, 0.22f, 0.22f, 0.02f, 0, 0.03f, -0.145f);    /* the speaker grille */
  bake_box(&b, 0.008f, 0.008f, 0.12f, 0.1f, -0.12f, -0.2f);
  bake(&b, t3_torus_geometry_new(0.045f, 0.01f, 6, 10, DS_PI * 2), 0, 0, 0.1f, -0.12f, -0.27f);
  return bake_done(&b);
}

/* ── instanced kit ── */
typedef struct { t3_instanced_mesh *im; int cap, n; } kit_t;
static kit_t K_RIFLE, K_CHUTE, K_BAGS, K_PELLET;
static t3_geometry *g_ball;

static kit_t kit_make(t3_geometry *g, t3_material *m, int cap) {
  kit_t k = { t3_instanced_mesh_new(g, m, cap), cap, 0 };
  t3_instanced_mesh_set_color_at(k.im, 0, t3_color_hex(0xffffff));
  t3_object *o = (t3_object *)k.im;
  o->frustum_culled = false; o->cast_shadow = false; o->receive_shadow = false;
  k.im->count = 0; o->visible = false;
  t3_object_add(&scn->base, k.im);
  t3_release(g); t3_release(m);
  return k;
}
static void kit_put(kit_t *k, v3 p, float yaw, float pitch, v3 s, uint32_t col) {
  if (k->n >= k->cap) return;
  t3_mat4 m;
  t3_quat q = t3_quat_from_euler((t3_euler){-pitch, yaw, 0, T3_YXZ});
  t3_mat4_compose(&m, t3_v3(p.x, p.y, p.z), q, t3_v3(s.x, s.y, s.z));
  t3_instanced_mesh_set_matrix_at(k->im, k->n, &m);
  t3_instanced_mesh_set_color_at(k->im, k->n, t3_color_hex(col));
  k->n++;
}
static void kit_flush(kit_t *k) {
  if (!k->im) return;
  k->im->count = k->n;
  ((t3_object *)k->im)->visible = k->n > 0;
  k->im->instance_matrix->version++;
  if (k->im->instance_color) k->im->instance_color->version++;
  k->n = 0;
}

static t3_material *plastic_mat(bool two_sided) {
  t3_material *m = t3_mesh_standard_material_new(0xffffff);
  m->roughness = 0.4f; m->metalness = 0;
  if (two_sided) m->side = T3_DOUBLE_SIDE;
  m->version++;
  return m;
}

/* ── projectiles, shockwaves, blasts ── */
enum { PJ_PELLET, PJ_GRENADE };
typedef struct { bool on, landed; int kind; v3 p, pp, v, land; float life, dmg, r, fuse; uint32_t col; } proj_t;
typedef struct { bool on, hit[2]; v3 c; float r, speed, max, dmg, delay; uint32_t col; } wave_t;
typedef struct { float life; bool hit[2]; v3 c; float r, dmg; } blast_t;
static proj_t PJ[MAXP];

static wave_t WV[MAXV];
static blast_t BL[MAXB];

#define PELLET_COL 0xffd84a

static proj_t *pellet(v3 from, v3 dir, float speed, float dmg) {
  for (int i = 0; i < MAXP; i++) if (!PJ[i].on) {
    proj_t *q = &PJ[i];
    memset(q, 0, sizeof *q);
    q->on = true; q->kind = PJ_PELLET; q->p = q->pp = from; q->v = v3_scale(v3_norm(dir), speed);
    q->dmg = dmg; q->r = 0.1f; q->life = 3.5f; q->col = PELLET_COL;
    return q;
  }
  return NULL;
}

/* a grenade lobbed to land at `to` after tf seconds */
static void grenade(v3 from, v3 to, float tf, float dmg) {
  proj_t *q = pellet(from, v3_(0, 1, 0), 1, dmg);
  if (!q) return;
  float g = 20;
  q->kind = PJ_GRENADE; q->r = 0.14f; q->col = 0x3e5622; q->fuse = 0.7f;
  q->v = v3_((to.x - from.x) / tf, (to.y - from.y + 0.5f * g * tf * tf) / tf, (to.z - from.z) / tf);
  q->land = to; q->life = tf + 3;
}

static void shockwave(v3 c, float speed, float max, float dmg, float delay, uint32_t col) {
  for (int i = 0; i < MAXV; i++) if (!WV[i].on) { WV[i] = (wave_t){true, {false, false}, c, 0.3f, speed, max, dmg, delay, col}; return; }
}
static void blast_at(v3 c, float r, float dmg) {
  for (int i = 0; i < MAXB; i++) if (BL[i].life <= 0) { BL[i] = (blast_t){0.1f, {false, false}, c, r, dmg}; return; }
}

/* does a sphere touch a standing cylinder (feet at p)? */
static bool cyl_sphere(v3 p, float r, float h, v3 c, float rad) {
  float dx = c.x - p.x, dz = c.z - p.z, l = hypotf(dx, dz);
  float cx = l > r ? p.x + dx / l * r : c.x, cz = l > r ? p.z + dz / l * r : c.z;
  float cy = ds_clamp(c.y, p.y, p.y + h);
  float ex = c.x - cx, ey = c.y - cy, ez = c.z - cz;
  return ex * ex + ey * ey + ez * ez < rad * rad;
}

static void hurt(int t, float dmg, v3 from) {
  if (!T[t].alive || dmg <= 0) return;
  hurt_acc[t] += dmg; hurt_from[t] = from;
}

static void grenade_pop(proj_t *q) {
  fx_explosion(q->p, 0.8f);
  sfx_at(SFX_BOOM, q->p.x, q->p.y, q->p.z, 0.8f);
  blast_at(q->p, 3, q->dmg);
  q->on = false;
}

static void proj_update(float dt) {
  for (int i = 0; i < MAXP; i++) {
    proj_t *q = &PJ[i];
    if (!q->on) continue;
    q->pp = q->p;
    q->life -= dt;
    if (q->kind == PJ_GRENADE) {
      /* a fuse spark on top, and a ring where it will land */
      v3 lp = q->landed ? q->p : q->land;
      float fy = world_floor_y(v3_(lp.x, lp.y + 0.5f, lp.z));
      if (fy > -1e8f && ((int)(gtime * 10) & 1)) fx_ring_flat(v3_(lp.x, fy + 0.04f, lp.z), 3, 0xff4a30);
      fx_glow(v3_add(q->p, v3_(0, 0.12f, 0)), 0.35f + 0.2f * ((int)(gtime * 24 + i) & 1), 0xffb347);
      if (q->landed) {
        q->fuse -= dt;
        if (q->fuse <= 0 || q->life <= 0) grenade_pop(q);
        continue;
      }
      q->v.y -= 20 * dt;
    }
    v3 step = v3_scale(q->v, dt);
    float sl = v3_len(step), t;
    bool end = q->life <= 0;
    v3 n = v3_(0, 1, 0);
    if (sl > 1e-4f && world_raycast(q->p, v3_scale(step, 1 / sl), sl + q->r, &t, &n)) {
      q->p = v3_add(q->p, v3_scale(step, fmaxf(0, t - q->r) / sl));
      if (q->kind == PJ_GRENADE) {
        /* bounces once off walls, settles on anything flat */
        if (n.y > 0.5f) { q->landed = true; q->v = v3_(0, 0, 0); }
        else { float vn = v3_dot(q->v, n); q->v = v3_scale(v3_sub(q->v, v3_scale(n, 2 * vn)), 0.4f); }
        sfx_at(SFX_PLINK, q->p.x, q->p.y, q->p.z, 0.3f);
        continue;
      }
      end = true;
    } else q->p = v3_add(q->p, step);
    if (q->kind == PJ_PELLET) {
      /* readable from across the room: a glow, a short trail, and the pellet itself */
      fx_glow(q->p, 0.5f, q->col);
      fx_beam(q->p, v3_sub(q->p, v3_scale(v3_norm(q->v), 0.7f)), 0xffb347, 0.07f);
      v3 mid = v3_lerp(q->pp, q->p, 0.5f);
      for (int k = 0; k < 2 && !end; k++) {
        if (!T[k].alive) continue;
        if (cyl_sphere(T[k].pos, T[k].r, T[k].h, q->p, q->r) || cyl_sphere(T[k].pos, T[k].r, T[k].h, mid, q->r)) {
          hurt(k, q->dmg, q->pp); end = true;
        }
      }
      if (end) { fx_sparks(q->p, v3_scale(v3_norm(q->v), -1), 0xffd36a, 3); q->on = false; }
    } else if (end) grenade_pop(q);
    else for (int k = 0; k < 2; k++) if (T[k].alive && cyl_sphere(T[k].pos, T[k].r, T[k].h, q->p, q->r + 0.1f)) { q->landed = true; q->v = v3_(0, 0, 0); q->fuse = fminf(q->fuse, 0.15f); }
  }
  for (int i = 0; i < MAXV; i++) {
    wave_t *w = &WV[i];
    if (!w->on) continue;
    if (w->delay > 0) { w->delay -= dt; continue; }
    w->r += w->speed * dt;
    fx_ring_flat(v3_(w->c.x, w->c.y + 0.08f, w->c.z), w->r, w->col);
    fx_ring_flat(v3_(w->c.x, w->c.y + 0.08f, w->c.z), fmaxf(0.1f, w->r - 0.35f), 0xfff0c8);
    for (int k = 0; k < 2; k++) {
      if (w->hit[k] || !T[k].alive) continue;
      float d = flat_dist(T[k].pos, w->c);
      /* a ring along the ground: jump it */
      if (fabsf(d - w->r) < 0.6f + T[k].r && T[k].pos.y < w->c.y + 0.7f && T[k].pos.y + T[k].h > w->c.y - 0.5f) { w->hit[k] = true; hurt(k, w->dmg, w->c); }
    }
    if (w->r >= w->max) w->on = false;
  }
  for (int i = 0; i < MAXB; i++) {
    blast_t *b = &BL[i];
    if (b->life <= 0) continue;
    b->life -= dt;
    for (int k = 0; k < 2; k++) {
      if (b->hit[k] || !T[k].alive) continue;
      v3 p = T[k].pos;
      v3 c = v3_(ds_clamp(b->c.x, p.x - T[k].r, p.x + T[k].r), ds_clamp(b->c.y, p.y, p.y + T[k].h), ds_clamp(b->c.z, p.z - T[k].r, p.z + T[k].r));
      float d = v3_len(v3_sub(c, b->c));
      if (d < b->r) { b->hit[k] = true; hurt(k, b->dmg * (1 - 0.6f * d / b->r), b->c); }
    }
  }
}

float enemy_hurt(int target, v3 *from) {
  if (target < 0 || target > 1) return 0;
  float d = hurt_acc[target];
  if (d > 0 && from) *from = hurt_from[target];
  hurt_acc[target] = 0;
  return d;
}

/* ── geometry queries ── */
static float sph_hit(v3 ro, v3 rd, v3 c, float r) {
  v3 oc = v3_sub(ro, c);
  float b = v3_dot(oc, rd), k = v3_dot(oc, oc) - r * r, h = b * b - k;
  if (h < 0) return -1;
  float t = -b - sqrtf(h);
  if (t < 0) t = -b + sqrtf(h);
  return t;
}
static float cap_hit(v3 ro, v3 rd, v3 pa, v3 pb, float r) {
  float best = -1, t;
  v3 ba = v3_sub(pb, pa), oa = v3_sub(ro, pa);
  float baba = v3_dot(ba, ba), bard = v3_dot(ba, rd), baoa = v3_dot(ba, oa), rdoa = v3_dot(rd, oa), oaoa = v3_dot(oa, oa);
  float a = baba - bard * bard, b = baba * rdoa - baoa * bard, c = baba * oaoa - baoa * baoa - r * r * baba, h = b * b - a * c;
  if (a > 1e-6f && h >= 0) {
    t = (-b - sqrtf(h)) / a;
    float y = baoa + t * bard;
    if (t >= 0 && y > 0 && y < baba) best = t;
  }
  t = sph_hit(ro, rd, pa, r); if (t >= 0 && (best < 0 || t < best)) best = t;
  t = sph_hit(ro, rd, pb, r); if (t >= 0 && (best < 0 || t < best)) best = t;
  return best;
}
/* the body as a capsule; a fallen army man is not a target */
static void body_cap(const en_t *e, v3 *a, v3 *b, float *r) {
  *r = e->r;
  *a = v3_(e->pos.x, e->pos.y + e->r, e->pos.z);
  *b = v3_(e->pos.x, e->pos.y + fmaxf(e->H - e->r, e->r), e->pos.z);
}

/* enemy_raycast that can leave some ids out (the coilgun's pierce) */
bool enemy_raycast_skip(v3 o, v3 d, float max_dist, const int *skip, int nskip, int *id, float *t, bool *weak) {
  float best = max_dist; int bi = -1; bool bw = false;
  d = v3_norm(d);
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!targetable(e)) continue;
    bool skipped = false;
    for (int k = 0; k < nskip; k++) if (skip[k] == i) skipped = true;
    if (skipped) continue;
    v3 a, b; float r; body_cap(e, &a, &b, &r);
    /* cheap reject: the ray's closest approach to the capsule's middle */
    v3 mid = v3_scale(v3_add(a, b), 0.5f), om = v3_sub(mid, o);
    float along = v3_dot(om, d), reach = e->H * 0.5f + r + 1.5f;
    if (along < -reach || along > best + reach) continue;
    if (v3_dot(om, om) - along * along > reach * reach) continue;
    float tb = cap_hit(o, d, a, b, r), tw = -1;
    for (int k = 0; k < e->nwp; k++) if (e->wp[k].alive) {
      float x = sph_hit(o, d, e->wp[k].p, e->wp[k].r);
      if (x >= 0 && (tw < 0 || x < tw)) tw = x;
    }
    float th = tb; bool isw = false;
    if (tw >= 0 && (tb < 0 || tw < tb + e->r * 0.4f)) { th = tw; isw = true; }
    if (th >= 0 && th < best) { best = th; bi = i; bw = isw; }
  }
  if (bi < 0) return false;
  if (id) *id = bi;
  if (t) *t = best;
  if (weak) *weak = bw;
  return true;
}
bool enemy_raycast(v3 o, v3 d, float max_dist, int *id, float *t, bool *weak) { return enemy_raycast_skip(o, d, max_dist, NULL, 0, id, t, weak); }

bool enemy_weak_point_near(v3 o, v3 d, float cc, v3 *at) {
  float best = cc; bool f = false; d = v3_norm(d);
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!targetable(e)) continue;
    for (int k = 0; k < e->nwp; k++) if (e->wp[k].alive) {
      v3 w = v3_sub(e->wp[k].p, o);
      float l = v3_len(w);
      if (l < 1e-3f) continue;
      float c = v3_dot(w, d) / l;
      if (c >= best) { best = c; if (at) *at = e->wp[k].p; f = true; }
    }
  }
  return f;
}

static float cap_dist(const en_t *e, v3 c) {
  v3 a, b; float r; body_cap(e, &a, &b, &r);
  v3 p = v3_(e->pos.x, ds_clamp(c.y, a.y, b.y), e->pos.z);
  return fmaxf(0, v3_len(v3_sub(c, p)) - r);
}

int enemy_in_sphere(v3 c, float r, int *ids, int max) {
  int n = 0;
  for (int i = 0; i < MAXE && n < max; i++) if (targetable(&E[i]) && cap_dist(&E[i], c) < r) ids[n++] = i;
  return n;
}

v3 enemy_pos(int id) { return id >= 0 && id < MAXE && E[id].used ? E[id].pos : v3_(0, 0, 0); }

void enemy_slow(int id, float factor, float seconds) {
  if (id < 0 || id >= MAXE || !E[id].used) return;
  en_t *e = &E[id];
  e->slow = e->slow_t > 0 ? fminf(e->slow, factor) : factor;
  e->slow_t = fmaxf(e->slow_t, seconds);
}

int enemy_alive(void) { int n = 0; for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].state != S_DEAD) n++; return n; }
int enemy_alive_near(v3 c, float r) {
  int n = 0;
  for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].state != S_DEAD && v3_len(v3_sub(E[i].pos, c)) < r) n++;
  return n;
}
int enemy_take_score(void) { int s = score_acc; score_acc = 0; return s; }

int enemy_nearest(v3 from, float range, bool need_sight) {
#if defined(SP_TEST_ARMY) && (SP_TEST_ARMY == 4 || SP_TEST_ARMY == 5)
  return -1;
#endif
  int ids[MAXE]; float ds[MAXE]; int n = 0;
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!targetable(e) || (e->a && !e->a->root->visible)) continue;
    float d = v3_len(v3_sub(e->pos, from));
    if (d < range) { ids[n] = i; ds[n] = d; n++; }
  }
  /* nearest first, sight tested on the closest few */
  for (int tries = 0; tries < 4 && n > 0; tries++) {
    int bi = 0;
    for (int k = 1; k < n; k++) if (ds[k] < ds[bi]) bi = k;
    en_t *e = &E[ids[bi]];
    if (!need_sight || world_visible(from, v3_(e->pos.x, e->pos.y + e->H * 0.55f, e->pos.z))) return ids[bi];
    ids[bi] = ids[n - 1]; ds[bi] = ds[n - 1]; n--;
  }
  return -1;
}

/* ── bosses ── */
bool enemy_boss_active(void) { return boss_id >= 0 && E[boss_id].used && E[boss_id].state != S_DEAD && E[boss_id].state != S_DORMANT; }
float enemy_boss_health(void) { return boss_id >= 0 && E[boss_id].used ? E[boss_id].hp / E[boss_id].maxhp : 0; }
int enemy_boss_phase(void) { return boss_id >= 0 && E[boss_id].used ? E[boss_id].phase : 1; }
bool enemy_boss_dead(void) { return boss_id >= 0 && E[boss_id].used && E[boss_id].state == S_DEAD; }
const char *enemy_boss_name(void) {
  if (boss_id < 0 || !E[boss_id].used) return "";
  return E[boss_id].type == EN_GENERAL ? "THE GENERAL" : "THE SERGEANT";
}
void enemy_boss_wake(void) {
  if (boss_id < 0 || !E[boss_id].used || E[boss_id].state != S_DORMANT) return;
  en_t *e = &E[boss_id];
  e->state = S_RISE; e->st = 0; e->seen = true;
  if (e->a) actor_play(e->a, "StandUp", 1, true);
}

/* ── squads ── */
/* how far a squad can march from p along heading h and stay on the same surface */
static float march_reach(v3 p, float h, float max) {
  v3 f = fwd(h);
  float d = 0;
  for (float s = 1; s <= max; s += 1) {
    v3 q = v3_add(p, v3_scale(f, s));
    float fy = world_floor_y(v3_(q.x, p.y + 0.6f, q.z));
    if (fabsf(fy - p.y) > 0.45f) break;
    if (!world_visible(v3_(p.x, p.y + 0.7f, p.z), v3_(q.x, p.y + 0.7f, q.z))) break;
    d = s;
  }
  return fmaxf(0, d - 1.2f);
}

static int squad_join(v3 pos, float yaw) {
  for (int i = 0; i < MAXQ; i++) {
    squad_t *q = &SQ[i];
    if (q->used && q->n < 5 && !q->alert && gtime - q->born < 0.5f && flat_dist(q->a, pos) < 7 && fabsf(q->a.y - pos.y) < 1) { q->n++; return i; }
  }
  for (int i = 0; i < MAXQ; i++) if (!SQ[i].used) {
    squad_t *q = &SQ[i];
    memset(q, 0, sizeof *q);
    q->used = true; q->n = 1; q->born = gtime; q->heading = yaw; q->leg = 1;
    float fy = world_floor_y(v3_(pos.x, pos.y + 0.5f, pos.z));
    if (fy > -1e8f) pos.y = fy;
    /* the ends leave room for the wedge trailing behind the marker after each about face */
    float ahead = march_reach(pos, yaw, 18), back = march_reach(pos, yaw + DS_PI, 8), depth = 2.4f;
    q->a = v3_sub(pos, v3_scale(fwd(yaw), back - depth));
    q->b = v3_add(pos, v3_scale(fwd(yaw), ahead - depth));
    if (v3_dot(v3_sub(q->b, q->a), fwd(yaw)) < 0) q->b = q->a;
    float along = ds_clamp(v3_dot(v3_sub(pos, q->a), fwd(yaw)), 0, flat_dist(q->a, q->b));
    q->p = v3_add(q->a, v3_scale(fwd(yaw), along));
    q->wait = ds_rr(0.5f, 1.5f);
    return i;
  }
  return -1;
}

/* where member `slot` stands: a wedge behind the marker */
static v3 squad_slot(const squad_t *q, int slot) {
  int row = (slot + 1) / 2;
  float side = slot == 0 ? 0 : (slot & 1 ? -0.75f : 0.75f);
  return v3_add(v3_add(q->p, v3_scale(fwd(q->heading), -1.25f * row)), v3_scale(rgt(q->heading), side * row));
}

static void squad_alert(int s, int target, v3 seen_at) {
  if (s < 0 || !SQ[s].used || SQ[s].alert) return;
  squad_t *q = &SQ[s];
  q->alert = true;
  sfx_at(SFX_WHISTLE, q->p.x, q->p.y + 1, q->p.z, 0.8f);
  callout(ds_rand() < 0.6f ? "CONTACT!" : "ENEMY SIGHTED. FORM UP!");
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!e->used || e->squad != s || e->state == S_DEAD) continue;
    if (e->state == S_PATROL || e->state == S_GUARD) { e->state = S_ALERT; e->st = ds_rr(0.25f, 0.7f); }
    e->seen = true; e->tgt = target; e->last_seen = seen_at;
  }
}

static void squads_update(float dt) {
  for (int s = 0; s < MAXQ; s++) {
    squad_t *q = &SQ[s];
    if (!q->used) continue;
    if (q->n <= 0) { q->used = false; continue; }
    if (q->alert || !q->awake) continue;
    q->clock += dt;
    float len = flat_dist(q->a, q->b);
    if (len < 2.5f) continue;   /* nowhere to march: they stand guard */
    if (q->wait > 0) { q->wait -= dt; continue; }
    /* the marker waits for stragglers so the wedge keeps its shape */
    bool lag = false;
    for (int i = 0; i < MAXE; i++) {
      en_t *e = &E[i];
      if (e->used && e->squad == s && e->state == S_PATROL && e->stuck_t < 1 && flat_dist(e->pos, squad_slot(q, e->slot)) > 2.2f) lag = true;
    }
    if (lag) continue;
    v3 goal = q->leg ? q->b : q->a;
    float want = yaw_to(q->p, goal), dh = ds_wrap_pi(want - q->heading);
    if (fabsf(dh) > 0.05f) { q->heading += ds_clamp(dh, -2.2f * dt, 2.2f * dt); continue; }   /* about face, then march */
    q->heading = want;
    v3 d = v3_sub(goal, q->p); d.y = 0;
    float l = v3_len(d), step = 1.35f * dt;
    if (l <= step) { q->p = v3_(goal.x, q->p.y, goal.z); q->leg ^= 1; q->wait = ds_rr(1.5f, 3); }
    else q->p = v3_add(q->p, v3_scale(d, step / l));
  }
}

/* ── lifecycle ── */
static void wp_add(en_t *e, float r) {
  if (e->nwp >= MAXW) return;
  wp_t *w = &e->wp[e->nwp++];
  t3_material *m = t3_mesh_basic_material_new(0xffa030);
  w->mesh = t3_mesh_new(g_ball, m);
  t3_release(m);
  ((t3_object *)w->mesh)->frustum_culled = false;
  ((t3_object *)w->mesh)->visible = false;
  t3_object_add(&scn->base, w->mesh);
  w->r = r; w->alive = true; w->hp = WP_HP;
  t3_object_set_scale(w->mesh, r * 1.3f, r * 1.3f, r * 1.3f);
}

static t3_mesh *prop_new(t3_geometry *g, uint32_t col) {
  t3_material *m = t3_mesh_standard_material_new(col);
  m->roughness = 0.45f; m->metalness = 0; m->side = T3_DOUBLE_SIDE; m->version++;
  t3_mesh *me = t3_mesh_new(g, m);
  t3_release(m); t3_release(g);
  ((t3_object *)me)->frustum_culled = false;
  ((t3_object *)me)->rotation.order = T3_YXZ;
  t3_object_add(&scn->base, me);
  return me;
}

static void en_free(en_t *e) {
  if (e->a) actor_put(e->a);
  for (int i = 0; i < e->nwp; i++) if (e->wp[i].mesh) { t3_object_remove(&scn->base, e->wp[i].mesh); t3_release(e->wp[i].mesh); }
  if (e->prop) { t3_object_remove(&scn->base, e->prop); t3_release(e->prop); }
  if (e->squad >= 0 && SQ[e->squad].used) SQ[e->squad].n--;
  memset(e, 0, sizeof *e);
  e->squad = -1;
}

#ifdef SP_TEST_ARMY
static bool test_spawned;
#endif

void enemy_clear(void) {
  for (int i = 0; i < MAXE; i++) if (E[i].used) en_free(&E[i]);
  memset(SQ, 0, sizeof SQ);
  memset(PJ, 0, sizeof PJ); memset(WV, 0, sizeof WV); memset(BL, 0, sizeof BL);
  boss_id = -1; score_acc = 0; nsh = 0; callout_cd = 0;
  hurt_acc[0] = hurt_acc[1] = 0;
  K_RIFLE.n = K_CHUTE.n = K_BAGS.n = K_PELLET.n = 0;
  kit_flush(&K_RIFLE); kit_flush(&K_CHUTE); kit_flush(&K_BAGS); kit_flush(&K_PELLET);
#ifdef SP_TEST_ARMY
  test_spawned = false;
#endif
}

void enemy_init(t3_scene *scene) {
  scn = scene;
  for (int i = 0; i < MAXE; i++) E[i].squad = -1;
  g_ball = t3_sphere_geometry_new(0.5f, 10, 6);
  K_RIFLE = kit_make(geo_rifle(), plastic_mat(false), MAXE);
  K_CHUTE = kit_make(geo_chute(), plastic_mat(true), 24);
  K_BAGS = kit_make(geo_bags(), plastic_mat(false), 16);
  t3_material *pm = t3_mesh_basic_material_new(0xffffff);
  K_PELLET = kit_make(t3_icosahedron_geometry_new(0.5f, 0), pm, MAXP);
  /* a few models ready before the first squad marches in */
  glb = ds_asset("soldier.glb", &glb_len);
  for (int i = 0; i < 6; i++) { actor_t *a = actor_new(); if (a) spare[nspare++] = a; }
}

static void attach_model(en_t *e) {
  e->a = actor_get(e->H, e->col);
  if (!e->a) return;
  if (e->state == S_DORMANT) actor_pose(e->a, "SitDown", 1);
  else if (e->state == S_RISE) actor_play(e->a, "StandUp", 1, true);
  else actor_play(e->a, "Idle", 1, true);
  if (e->a->cur && e->state != S_DORMANT && e->state != S_RISE) e->a->cur->time = ds_rr(0, e->a->dur);   /* not every idle in lockstep */
  t3_object_set_position(e->a->root, e->pos.x, e->pos.y, e->pos.z);
  t3_object_set_rotation(e->a->root, 0, e->yaw, 0);
  e->built_once = true;
}

void enemy_spawn(int type, v3 pos, float yaw) {
  if (type < 0 || type >= EN_TYPE_COUNT) return;
  en_t *e = NULL; int id = -1;
  for (int i = 0; i < MAXE; i++) if (!E[i].used) { e = &E[i]; id = i; break; }
  if (!e) return;
  memset(e, 0, sizeof *e);
  const info_t *I = &INFO[type];
  e->used = true; e->type = type; e->yaw = yaw; e->hp = e->maxhp = I->hp;
  e->H = I->H; e->r = I->H * I->r_frac; e->sc = I->H / ARMY_H; e->col = I->col;
  e->squad = -1; e->slow = 1; e->phase = 1; e->tgt = 0;
  e->cd = ds_rr(0.6f, 1.6f);
  e->aim = yaw;
  if (type == EN_VETERAN && ds_rand() < 0.5f) e->col = 0xa88e5c;   /* two moulds of tan */
  /* stand on whatever is under the spawn, except a paratrooper, who is in the air */
  float fy = world_floor_y(v3_(pos.x, pos.y + 0.5f, pos.z));
  if (type == EN_PARA && fy > -1e8f && pos.y > fy + 1.5f) { e->state = S_DROP; e->chute = 1; e->seen = true; }
  else { if (fy > -1e8f) pos.y = fy; e->state = S_GUARD; }
  e->pos = e->home = pos;
  e->last_seen = pos;
  if (type == EN_RIFLE || type == EN_GRENADIER || type == EN_VETERAN) {
    e->squad = squad_join(pos, yaw);
    if (e->squad >= 0) { e->slot = SQ[e->squad].n - 1; e->state = S_PATROL; }
  }
  if (is_boss(type)) {
    boss_id = id; e->boss = true;
    e->state = S_DORMANT;
    if (type == EN_SERGEANT) e->prop = prop_new(geo_megaphone(), 0xd8c9a0);
    else {
      e->prop = prop_new(geo_pullbox(), 0x6a6a5e);
      wp_add(e, 0.75f);   /* the pull-string box */
      wp_add(e, 0.55f);   /* the battery hatch */
      wp_add(e, 0.6f);    /* the knees */
      wp_add(e, 0.6f);
    }
  }
}

/* ── damage ── */
static void kill(en_t *e, int kind) {
  e->state = S_DEAD; e->dead_t = 0; e->melee_on = false;
  score_acc += INFO[e->type].score;
  sfx_at(SFX_TOPPLE, e->pos.x, e->pos.y, e->pos.z, 0.9f);
  if (kind == WP_IRON) { e->melted = true; sfx_at(SFX_MELT, e->pos.x, e->pos.y, e->pos.z, 0.8f); }
  if (e->a) actor_play(e->a, "Defeat", e->boss ? 0.6f : 1.3f, true);
  for (int i = 0; i < e->nwp; i++) e->wp[i].alive = false;
  if (e->squad >= 0 && SQ[e->squad].used && SQ[e->squad].alert) callout(ds_rand() < 0.5f ? "MAN DOWN!" : "TAKE COVER!");
  if (e->type == EN_GENERAL) {
    shout("THE GENERAL", "PULL MY STR");
    for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].minion && E[i].state != S_DEAD) { E[i].hp = 0; kill(&E[i], -1); }
  } else if (e->type == EN_SERGEANT) {
    shout("THE SERGEANT", "AT EASE. AT EASE.");
    for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].minion && E[i].state != S_DEAD) { E[i].hp = 0; kill(&E[i], -1); }
  }
}

static void boss_phase_check(en_t *e) {
  float f = e->hp / e->maxhp;
  int ph = f > 0.66f ? 1 : f > 0.33f ? 2 : 3;
  if (ph <= e->phase || e->state == S_DEAD) return;
  e->phase = ph; e->state = S_STAGGER; e->st = 1.4f; e->atk = A_NONE; e->melee_on = false;
  for (int i = 0; i < e->nwp; i++) { e->wp[i].alive = true; e->wp[i].hp = WP_HP; }
  sfx_at(SFX_ALARM, e->pos.x, e->pos.y, e->pos.z, 1);
  if (e->a) actor_play(e->a, "RecieveHit", 0.7f, true);
  if (e->type == EN_GENERAL) shout("THE GENERAL", ph == 2 ? "PULL MY STRING." : "HOLD THE LINE.");
  else shout("THE SERGEANT", ph == 2 ? "PARATROOPERS. GO, GO, GO!" : "NOBODY TOUCHES MY BENCH!");
}

bool enemy_damage(int id, float amount, v3 at, bool weak, int kind) {
  if (id < 0 || id >= MAXE || !targetable(&E[id]) || E[id].state == S_RISE) return false;
  en_t *e = &E[id];
  if (e->boss) {
    wp_t *hit = NULL; float bd = 1e9f;
    if (weak) for (int k = 0; k < e->nwp; k++) if (e->wp[k].alive) {
      float d = v3_len(v3_sub(at, e->wp[k].p));
      if (d < bd) { bd = d; hit = &e->wp[k]; }
    }
    if (hit) {
      amount *= 1.5f;
      hit->hp -= amount;
      if (hit->hp <= 0) {
        /* a weak point gives: sparks, a stagger and a chunk of health */
        hit->alive = false; amount += 120;
        fx_explosion(hit->p, e->H * 0.06f); sfx_at(SFX_BOOM, hit->p.x, hit->p.y, hit->p.z, 0.8f); score_acc += 250;
        if (e->state != S_STAGGER) { e->state = S_STAGGER; e->st = 1.2f; e->atk = A_NONE; e->melee_on = false; actor_play(e->a, "RecieveHit", 0.8f, true); }
      }
      sfx_at(SFX_BOSS_HIT, at.x, at.y, at.z, 0.8f);
      fx_sparks(at, v3_norm(v3_sub(at, e->pos)), 0xffd36a, 8);
    } else {
      weak = false;
      if (e->type == EN_GENERAL) amount *= 0.6f;   /* thick plastic: the weak points are the fight */
      sfx_at(SFX_PLINK, at.x, at.y, at.z, 0.6f);
      fx_chips(at, e->col, 3);
    }
  } else {
    sfx_at(SFX_PLINK, at.x, at.y, at.z, 0.6f);
    fx_chips(at, e->col, amount > 30 ? 6 : 3);
    v3 away = v3_norm(v3_(e->pos.x - T[0].pos.x, 0, e->pos.z - T[0].pos.z));
    float push = amount > 30 ? 4 : 0.8f;
    e->kb = v3_add(e->kb, v3_scale(away, push));
    e->lean = fminf(0.45f, e->lean + amount * 0.012f);
    e->stag += amount;
    if (e->stag >= 12 && e->hit_cd <= 0 && e->state != S_DROP && e->state != S_DEAD) {
      e->stag = 0; e->hit_cd = 1.2f; e->st = 0.45f; e->state = S_FLINCH; e->atk = A_NONE;
      if (e->a) actor_play(e->a, "RecieveHit", 1.4f, true);
    }
    /* shot at: they know where from */
    if (!e->seen) { e->seen = true; e->tgt = 0; e->last_seen = T[0].pos; }
    if (e->squad >= 0) squad_alert(e->squad, 0, T[0].pos);
  }
  if (kind == WP_GLUE) { e->burn_t = 2; e->slow = fminf(e->slow_t > 0 ? e->slow : 1, 0.4f); e->slow_t = fmaxf(e->slow_t, 2); }
  if (kind == WP_IRON) sfx_at(SFX_MELT, at.x, at.y, at.z, 0.6f);
  if (weak) score_acc += 50;
  e->hp -= amount; e->flash = FLASH_T;
  if (e->boss) boss_phase_check(e);
  if (e->hp <= 0) { e->hp = 0; kill(e, kind); return true; }
  return false;
}

void enemy_blast(v3 at, float radius, float amount) {
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!targetable(e)) continue;
    float d = cap_dist(e, at);
    if (d >= radius) continue;
    if (!e->boss) {
      /* blown off their feet */
      v3 away = v3_norm(v3_(e->pos.x - at.x, 0, e->pos.z - at.z));
      e->kb = v3_add(e->kb, v3_scale(away, 7 * (1 - d / radius)));
      e->vel.y = fmaxf(e->vel.y, 5 * (1 - d / radius));
    }
    enemy_damage(i, amount * (1 - 0.6f * d / radius), v3_(e->pos.x, e->pos.y + e->H * 0.5f, e->pos.z), false, -1);
  }
}

/* veterans sidestep the reticle (weapon.c tells us where it points each frame) */
void enemy_notice_aim(v3 eye, v3 dir) {
  dir = v3_norm(dir);
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!targetable(e) || e->type != EN_VETERAN || e->dodge_cd > 0 || (e->state != S_COMBAT && e->state != S_WIND)) continue;
    v3 c = v3_sub(v3_(e->pos.x, e->pos.y + e->H * 0.55f, e->pos.z), eye);
    float along = v3_dot(c, dir);
    if (along < 2 || along > 45) continue;
    if (v3_dot(c, c) - along * along > 0.7f * 0.7f) continue;
    v3 side = v3_norm(v3_cross(dir, v3_(0, 1, 0)));
    if (ds_rand() < 0.5f) side = v3_scale(side, -1);
    e->kb = v3_add(e->kb, v3_scale(side, 7));
    e->dodge_cd = ds_rr(1.8f, 3);
  }
}

/* ── behaviour helpers ── */
static void turn_to(en_t *e, float want, float rate, float dt) {
  float d = ds_wrap_pi(want - e->yaw);
  e->yaw += ds_clamp(d, -rate * dt, rate * dt);
}

static float slow_k(const en_t *e) { return (e->slow_t > 0 ? e->slow : 1) * weapon_glue_slow(e->pos); }

/* would a step along dir leave the surface? army men do not walk off tables */
static bool edge_ok(const en_t *e, v3 dir) {
  v3 ahead = v3_add(e->pos, v3_scale(dir, e->r + 0.45f * e->sc));
  float fy = world_floor_y(v3_(ahead.x, e->pos.y + 0.6f * e->sc, ahead.z));
  return fy > e->pos.y - 0.8f * e->sc;
}

static void walk(en_t *e, v3 dir, float speed, float dt) {
  v3 want = v3_scale(dir, speed * slow_k(e));
  if (speed > 0.05f && !edge_ok(e, dir)) want = v3_(0, 0, 0);
  float kl = hypotf(e->kb.x, e->kb.z);
  if (kl > 0.1f) {
    v3 kd = v3_scale(e->kb, 1 / kl);
    if (edge_ok(e, kd) || e->state == S_DEAD) want = v3_add(want, v3_(e->kb.x, 0, e->kb.z));
    e->kb = v3_scale(e->kb, expf(-5 * dt));
  }
  float grip = e->grounded ? 10 : 1.5f;
  e->vel.x = ds_damp(e->vel.x, want.x, grip, dt); e->vel.z = ds_damp(e->vel.z, want.z, grip, dt);
  e->vel.y -= GRAV * dt;
  world_move(&e->pos, &e->vel, e->r, e->H * 0.95f, dt, &e->grounded);
  if (e->pos.y < -30) { e->pos = e->home; e->vel = v3_(0, 0, 0); }
}

/* walk toward a point; true once there */
static bool walk_to(en_t *e, v3 goal, float speed, float dt, float turn) {
  v3 d = v3_(goal.x - e->pos.x, 0, goal.z - e->pos.z);
  float l = v3_len(d);
  if (l < 0.35f) { walk(e, v3_(0, 0, 0), 0, dt); return true; }
  d = v3_scale(d, 1 / l);
  turn_to(e, atan2f(d.x, d.z), turn, dt);
  float sp = fminf(speed, l * 3);
  float before = flat_dist(e->pos, goal);
  walk(e, d, sp, dt);
  /* no progress for a while: give up on this goal */
  if (before - flat_dist(e->pos, goal) < sp * dt * 0.25f) e->stuck_t += dt; else e->stuck_t = fmaxf(0, e->stuck_t - dt);
  return false;
}

/* sight: every 0.25 s, the nearer visible hero within range (the player preferred) */
static void sense(en_t *e, float dt, float range, bool cone) {
  e->los_t -= dt;
  if (e->los_t > 0) return;
  e->los_t = 0.22f + (float)((e - E) % 5) * 0.02f;
  v3 eye = eye_of(e);
  int best = -1; float bd = 1e9f;
  for (int k = 0; k < 2; k++) {
    if (!T[k].alive) continue;
    v3 to = tchest(k), d = v3_sub(to, eye);
    float L = v3_len(d);
    if (L > range) continue;
    if (cone && !e->seen && L > 8) { v3 f = fwd(e->yaw); if (v3_dot(v3_norm(v3_(d.x, 0, d.z)), f) < 0.2f) continue; }
    float score = L * (k ? 1.35f : 1);
    if (score >= bd) continue;
    if (!world_visible(eye, to)) continue;
    best = k; bd = score;
  }
  e->los = best >= 0;
  if (e->los) { e->tgt = best; e->last_seen = T[best].pos; e->lost_t = 0; }
}

static int shooters_now(void) {
  int n = 0;
  for (int i = 0; i < MAXE; i++) if (E[i].used && !E[i].boss && (E[i].state == S_WIND || E[i].state == S_BURST)) n++;
  return n;
}

/* a spot near e behind something, seen from the target's chest: low line blocked, standing line open if possible */
static int cover_budget;
static bool find_cover(en_t *e, v3 *out) {
  if (cover_budget <= 0) return false;
  cover_budget--;
  v3 tc = tchest(e->tgt);
  float best = -1e9f; bool found = false;
  float base = ds_rr(0, 6.28f);
  for (int i = 0; i < 12; i++) {
    float a = base + i * (6.2832f / 12), r = i & 1 ? 4.5f : 2.5f;
    v3 c = v3_add(e->pos, v3_(cosf(a) * r, 0, sinf(a) * r));
    float fy = world_floor_y(v3_(c.x, e->pos.y + 0.6f, c.z));
    if (fabsf(fy - e->pos.y) > 0.4f) continue;
    c.y = fy;
    if (!world_visible(v3_(e->pos.x, e->pos.y + 0.6f, e->pos.z), v3_(c.x, c.y + 0.6f, c.z))) continue;
    bool low = !world_visible(tc, v3_(c.x, c.y + 0.55f, c.z));
    if (!low) continue;
    bool peek = world_visible(tc, v3_(c.x, c.y + 1.3f, c.z));
    float score = (peek ? 3 : 1) - r * 0.2f - fabsf(flat_dist(c, tc) - 16) * 0.05f;
    if (score > best) { best = score; *out = c; found = true; }
  }
  return found;
}

static v3 aim_dir(const en_t *e, v3 from, float spread) {
  v3 to = tchest(e->tgt);
  /* aim a little off with distance, so standing still is not a death sentence and moving is safe */
  float d = v3_len(v3_sub(to, from));
  to = v3_add(to, v3_(ds_rr(-1, 1) * (0.45f + d * 0.015f), ds_rr(-1, 1) * (0.3f + d * 0.01f), ds_rr(-1, 1) * (0.45f + d * 0.015f)));
  v3 dir = v3_norm(v3_sub(to, from));
  if (spread > 0) dir = v3_norm(v3_add(dir, v3_(ds_rr(-spread, spread), ds_rr(-spread, spread) * 0.5f, ds_rr(-spread, spread))));
  return dir;
}

static void telegraph(v3 at, float size, float k, uint32_t col) { fx_glow(at, size * (0.3f + 0.8f * k) * (0.9f + 0.1f * sinf(gtime * 40)), col); }

/* ── the rank and file: rifle, grenadier, veteran, para once landed ── */
static void trooper_update(en_t *e, float dt) {
  float range = e->type == EN_VETERAN ? SIGHT + 5 : SIGHT;
  sense(e, dt, range, e->state == S_PATROL || e->state == S_GUARD);
  e->cd -= dt; e->st -= dt; e->goal_t -= dt;
  v3 tp = e->seen ? (e->los ? T[e->tgt].pos : e->last_seen) : e->pos;
  float dist = flat_dist(e->pos, tp);
  float speed = INFO[e->type].speed;
  switch (e->state) {
  case S_GUARD: case S_PATROL: {
    if (e->los) {
      if (e->squad >= 0) squad_alert(e->squad, e->tgt, T[e->tgt].pos);
      else { e->state = S_ALERT; e->st = ds_rr(0.3f, 0.6f); e->seen = true; sfx_at(SFX_WHISTLE, e->pos.x, e->pos.y + 1, e->pos.z, 0.5f); }
      break;
    }
    if (e->state == S_PATROL && e->squad >= 0) {
      squad_t *q = &SQ[e->squad];
      v3 slot = squad_slot(q, e->slot);
      if (walk_to(e, slot, flat_dist(e->pos, slot) > 1.2f ? 2.4f : 1.45f, dt, 4)) turn_to(e, q->heading, 3, dt);
    } else walk(e, v3_(0, 0, 0), 0, dt);
    break; }
  case S_ALERT:
    walk(e, v3_(0, 0, 0), 0, dt);
    turn_to(e, yaw_to(e->pos, tp), 6, dt);
    if (e->st <= 0) { e->state = S_COMBAT; e->goal_t = 0; e->cd = fmaxf(e->cd, ds_rr(0.5f, 1.2f)); }
    break;
  case S_COMBAT: {
    if (!e->los) {
      e->lost_t += dt;
      if (e->lost_t > 8) { e->state = S_GUARD; e->seen = false; e->home = e->pos; break; }
    }
    if (e->goal_t <= 0 || e->stuck_t > 1.2f) {
      e->stuck_t = 0;
      v3 to = v3_norm(v3_(tp.x - e->pos.x, 0, tp.z - e->pos.z));
      v3 side = v3_(-to.z, 0, to.x);
      v3 c;
      if (e->type == EN_GRENADIER) {
        /* keeps 15 to 25 away */
        float want = dist < 15 ? -5 : dist > 25 ? 5 : 0;
        e->goal = v3_add(v3_add(e->pos, v3_scale(to, want)), v3_scale(side, ds_rr(-3, 3)));
        e->goal_t = ds_rr(2.5f, 4);
      } else if (e->type == EN_VETERAN) {
        /* flanks: a point out to the target's side */
        float a = yaw_to(tp, e->pos) + (e->slot & 1 ? 1 : -1) * ds_rr(0.6f, 1.2f);
        e->goal = v3_add(tp, v3_scale(fwd(a), ds_rr(10, 16)));
        e->goal_t = ds_rr(2, 3.5f);
      } else if (ds_rand() < 0.65f && find_cover(e, &c)) {
        e->goal = c; e->goal_t = ds_rr(3.5f, 6);
      } else {
        /* advance a few steps, or hold at a good range */
        float want = dist > 20 ? ds_rr(3, 6) : dist < 9 ? -3 : 0;
        e->goal = v3_add(v3_add(e->pos, v3_scale(to, want)), v3_scale(side, ds_rr(-2.5f, 2.5f)));
        e->goal_t = ds_rr(2, 3.5f);
      }
    }
    bool there = walk_to(e, e->goal, speed, dt, 5);
    if (there || e->los) turn_to(e, yaw_to(e->pos, tp), there ? 5 : 2, dt);
    if (e->los && e->cd <= 0 && dist < range && shooters_now() < SHOOTERS) {
      e->state = S_WIND; e->st = e->type == EN_GRENADIER ? 0.6f : 0.42f;
      e->atk = e->type == EN_GRENADIER ? A_THROW : A_SHOOT;
    }
    break; }
  case S_WIND: {
    walk(e, v3_(0, 0, 0), 0, dt);
    turn_to(e, yaw_to(e->pos, T[e->tgt].pos), 6, dt);
    float tot = e->atk == A_THROW ? 0.6f : 0.42f, k = 1 - ds_clamp(e->st / tot, 0, 1);
    if (e->atk == A_SHOOT) telegraph(e->muzzle, 0.7f, k, 0xffd36a);
    if (e->st <= 0) {
      if (e->atk == A_THROW) {
        v3 from = v3_add(e->pos, v3_(0, e->H * 0.95f, 0));
        v3 at = T[e->tgt].pos;
        at = v3_add(at, v3_(ds_rr(-1.5f, 1.5f), 0, ds_rr(-1.5f, 1.5f)));
        grenade(from, at, 0.9f + dist * 0.02f, 15);
        sfx_at(SFX_LIPO_THROW, from.x, from.y, from.z, 0.4f);
        e->cd = ds_rr(2.7f, 3.3f);
        if (ds_rand() < 0.3f) callout("GRENADE!");
      } else {
        float dmg = e->type == EN_VETERAN ? 8 : 6;
        v3 dir = aim_dir(e, e->muzzle, 0);
        pellet(e->muzzle, dir, e->type == EN_VETERAN ? 18 : 16, dmg);
        fx_muzzle(e->muzzle, dir);
        sfx_at(SFX_PELLET, e->muzzle.x, e->muzzle.y, e->muzzle.z, 0.7f);
        e->cd = e->type == EN_VETERAN ? ds_rr(0.8f, 1.0f) : ds_rr(1.05f, 1.35f);
      }
      e->state = S_COMBAT; e->st = 0.3f;
    }
    break; }
  case S_FLINCH:
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->st <= 0) { e->state = e->seen ? S_COMBAT : S_ALERT; e->st = 0.2f; e->goal_t = 0; }
    break;
  }
}

/* a paratrooper on the way down: drifts toward the fight, lands, fights as a rifleman */
static void para_drop(en_t *e, float dt) {
  float fy = world_floor_y(v3_(e->pos.x, e->pos.y + 0.2f, e->pos.z));
  float above = fy > -1e8f ? e->pos.y - fy : 10;
  /* 0.5 units a second for the last few units; faster than that from a shelf top so the fight comes to you */
  float fall = above > 4 ? fminf(2.4f, 0.5f + (above - 4) * 0.25f) : 0.5f;
  v3 tp = T[0].alive ? T[0].pos : e->home;
  v3 d = v3_(tp.x - e->pos.x, 0, tp.z - e->pos.z);
  float l = v3_len(d);
  v3 drift = l > 3 ? v3_scale(d, 0.7f / l) : v3_(0, 0, 0);
  drift = v3_add(drift, v3_(sinf(gtime * 0.7f + (float)(e - E)) * 0.4f, 0, cosf(gtime * 0.5f + (float)(e - E)) * 0.4f));
  e->vel = v3_(drift.x, -fall, drift.z);
  world_move(&e->pos, &e->vel, e->r, e->H, dt, &e->grounded);
  turn_to(e, yaw_to(e->pos, tp), 1.2f, dt);
  e->lean = 0.08f * sinf(gtime * 1.3f + (float)(e - E));
  if (e->grounded || above < 0.05f) {
    e->state = S_ALERT; e->st = 0.4f; e->seen = true; e->home = e->pos; e->last_seen = tp;
    sfx_at(SFX_LAND, e->pos.x, e->pos.y, e->pos.z, 0.4f);
  }
}

/* ── the gunner: dug in, tracks, bursts of six, reloads ── */
static void gunner_update(en_t *e, float dt) {
  sense(e, dt, SIGHT + 5, e->state == S_GUARD);
  e->cd -= dt; e->st -= dt;
  walk(e, v3_(0, 0, 0), 0, dt);
  v3 tp = e->los ? T[e->tgt].pos : e->last_seen;
  float want = e->seen ? yaw_to(e->pos, tp) : e->aim + sinf(gtime * 0.6f + (float)(e - E)) * 0.7f;
  switch (e->state) {
  case S_GUARD:
    turn_to(e, want, 1.2f, dt);
    if (e->los) { e->state = S_COMBAT; e->seen = true; e->cd = 0.8f; sfx_at(SFX_WHISTLE, e->pos.x, e->pos.y + 1, e->pos.z, 0.5f); callout("HOLD THE LINE!"); }
    break;
  case S_COMBAT: case S_ALERT: case S_FLINCH:
    if (e->state != S_COMBAT && e->st > 0) break;
    e->state = S_COMBAT;
    turn_to(e, want, 1.8f, dt);
    if (!e->los) { e->lost_t += dt; if (e->lost_t > 8) { e->state = S_GUARD; e->seen = false; } }
    if (e->los && e->cd <= 0 && fabsf(ds_wrap_pi(want - e->yaw)) < 0.2f && shooters_now() < SHOOTERS) { e->state = S_BURST; e->shots = 6; e->st = 0.35f; }
    break;
  case S_BURST:
    turn_to(e, want, 1.4f, dt);
    if (e->shots == 6) telegraph(e->muzzle, 0.8f, 1 - ds_clamp(e->st / 0.35f, 0, 1), 0xffd36a);
    if (e->st <= 0 && e->shots > 0) {
      v3 dir = aim_dir(e, e->muzzle, 0.04f);
      pellet(e->muzzle, dir, 20, 3);
      fx_muzzle(e->muzzle, dir);
      sfx_at(SFX_MG, e->muzzle.x, e->muzzle.y, e->muzzle.z, 0.6f);
      e->shots--; e->st = 0.11f;
    }
    if (e->shots <= 0 && e->st <= 0) { e->state = S_COMBAT; e->cd = 2; }
    break;
  }
}

/* ── bosses ── */
static int minions_alive(void) { int n = 0; for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].minion && E[i].state != S_DEAD) n++; return n; }

/* reinforcements: a squad marching in from somewhere on the target's surface, or paratroopers above it */
static void call_in(en_t *boss, bool paras, int count) {
  v3 tp = T[0].alive ? T[0].pos : boss->pos;
  for (int tries = 0; tries < 8; tries++) {
    float a = ds_rr(0, 6.28f), r = paras ? ds_rr(3, 7) : ds_rr(12, 18);
    v3 c = v3_add(tp, v3_(cosf(a) * r, 0, sinf(a) * r));
    if (c.x < 2 || c.x > WORLD_MAX_X - 2 || c.z < 2 || c.z > WORLD_MAX_Z - 2) continue;
    float fy = world_floor_y(v3_(c.x, tp.y + 1, c.z));
    if (fy < -1e8f || fabsf(fy - tp.y) > 1.5f) continue;
    c.y = fy;
    if (!paras && !world_visible(v3_(tp.x, tp.y + 1, tp.z), v3_(c.x, c.y + 1, c.z))) continue;
    for (int k = 0; k < count; k++) {
      int before = -1;
      for (int j = 0; j < MAXE; j++) if (!E[j].used) { before = j; break; }
      if (before < 0) return;
      v3 sp = v3_add(c, v3_(ds_rr(-1.5f, 1.5f), paras ? ds_rr(9, 13) : 0, ds_rr(-1.5f, 1.5f)));
      enemy_spawn(paras ? EN_PARA : EN_RIFLE, sp, yaw_to(sp, tp));
      en_t *m = &E[before];
      m->minion = true; m->seen = true; m->tgt = 0; m->last_seen = tp;
      if (!paras) { m->state = S_ALERT; m->st = ds_rr(0.3f, 0.8f); if (m->squad >= 0) SQ[m->squad].alert = true; }
      if (!paras) fx_poof(v3_add(sp, v3_(0, 0.6f, 0)), C_ARMY, 0.8f);
    }
    sfx_at(SFX_WHISTLE, c.x, c.y + 1, c.z, 1);
    return;
  }
}

static void boss_shoot(en_t *e, v3 mz, float spread_a, float speed, float dmg) {
  v3 to = tchest(e->tgt);
  float a = yaw_to(mz, to) + spread_a;
  float d = flat_dist(mz, to);
  v3 dir = v3_norm(v3_(sinf(a) * d, to.y - mz.y, cosf(a) * d));
  pellet(mz, dir, speed, dmg);
  fx_muzzle(mz, dir);
  sfx_at(SFX_PELLET, mz.x, mz.y, mz.z, 0.8f);
}

static void sergeant_update(en_t *e, float dt) {
  sense(e, dt, 70, false);
  e->cd -= dt; e->st -= dt;
  v3 tp = T[e->tgt].alive ? T[e->tgt].pos : e->last_seen;
  float dist = flat_dist(e->pos, tp);
  float rage = e->phase == 3 ? 1.3f : 1;
  v3 to = v3_norm(v3_(tp.x - e->pos.x, 0, tp.z - e->pos.z)), side = v3_(-to.z, 0, to.x);
  v3 mega = en_local(e, v3_(-0.25f * e->sc, 1.3f * e->sc, 0.6f * e->sc));
  switch (e->state) {
  case S_RISE:
    walk(e, v3_(0, 0, 0), 0, dt);
    e->st += 2 * dt;   /* st counts up while standing */
    if (e->a && e->a->cur && e->a->cur->time >= e->a->dur - 0.1f) {
      e->state = S_COMBAT; e->cd = 1.2f;
      shout("THE SERGEANT", "FALL IN! THIS IS A FORWARD BASE.");
    }
    break;
  case S_COMBAT: {
    turn_to(e, yaw_to(e->pos, tp), 2.4f * rage, dt);
    e->goal_t -= dt;
    if (e->goal_t <= 0) { e->goal = v3_add(e->home, v3_add(v3_scale(side, ds_rr(-6, 6)), v3_scale(to, ds_rr(-2, 3)))); e->goal_t = ds_rr(2, 4); }
    walk_to(e, e->goal, INFO[e->type].speed * rage, dt, 1.5f);
    turn_to(e, yaw_to(e->pos, tp), 3 * rage, dt);
    if (e->cd <= 0) {
      int minions = minions_alive();
      float r = ds_rand();
      if (e->phase >= 2 && r < 0.25f && dist < 22) e->atk = A_CHARGE;
      else if (r < 0.45f && minions < (e->phase == 1 ? 4 : 7)) e->atk = A_CALL;
      else if (dist < 8 && r < 0.75f) e->atk = A_SHOUT;
      else e->atk = A_SPREAD;
      e->state = S_WIND; e->shots = e->phase >= 2 ? 2 : 1;
      e->st = e->atk == A_CHARGE ? 0.7f : e->atk == A_SHOUT ? 0.8f : 0.55f;
      if (e->atk == A_SPREAD) actor_play(e->a, "Shoot_OneHanded", 1, true);
      else if (e->atk == A_CHARGE) actor_play(e->a, "Punch", 0.6f, true);
    }
    break; }
  case S_WIND: {
    walk(e, v3_(0, 0, 0), 0, dt);
    turn_to(e, yaw_to(e->pos, tp), 3, dt);
    float tot = e->atk == A_CHARGE ? 0.7f : e->atk == A_SHOUT ? 0.8f : 0.55f, k = 1 - ds_clamp(e->st / tot, 0, 1);
    if (e->atk == A_SHOUT || e->atk == A_CALL) telegraph(mega, 1.6f, k, 0xffd36a);
    if (e->atk == A_SHOUT) fx_ring_flat(v3_(e->pos.x, e->pos.y + 0.06f, e->pos.z), 9, 0xff4a30);
    if (e->atk == A_SPREAD) telegraph(e->muzzle, 1.0f, k, 0xffd36a);
    if (e->atk == A_CHARGE) telegraph(v3_add(e->pos, v3_(0, e->H * 0.5f, 0)), 2.4f, k, 0xff5a30);
    if (e->st > 0) break;
    if (e->atk == A_SPREAD) {
      for (int i = -1; i <= 1; i++) boss_shoot(e, e->muzzle, i * 0.16f, 17 * rage, 7);
      if (--e->shots > 0) { e->st = 0.45f; break; }
      e->cd = ds_rr(1.6f, 2.2f) / rage;
    } else if (e->atk == A_SHOUT) {
      shockwave(e->pos, 14, 9, 10, 0, 0xffb347);
      sfx_at(SFX_ALARM, mega.x, mega.y, mega.z, 0.9f);
      static const char *const L[] = {"DROP AND GIVE ME TWENTY!", "BACK IN LINE!", "WHO LEFT THIS BENCH MESSY?"};
      shout("THE SERGEANT", L[(int)(ds_rand() * 3) % 3]);
      e->cd = ds_rr(1.8f, 2.4f) / rage;
    } else if (e->atk == A_CALL) {
      bool paras = e->phase >= 2 && ds_rand() < 0.5f;
      call_in(e, paras, paras ? 3 : 3 + (e->phase == 3));
      shout("THE SERGEANT", paras ? "PARATROOPERS. GO, GO, GO!" : "SQUAD, FALL IN!");
      sfx_at(SFX_ALARM, mega.x, mega.y, mega.z, 0.7f);
      e->cd = ds_rr(2.4f, 3.2f) / rage;
    } else if (e->atk == A_CHARGE) {
      e->state = S_CHARGE; e->st = 1.5f; e->aim = yaw_to(e->pos, tp); e->melee_done = false;
      actor_play(e->a, "Walk", 2.2f, true);
      break;
    }
    e->state = S_COMBAT;
    break; }
  case S_CHARGE: {
    v3 f = fwd(e->aim);
    turn_to(e, e->aim, 4, dt);
    walk(e, f, 9 * rage, dt);
    e->melee_on = true;
    v3 hitp = en_local(e, v3_(0, e->H * 0.5f, e->r + 0.4f));
    for (int k = 0; k < 2 && !e->melee_done; k++) if (T[k].alive && cyl_sphere(T[k].pos, T[k].r, T[k].h, hitp, 1.1f)) { hurt(k, 15, e->pos); e->melee_done = true; }
    fx_beam(v3_add(e->pos, v3_(0, 0.3f, 0)), v3_add(v3_sub(e->pos, v3_scale(f, 2)), v3_(0, 0.3f, 0)), 0xd8c9a0, 0.3f);
    if (e->st <= 0 || e->melee_done || !edge_ok(e, f)) { e->state = S_STAGGER; e->st = 0.6f; e->melee_on = false; e->cd = ds_rr(1.5f, 2.2f); }
    break; }
  case S_STAGGER:
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->st <= 0) { e->state = S_COMBAT; e->cd = fmaxf(e->cd, 0.8f); }
    break;
  }
}

static void general_update(en_t *e, float dt) {
  sense(e, dt, 120, false);
  e->cd -= dt; e->st -= dt;
  v3 tp = T[e->tgt].alive ? T[e->tgt].pos : e->last_seen;
  float dist = flat_dist(e->pos, tp);
  float rage = e->phase == 3 ? 1.3f : e->phase == 2 ? 1.12f : 1;
  v3 chestp = en_local(e, v3_(0, e->H * 0.62f, e->r * 0.6f));
  e->bob = ds_damp(e->bob, 0, 8, dt);
  switch (e->state) {
  case S_RISE:
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->a && e->a->cur && e->a->cur->time >= e->a->dur - 0.1f) {
      e->state = S_COMBAT; e->cd = 1.5f;
      shout("THE GENERAL", "THIS IS MY BENCH NOW.");
    }
    break;
  case S_COMBAT: {
    e->goal_t -= dt;
    if (e->goal_t <= 0) {
      v3 to = v3_norm(v3_(tp.x - e->pos.x, 0, tp.z - e->pos.z));
      e->goal = v3_add(e->home, v3_add(v3_scale(v3_(-to.z, 0, to.x), ds_rr(-14, 14)), v3_scale(to, ds_rr(-4, 8))));
      e->goal_t = ds_rr(3, 5);
    }
    walk_to(e, e->goal, INFO[e->type].speed * rage, dt, 0.8f);
    turn_to(e, yaw_to(e->pos, tp), 1.0f * rage, dt);
    if (e->cd <= 0) {
      float r = ds_rand();
      int minions = minions_alive();
      if (e->phase >= 2 && r < 0.3f && minions < 6) e->atk = A_CALL;
      else if (dist < 20 && r < 0.65f) e->atk = A_STOMP;
      else e->atk = A_SWEEP;
      e->state = S_WIND;
      e->st = e->atk == A_STOMP ? 0.9f : e->atk == A_SWEEP ? 0.7f : 0.6f;
      if (e->atk == A_SWEEP) actor_play(e->a, "Shoot_OneHanded", 0.7f, true);
      else if (e->atk == A_CALL) actor_play(e->a, "Victory", 1, true);
      static const char *const L[] = {"HOLD THE LINE.", "PULL MY STRING.", "BATTERIES NOT INCLUDED.", "THIS IS MY BENCH NOW."};
      if (ds_rand() < 0.3f) shout("THE GENERAL", L[(int)(ds_rand() * 4) % 4]);
    }
    break; }
  case S_WIND: {
    walk(e, v3_(0, 0, 0), 0, dt);
    float tot = e->atk == A_STOMP ? 0.9f : e->atk == A_SWEEP ? 0.7f : 0.6f, k = 1 - ds_clamp(e->st / tot, 0, 1);
    if (e->atk == A_STOMP) {
      /* rises on his toes, then the slam */
      e->bob = 0.9f * sinf(k * DS_PI * 0.5f);
      fx_ring_flat(v3_(e->pos.x, e->pos.y + 0.06f, e->pos.z), 6 + 24 * k, 0xff4a30);
    } else {
      turn_to(e, yaw_to(e->pos, tp), 1.2f, dt);
      telegraph(chestp, 3.5f, k, e->atk == A_SWEEP ? 0xffd36a : 0x9be35a);
    }
    if (e->st > 0) break;
    if (e->atk == A_STOMP) {
      e->bob = -0.2f;
      shockwave(e->pos, 15 * rage, 30, 18, 0, 0xffb347);
      if (e->phase == 3) shockwave(e->pos, 15 * rage, 30, 18, 0.6f, 0xffb347);
      fx_explosion(v3_add(e->pos, v3_(0, 0.3f, 0)), 2.5f);
      sfx_at(SFX_BOOM, e->pos.x, e->pos.y, e->pos.z, 1);
      e->state = S_STAGGER; e->st = 0.8f; e->cd = ds_rr(1.8f, 2.6f) / rage;
    } else if (e->atk == A_SWEEP) {
      e->state = S_BURST; e->shots = e->phase == 3 ? 22 : 16; e->st = 0; e->aim = yaw_to(e->pos, tp) - 0.7f;
      e->wave_t = ds_rand() < 0.5f ? 1 : -1;
      if (e->wave_t < 0) e->aim += 1.4f;
    } else {
      call_in(e, ds_rand() < 0.4f, 4);
      e->state = S_STAGGER; e->st = 0.6f; e->cd = ds_rr(2, 3) / rage;
    }
    break; }
  case S_BURST: {
    /* a sweeping stream of pellets across the target: jump it or dash through a gap */
    int n = e->phase == 3 ? 22 : 16;
    if (e->st <= 0 && e->shots > 0) {
      float k = (float)(n - e->shots) / (float)(n - 1);
      float a = e->aim + e->wave_t * k * 1.4f;
      v3 to = tchest(e->tgt);
      float d = fmaxf(4, flat_dist(chestp, to));
      v3 dir = v3_norm(v3_(sinf(a) * d, to.y - chestp.y, cosf(a) * d));
      pellet(chestp, dir, 19 * rage, 5);
      fx_muzzle(chestp, dir);
      sfx_at(SFX_MG, chestp.x, chestp.y, chestp.z, 0.8f);
      e->shots--; e->st = 0.1f;
    }
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->shots <= 0) { e->state = S_STAGGER; e->st = 0.7f; e->cd = ds_rr(1.6f, 2.4f) / rage; }
    break; }
  case S_STAGGER:
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->st <= 0) e->state = S_COMBAT;
    break;
  }
}

/* the General's weak points ride his bones: the box on his back, the hatch below it, both knees */
static void general_parts(en_t *e) {
  actor_t *a = e->a;
  if (!a) return;
  v3 f = fwd(e->yaw);
  v3 torso = bone_pos(a->b_torso, v3_add(e->pos, v3_(0, e->H * 0.6f, 0)));
  v3 abd = bone_pos(a->b_abd, v3_add(e->pos, v3_(0, e->H * 0.5f, 0)));
  v3 box = v3_sub(v3_add(torso, v3_(0, e->H * 0.02f, 0)), v3_scale(f, e->H * 0.11f));
  if (e->prop) {
    t3_object_set_position(e->prop, box.x, box.y, box.z);
    t3_object_set_rotation(e->prop, 0, e->yaw, 0);
    t3_object_set_scale(e->prop, e->sc, e->sc, e->sc);
    ((t3_object *)e->prop)->visible = true;
  }
  if (e->nwp < 4) return;
  e->wp[0].p = v3_sub(box, v3_scale(f, e->H * 0.05f));
  e->wp[1].p = v3_sub(v3_add(abd, v3_(0, -e->H * 0.03f, 0)), v3_scale(f, e->H * 0.1f));
  for (int k = 0; k < 2; k++) e->wp[2 + k].p = v3_add(bone_pos(a->b_knee[k], v3_add(e->pos, v3_(0, e->H * 0.27f, 0))), v3_scale(f, e->H * 0.05f));
  for (int i = 0; i < e->nwp; i++) {
    wp_t *w = &e->wp[i];
    if (!w->mesh) continue;
    bool lit = w->alive && e->state != S_DEAD && e->state != S_DORMANT;
    ((t3_object *)w->mesh)->visible = lit;
    if (!lit) continue;
    t3_object_set_position(w->mesh, w->p.x, w->p.y, w->p.z);
    float pulse = 0.6f + 0.4f * sinf(gtime * 6 + i * 1.7f);
    w->mesh->materials[0]->color = (t3_color){1.0f, 0.4f + 0.3f * pulse, 0.08f}; w->mesh->materials[0]->version++;
    fx_glow(w->p, w->r * (2.6f + 1.2f * pulse), 0xff8a2a);
  }
}

/* ── the frame ── */
static void set_tint(en_t *e) {
  actor_t *a = e->a;
  if (!a || !a->mat) return;
  /* the hit flash: a bright tick on an army man, softer on a boss who is hit all fight long */
  float k = (e->flash > 0 ? e->flash / FLASH_T : 0) * (e->boss ? 0.5f : 1);
  t3_color c = t3_color_hex(e->col);
  if (e->melted) { c.r *= 0.55f; c.g *= 0.5f; c.b *= 0.45f; }
  float glue = e->burn_t > 0 ? 0.25f : 0;
  float wind = (e->state == S_WIND && e->boss) ? 0.15f + 0.1f * sinf(gtime * 30) : 0;
  t3_color em = {k * 0.45f + glue * 0.96f + wind, k * 0.45f + glue * 0.9f + wind * 0.5f, k * 0.4f + glue * 0.66f};
  if (e->state == S_DORMANT) c = (t3_color){c.r * 0.5f, c.g * 0.5f, c.b * 0.5f};
  a->mat->color = c; a->mat->emissive = em; a->mat->emissive_intensity = 1; a->mat->version++;
}

static void choose_clip(en_t *e) {
  actor_t *a = e->a;
  if (!a || e->state == S_DEAD || e->state == S_DORMANT || e->state == S_RISE) return;
  float sp = hypotf(e->vel.x, e->vel.z);
  if (e->state == S_FLINCH) return;   /* RecieveHit was started by the hit */
  if (e->state == S_STAGGER && e->boss) return;
  if (e->state == S_CHARGE) return;
  if (e->state == S_WIND || e->state == S_BURST) {
    if (e->atk == A_THROW) actor_play(a, "Punch", 1.2f, false);
    else if (e->atk == A_STOMP || e->atk == A_CALL || e->atk == A_SHOUT) { if (e->atk == A_SHOUT) actor_play(a, "Victory", 1.3f, false); }
    else actor_play(a, "Shoot_OneHanded", e->type == EN_GENERAL ? 0.7f : 1.4f, false);
    return;
  }
  if (e->state == S_DROP) { actor_play(a, "Idle", 0.6f, false); return; }
  float ref = 1.4f * e->sc;
  if (sp > 0.25f * e->sc) actor_play(a, "Walk", ds_clamp(sp / ref, 0.6f, 2.2f), false);
  else actor_play(a, "Idle", 1, false);
  /* marching in step: every walker of a squad on the same beat */
  if (e->state == S_PATROL && e->squad >= 0 && a->cur && !strcmp(a->name, "Walk") && a->dur > 0) {
    float ph = fmodf(SQ[e->squad].clock * ds_clamp(sp / ref, 0.6f, 2.2f), a->dur);
    a->cur->time = ph;
  }
}

/* the rifle in the hand: aimed down the shot when shooting, at the slope when marching */
static void place_kit(en_t *e) {
  actor_t *a = e->a;
  v3 s = v3_(e->sc, e->sc, e->sc);
  v3 hand = bone_pos(a->b_hand, en_local(e, v3_(0.25f * e->sc, e->H * 0.55f, 0.2f * e->sc)));
  float pitch, yaw = e->yaw;
  bool aiming = e->state == S_WIND || e->state == S_BURST || (e->state == S_COMBAT && e->los && e->cd < 0.5f);
  uint32_t rc = e->melted ? 0x2a3a1a : e->col;
  if (e->type == EN_GENERAL || e->type == EN_SERGEANT) aiming = aiming && e->atk != A_SHOUT && e->atk != A_CALL;
  if (e->state == S_DEAD) {
    /* dropped beside him */
    v3 g = en_local(e, v3_(0.45f * e->sc, 0.03f * e->sc, 0.2f * e->sc));
    kit_put(&K_RIFLE, g, e->yaw + 1.3f, 0, s, rc);
  } else {
    if (aiming && T[e->tgt].alive) {
      v3 d = v3_sub(tchest(e->tgt), hand);
      pitch = atan2f(d.y, hypotf(d.x, d.z));
      yaw = atan2f(d.x, d.z);
    } else pitch = e->state == S_DROP ? 1.2f : 0.85f;
    if (e->type == EN_SERGEANT && e->prop) {
      /* the megaphone in the other hand */
      v3 lh = bone_pos(a->b_lhand, en_local(e, v3_(-0.25f * e->sc, e->H * 0.55f, 0.2f * e->sc)));
      bool up = e->state == S_WIND && (e->atk == A_SHOUT || e->atk == A_CALL);
      t3_object_set_position(e->prop, lh.x, lh.y, lh.z);
      t3_object_set_rotation(e->prop, up ? 0.1f : 1.1f, e->yaw, 0);
      t3_object_set_scale(e->prop, e->sc, e->sc, e->sc);
    }
    kit_put(&K_RIFLE, hand, yaw, pitch, s, rc);
    float c = cosf(pitch);
    v3 dir = v3_(sinf(yaw) * c, sinf(pitch), cosf(yaw) * c);
    e->muzzle = v3_add(v3_add(hand, v3_scale(dir, 0.56f * e->sc)), v3_(0, 0.04f * e->sc, 0));
  }
  if (e->chute > 0.01f) {
    float k = e->chute;
    kit_put(&K_CHUTE, v3_(e->pos.x, e->pos.y - (1 - k) * 1.2f, e->pos.z), e->yaw, e->lean, v3_(1.0f + (1 - k) * 0.4f, k, 1.0f + (1 - k) * 0.4f), 0xe8e4d4);
  }
  if (e->type == EN_GUNNER && e->state != S_DEAD) kit_put(&K_BAGS, e->home, e->aim, 0, v3_(1, 1, 1), 0x8a7a50);
}

#ifdef SP_TEST_ARMY
/* test scenes round the laser corner start (EXTRA_CFLAGS=-DSP_TEST_ARMY): 1 a mixed platoon, 2 the Sergeant, 3 the General,
 * 4 a blind squad marching past for a look at the models, 5 blind targets in melee range, 6 a rifle squad on the lid */
static float test_wake = -1;
static void test_spawn(void) {
  test_spawned = true;
#if SP_TEST_ARMY == 2
  enemy_spawn(EN_SERGEANT, v3_(522.5f, 19.4f, 13.8f), DS_PI / 2);
  test_wake = 1.0f;
#elif SP_TEST_ARMY == 3
  enemy_spawn(EN_GENERAL, v3_(521.5f, 19.4f, 13.8f), DS_PI / 2);
  test_wake = 1.0f;
#elif SP_TEST_ARMY == 6
  /* a rifle squad on the lid that can see the start: cover and advancing */
  for (int i = 0; i < 4; i++) enemy_spawn(EN_RIFLE, v3_(522 + (i & 1) * 1.2f, 19.4f, 9 + (i >> 1) * 1.3f), DS_PI / 2);
#elif SP_TEST_ARMY == 5
  /* melee range: two riflemen right in front of the start */
  enemy_spawn(EN_PARA, v3_(529.0f, 19.4f, 13.4f), DS_PI / 2);
  enemy_spawn(EN_PARA, v3_(529.2f, 19.4f, 14.5f), DS_PI / 2);
  enemy_spawn(EN_PARA, v3_(524.0f, 19.4f, 13.4f), DS_PI / 2);
#elif SP_TEST_ARMY == 4
  /* a look at the models: a squad marching across in front of the start, a gunner, a paratrooper; the army is blind */
  for (int i = 0; i < 5; i++) enemy_spawn(EN_RIFLE, v3_(527.6f + (i & 1) * 0.8f, 19.4f, 7.5f + i * 0.5f), 0);
  enemy_spawn(EN_GUNNER, v3_(524.5f, 19.4f, 18.5f), DS_PI / 2);
  enemy_spawn(EN_PARA, v3_(526, 23, 15.5f), DS_PI / 2);
  enemy_spawn(EN_PARA, v3_(527.8f, 19.4f, 12.6f), DS_PI / 2);   /* standing, facing the camera */
  enemy_spawn(EN_PARA, v3_(527.8f, 19.4f, 15.0f), -DS_PI / 2);  /* and facing away */
#else
  for (int i = 0; i < 4; i++) enemy_spawn(EN_RIFLE, v3_(500 + (i & 1) * 1.4f, 0, 34 + (i >> 1) * 1.4f), DS_PI * 0.5f);
  for (int i = 0; i < 3; i++) enemy_spawn(EN_RIFLE, v3_(505 + i * 1.2f, 0, 48), DS_PI);
  enemy_spawn(EN_GRENADIER, v3_(498, 0, 46), DS_PI * 0.5f);
  for (int i = 0; i < 3; i++) enemy_spawn(EN_VETERAN, v3_(522.5f, 19.4f, 9 + i * 1.3f), DS_PI * 0.5f);
  enemy_spawn(EN_GUNNER, v3_(523, 19.4f, 19.5f), DS_PI * 0.5f);
  for (int i = 0; i < 2; i++) enemy_spawn(EN_PARA, v3_(515 + i * 3, 14, 24 + i * 2), 0);
#endif
}
#endif

void enemy_update(float dt, const enemy_target_t *player, const enemy_target_t *buddy) {
  gtime += dt;
  callout_cd -= dt;
  T[0] = player ? *player : (enemy_target_t){{0, 0, 0}, 0.4f, 1.8f, false};
  T[1] = buddy ? *buddy : (enemy_target_t){{0, 0, 0}, 0.4f, 1.8f, false};
#ifdef SP_TEST_ARMY
  if (!test_spawned && T[0].alive) test_spawn();
#if SP_TEST_ARMY == 4 || SP_TEST_ARMY == 5
  T[0].alive = T[1].alive = false;
#endif
  if (test_wake > 0 && (test_wake -= dt) <= 0) enemy_boss_wake();
#endif
  for (int s = 0; s < MAXQ; s++) SQ[s].awake = false;
  int builds = 0;
  cover_budget = 3;
  v3 pp = T[0].pos;
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!e->used) continue;
    float pd = flat_dist(e->pos, pp);
    bool keep = e->boss || e->minion || e->seen;
    if (!e->a) {
      if ((pd < STREAM_R || e->boss) && actors_out < MAX_ACTORS && builds < BUILD_PER_FRAME) { builds++; attach_model(e); }
      if (!e->a) continue;
    }
    if (!keep && e->state != S_DEAD && pd > SLEEP_R) {   /* far and unalerted: asleep */
      e->a->root->visible = false;
      if (pd > DROP_R) { actor_put(e->a); e->a = NULL; }
      continue;
    }
    e->a->root->visible = true;
    if (e->squad >= 0) SQ[e->squad].awake = true;
    e->flash = fmaxf(0, e->flash - dt); e->hit_cd -= dt; e->dodge_cd -= dt;
    e->lean = ds_damp(e->lean, 0, 6, dt);
    e->stag = fmaxf(0, e->stag - dt * 6);
    if (e->slow_t > 0) { e->slow_t -= dt; if (e->slow_t <= 0) e->slow = 1; }
    if (e->burn_t > 0 && e->state != S_DEAD) {
      /* hot glue: 6 a second for two seconds */
      e->burn_t -= dt;
      e->hp -= 6 * dt;
      if (((int)(gtime * 8) & 1) && ((int)((gtime - dt) * 8) & 1) == 0) fx_glue(v3_add(e->pos, v3_(0, e->H * 0.6f, 0)), 1);
      if (e->hp <= 0) { e->hp = 0; kill(e, WP_GLUE); }
    }
    if (e->chute > 0 && e->state != S_DROP) e->chute = fmaxf(0, e->chute - dt * 1.6f);
    bool dead = e->state == S_DEAD;
    if (dead) {
      e->dead_t += dt;
      walk(e, v3_(0, 0, 0), 0, dt);
      if (e->boss) {
        if (e->dead_t < 3 && ((int)(e->dead_t * 5) != (int)((e->dead_t - dt) * 5)))
          fx_sparks(en_local(e, v3_(ds_rr(-0.2f, 0.2f) * e->H, ds_rr(0.05f, 0.4f) * e->H, ds_rr(-0.3f, 0.3f) * e->H)), v3_(0, 1, 0), 0xffb347, 6);
      } else if (e->dead_t > 3.4f) {
        /* back to dust: a puff of plastic */
        fx_poof(v3_add(e->pos, v3_(0, 0.35f, 0)), e->melted ? 0x6a6a50 : e->col, 0.9f);
        en_free(e);
        continue;
      }
    } else {
      switch (e->type) {
      case EN_RIFLE: case EN_GRENADIER: case EN_VETERAN: trooper_update(e, dt); break;
      case EN_PARA: if (e->state == S_DROP) para_drop(e, dt); else trooper_update(e, dt); break;
      case EN_GUNNER: gunner_update(e, dt); break;
      case EN_SERGEANT: if (e->state != S_DORMANT) sergeant_update(e, dt); else walk(e, v3_(0, 0, 0), 0, dt); break;
      case EN_GENERAL: if (e->state != S_DORMANT) general_update(e, dt); else walk(e, v3_(0, 0, 0), 0, dt); break;
      }
    }
    /* pose */
    choose_clip(e);
    set_tint(e);
    actor_update(e->a, dt * (e->state == S_DEAD ? 1 : slow_k(e)), dead || e->state == S_RISE);
    float s = e->H / SOLDIER_H;
    if (e->melted && dead) t3_object_set_scale(e->a->model, s * 1.15f, s * ds_lerp(1, 0.45f, ds_clamp(e->dead_t * 3, 0, 1)), s * 1.15f);
    t3_object_set_position(e->a->root, e->pos.x, e->pos.y + e->bob * (e->type == EN_GENERAL ? 1 : 0), e->pos.z);
    t3_object_set_rotation(e->a->root, -e->lean * 0.7f, e->yaw, 0);
    t3_object_update_matrix_world(e->a->root, true);
    place_kit(e);
    if (e->type == EN_GENERAL) general_parts(e);
  }
  squads_update(dt);
  proj_update(dt);
  for (int i = 0; i < MAXP; i++) if (PJ[i].on) {
    float d = PJ[i].kind == PJ_GRENADE ? 0.28f : 0.16f;
    kit_put(&K_PELLET, PJ[i].p, 0, 0, v3_(d, d, d), PJ[i].col);
  }
  kit_flush(&K_RIFLE); kit_flush(&K_CHUTE); kit_flush(&K_BAGS); kit_flush(&K_PELLET);
}
