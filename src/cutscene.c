/* cutscene.c: the director. A cutscene is a list of lines (the script, typed
 * out as captions and spoken) and a list of shots whose cuts are keyed to
 * those lines; each cutscene also has an act function that poses Moheeb,
 * Luis and the staged cast from the clock. Everything is staged in the lab
 * itself: the camera, the two heroes (game.c draws them from cine_view_t),
 * a cast of plastic army men, the Sergeant and the General loaded from
 * soldier.glb, and a few props (the SCALE button, a drone, the breaker
 * handle, the note on the laser, a printer's screen). The army the level
 * spawned is hidden while a scene runs and handed back after.
 * The death screen quips are at the bottom. Every line follows docs/STORY.md. */
#include "cutscene.h"
#include "audio.h"
#include "world.h"
#include "mission.h"
#include "fx.h"
#include "voice.h"
#include "cv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAXL 16
#define MAXS 20
#define TYPE_CPS 45.f            /* captions type out at about 45 characters per second */
#define SKIP_HOLD 0.8f
#define ARMY_H 1.5f              /* an army man, in game units */
#define NCAST 12

typedef struct { const char *who, *text, *spoken; float pre, t0, dur; int n; } line_t;
/* a shot starts at line `line` plus `off` (line < 0: `off` from the start). Its camera runs
 * p0,a0,f0 to p1,a1,f1 until the next shot starts; coordinates are in the cutscene's frame */
typedef struct { int line; float off, s0; v3 p0, p1, a0, a1; float f0, f1, shake; int ease; } shot_t;
typedef void (*act_fn)(float t, float dt, cine_view_t *o);

static struct {
  bool active, armed;
  cut_id id;
  hero_id player;
  float t, end, skip, tail, fi, fo;
  line_t L[MAXL]; int nl;
  shot_t S[MAXS]; int ns;
  v3 org; float yaw;           /* the cutscene's frame: W() maps it to the lab */
  act_fn act;
  bool ray;                    /* keep the camera in front of the furniture */
  int last_line, last_chars, prep;
} C;

static t3_scene *scn;
static t3_object *root;        /* everything the cutscenes own */
static v3 hint_player; static bool have_hint;   /* where the player stood when an in-level scene began */
static bool cued[12];
static void cue(int i, float t, float at, sfx_t s, float gain) { if (!cued[i] && t >= at) { cued[i] = true; sfx(s, 0, gain); } }

static float sm(float x) { x = ds_clamp(x, 0, 1); return x * x * (3 - 2 * x); }
static float seg(float t, float a, float b) { return ds_clamp((t - a) / (b - a), 0, 1); }
static v3 W(v3 p) {
  float c = cosf(C.yaw), s = sinf(C.yaw);
  return v3_(C.org.x + p.x * c + p.z * s, C.org.y + p.y, C.org.z - p.x * s + p.z * c);
}
static float WY(float y) { return C.yaw + y; }
static v3 pv(float x, float y, float z) { return v3_(x, y, z); }
static float face(v3 from, v3 to) { return atan2f(to.x - from.x, to.z - from.z); }
/* the lab's surface under p, searched from just above it */
static float ground(v3 p, float above) { float y = world_floor_y(v3_(p.x, p.y + above, p.z)); return y > -1e8f ? y : p.y; }
static v3 lm_c(const lab_box_t *b) { return v3_((b->min[0] + b->max[0]) * 0.5f, b->max[1], (b->min[2] + b->max[2]) * 0.5f); }

/* ── the script ── */
static void cut_reset(void) {
  C.ray = false; C.nl = C.ns = 0; C.tail = 2; C.fi = 1.0f; C.fo = 1.1f;
  C.org = v3_(0, 0, 0); C.yaw = 0;
}
static int say(const char *who, const char *text, float pre) {
  line_t *l = &C.L[C.nl];
  l->who = who; l->text = text; l->spoken = NULL; l->pre = pre; l->n = (int)strlen(text);
  return C.nl++;
}
/* a line whose caption and speech differ ("1:1" is read "one to one") */
static int say_as(const char *who, const char *text, const char *spoken, float pre) {
  int i = say(who, text, pre); C.L[i].spoken = spoken; return i;
}
static const char *spoken(const line_t *l) { return l->spoken ? l->spoken : l->text; }
static void shot(int line, float off, v3 p0, v3 a0, float f0, v3 p1, v3 a1, float f1, float shake, int ease) {
  shot_t *s = &C.S[C.ns++];
  s->line = line; s->off = off; s->p0 = p0; s->a0 = a0; s->f0 = f0; s->p1 = p1; s->a1 = a1; s->f1 = f1; s->shake = shake; s->ease = ease;
}
static void hold(int line, float off, v3 p, v3 a, float f) { shot(line, off, p, a, f, p, a, f, 0.02f, 0); }
static float lt0(int i) { return C.L[i].t0; }
static float lend(int i) { return C.L[i].t0 + C.L[i].dur; }
static void finish_lines(void) {
  float t = 0;
  for (int i = 0; i < C.nl; i++) {
    line_t *l = &C.L[i];
    t += l->pre; l->t0 = t;
    l->dur = (float)l->n / TYPE_CPS + 1.0f + 0.03f * (float)l->n;   /* typing, then time to read it */
    t += l->dur;
  }
  C.end = t + C.tail;
}
static void resolve_shots(void) {
  for (int i = 0; i < C.ns; i++) C.S[i].s0 = C.S[i].line >= 0 ? C.L[C.S[i].line].t0 + C.S[i].off : C.S[i].off;
}

/* ═════════════ the kit: props from boxes, cylinders and painted quads ═════════════ */
static t3_material *mat(uint32_t c, float rough, float emis) {
  t3_material *m = t3_mesh_standard_material_new(c);
  m->roughness = rough; m->metalness = 0;
  if (emis > 0) { m->emissive = t3_color_hex(c); m->emissive_intensity = emis; }
  return m;
}
static t3_mesh *put(t3_object *par, t3_geometry *g, t3_material *m, float x, float y, float z) {
  t3_mesh *me = t3_mesh_new(g, m);
  t3_release(g); t3_release(m);
  T3_OBJ(me)->frustum_culled = false;
  t3_object_set_position(me, x, y, z);
  t3_object_add(par, me); t3_release(me);
  return me;
}
static t3_mesh *bx(t3_object *p, float w, float h, float d, float x, float y, float z, uint32_t c) { return put(p, t3_box_geometry_new(w, h, d, 1, 1, 1), mat(c, 0.7f, 0), x, y, z); }
static t3_mesh *cy(t3_object *p, float r0, float r1, float h, float x, float y, float z, uint32_t c, float e) { return put(p, t3_cylinder_geometry_new(r0, r1, h, 10, 1, false), mat(c, 0.55f, e), x, y, z); }
static t3_object *group(t3_object *par) { t3_object *g = t3_object_new(); t3_object_add(par, g); t3_release(g); return g; }
/* a texture painted with the canvas API, on an unlit quad facing +z */
static t3_mesh *painted(t3_object *par, int w, int h, void (*fn)(cv *, int, int), float qw, float qh) {
  cv *c = cv_new(w, h);
  fn(c, w, h);
  uint8_t *px = malloc((size_t)w * h * 4);
  cv_read(c, px);
  cv_free(c);
  t3_texture *tx = t3_texture_new(w, h, px);
  free(px);
  tx->color_space = T3_SRGB_COLOR_SPACE;
  tx->mag_filter = T3_NEAREST;
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(m, tx); t3_release(tx);
  return put(par, t3_plane_geometry_new(qw, qh, 1, 1), m, 0, 0, 0);
}
static void tx_scale(cv *c, int w, int h) {
  cv_fill_hex(c, 0x1a1c22, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0xffd23a, 1); cv_fill_rect(c, 3, 3, (float)w - 6, (float)h - 6);
  cv_fill_hex(c, 0x1a1c22, 1); cv_fill_rect(c, 6, 6, (float)w - 12, (float)h - 12);
  cv_font(c, F_DISPLAY, 26); cv_letter_spacing(c, 3); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_hex(c, 0xffd23a, 1); cv_fill_text(c, "SCALE", w * 0.5f, h * 0.5f + 1);
}
static void tx_note(cv *c, int w, int h) {
  cv_fill_hex(c, 0xf4f0e4, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0xc8d8f0, 1);
  for (int y = 30; y < h; y += 22) cv_fill_rect(c, 0, (float)y, (float)w, 1.5f);
  cv_font(c, F_DISPLAY, 21); cv_letter_spacing(c, 1); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_ALPHABETIC);
  cv_fill_hex(c, 0x1a2a6a, 1);
  cv_fill_text(c, "PLEASE RETURN", w * 0.5f, 46);
  cv_fill_text(c, "THE SCALE TO 1:1", w * 0.5f, 74);
  cv_fill_text(c, "WHEN YOU ARE DONE", w * 0.5f, 102);
  cv_font(c, F_TYPE, 15); cv_fill_text(c, "THANKS. THE LAB", w * 0.5f, 126);
  cv_fill_hex(c, 0xe8e0b0, 0.8f); cv_fill_rect(c, w * 0.5f - 30, 0, 60, 12);   /* the tape */
}
static void tx_screen62(cv *c, int w, int h) {
  cv_fill_hex(c, 0x0a1830, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0x3aa8ff, 1); cv_fill_rect(c, 2, 2, (float)w - 4, 2);
  cv_font(c, F_MONO, 15); cv_letter_spacing(c, 0); cv_text_align(c, CV_LEFT); cv_text_baseline(c, CV_ALPHABETIC);
  cv_fill_hex(c, 0xf2ead8, 1); cv_fill_text(c, "ARMY_MEN_x500", 6, 24);
  cv_font(c, F_MONO, 22);
  cv_fill_hex(c, 0xff7a1a, 1); cv_fill_text(c, "62%", 6, 50);
  cv_fill_hex(c, 0x1c2a44, 1); cv_fill_rect(c, 56, 38, (float)w - 64, 10);
  cv_fill_hex(c, 0xff7a1a, 1); cv_fill_rect(c, 56, 38, ((float)w - 64) * 0.62f, 10);
}

/* the note's place: on the laser's front face, found by a ray from the floor side */
static v3 note_at(void) {
  const lab_box_t *lid = &LM_LASER_LID[0];
  v3 o = v3_(lm_c(lid).x - 3, ground(lm_c(lid), 2) - 4.5f, lid->max[2] + 12);
  float t; v3 n;
  if (world_raycast(o, v3_(0, 0, -1), 20, &t, &n)) o.z -= t - 0.08f;
  else o.z = lid->max[2] + 0.12f;
  return o;
}

/* the props, each a group shown only by the scene that wants it */
static struct {
  t3_object *button, *drone, *motors[4], *breaker, *handle, *note, *screen;
  t3_object *megaphone, *pullbox;
} PROP;

static void props_build(void) {
  const lab_box_t *lid = &LM_LASER_LID[0];
  float ly = ground(lm_c(lid), 2);
  /* the laser's control panel: a SCALE button someone added to the firmware */
  PROP.button = group(root);
  t3_object_set_position(PROP.button, lid->max[0] - 2.4f, ly, lid->max[2] - 1.2f);
  bx(PROP.button, 3.4f, 0.3f, 1.6f, 0, 0.15f, 0, 0x2b2f36);
  cy(PROP.button, 0.36f, 0.4f, 0.32f, 0.9f, 0.45f, 0, 0xe8433a, 0.6f);
  t3_mesh *lab = painted(PROP.button, 96, 40, tx_scale, 1.7f, 0.7f);
  t3_object_set_position(lab, -0.6f, 0.31f, 0);
  t3_object_set_rotation(lab, -DS_PI / 2, 0, 0);
  /* a quadcopter on the lid, about to lose two motors each to the heroes' backs */
  PROP.drone = group(root);
  t3_object_set_position(PROP.drone, lm_c(lid).x + 3.6f, ly, lm_c(lid).z - 3.0f);
  bx(PROP.drone, 1.1f, 0.32f, 1.1f, 0, 0.3f, 0, 0x30343c);
  bx(PROP.drone, 0.8f, 0.12f, 0.5f, 0, 0.52f, 0, 0xe8c43a);
  for (int i = 0; i < 4; i++) {
    float a = DS_PI / 4 + i * DS_PI / 2, x = sinf(a) * 1.1f, z = cosf(a) * 1.1f;
    t3_mesh *arm = bx(PROP.drone, 0.12f, 0.08f, 1.4f, x * 0.5f, 0.32f, z * 0.5f, 0x1d2026);
    t3_object_set_rotation(arm, 0, a, 0);
    PROP.motors[i] = group(PROP.drone);
    t3_object_set_position(PROP.motors[i], x, 0.36f, z);
    cy(PROP.motors[i], 0.16f, 0.16f, 0.24f, 0, 0.1f, 0, 0x9aa2ac, 0);
    bx(PROP.motors[i], 0.9f, 0.02f, 0.1f, 0, 0.25f, 0, i & 1 ? 0xff7a1a : 0x2ec4b6);
  }
  /* the breaker panel's main handle (the panel itself is in the lab) */
  const lab_box_t *pn = &LM_PANEL[0];
  PROP.breaker = group(root);
  t3_object_set_position(PROP.breaker, (pn->min[0] + pn->max[0]) * 0.5f, pn->min[1] + 9.5f, pn->min[2] - 0.15f);
  bx(PROP.breaker, 3.0f, 2.0f, 0.3f, 0, 0, 0, 0x3a3e46);
  PROP.handle = group(PROP.breaker);
  t3_object_set_position(PROP.handle, 0, 0, -0.2f);
  bx(PROP.handle, 0.7f, 1.6f, 0.4f, 0, 0.6f, -0.1f, 0xd8302a);
  /* the note taped to the laser's front */
  PROP.note = group(root);
  t3_object_set_position(PROP.note, note_at().x, note_at().y, note_at().z);
  painted(PROP.note, 192, 144, tx_note, 4.0f, 3.0f);
  /* a printer's screen, after everything: the job resumed */
  const lab_box_t *pr = &LM_PRINTER[2];
  PROP.screen = group(root);
  t3_object_set_position(PROP.screen, pr->max[0] - 2.1f - 0.8f, pr->min[1] + 2.2f, pr->max[2] + 0.14f);
  painted(PROP.screen, 128, 64, tx_screen62, 4.2f, 2.1f);
  /* the Sergeant's megaphone and the General's pull-string box, placed on their bones each frame */
  PROP.megaphone = group(root);
  t3_mesh *cone = cy(PROP.megaphone, 0.42f, 0.14f, 0.9f, 0, 0, 0, 0xf2ead8, 0);
  t3_object_set_rotation(cone, DS_PI / 2, 0, 0);
  t3_object_set_position(cone, 0, 0, 0.45f);
  bx(PROP.megaphone, 0.14f, 0.36f, 0.16f, 0, -0.2f, 0.1f, 0x2b2f36);
  PROP.pullbox = group(root);
  bx(PROP.pullbox, 2.4f, 2.6f, 1.2f, 0, 0, 0, 0x3a3f2a);
  cy(PROP.pullbox, 0.5f, 0.5f, 0.2f, 0, 0.3f, -0.66f, 0x9aa2ac, 0);
  bx(PROP.pullbox, 0.1f, 1.6f, 0.1f, 0, -1.6f, -0.7f, 0xf2ead8);
  put(PROP.pullbox, t3_torus_geometry_new(0.32f, 0.07f, 6, 10, 2 * DS_PI), mat(0xe8433a, 0.5f, 0), 0, -2.5f, -0.7f);   /* the pull ring */
  bx(PROP.pullbox, 1.0f, 0.8f, 0.1f, 0.4f, -0.6f, 0.62f, 0x2a2e20);   /* the battery hatch, on the far side */
}
static void props_hide(void) {
  PROP.button->visible = PROP.drone->visible = PROP.breaker->visible = false;
  PROP.note->visible = PROP.screen->visible = PROP.megaphone->visible = PROP.pullbox->visible = false;
  for (int i = 0; i < 4; i++) PROP.motors[i]->visible = true;
}

/* ═════════════ the cast: posed plastic army men ═════════════ */
typedef struct {
  t3_gltf *gltf;
  t3_object *root, *model;
  t3_animation_mixer *mixer;
  t3_animation_action *cur, *prev;
  float fade, unit;            /* unit: the model scale that makes it one army man tall */
  bool live;
} act_t;
static act_t CAST[NCAST], SARGE, GEN;

static void act_mesh_cb(t3_object *o, void *ctx) {
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  uint32_t col = *(uint32_t *)ctx;
  o->frustum_culled = false; o->cast_shadow = false; o->receive_shadow = false;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) {
    t3_material *M = m->materials[i];
    if (!M) continue;
    M->color = t3_color_hex(col); M->metalness = 0; M->roughness = 0.42f;   /* moulded plastic, a little gloss */
    M->version++;
  }
}
static bool act_load(act_t *a, uint32_t col) {
  memset(a, 0, sizeof *a);
  size_t n; uint8_t *b = ds_asset("soldier.glb", &n);
  if (!b) return false;
  a->gltf = t3_gltf_parse(b, n, NULL, NULL);
  free(b);
  if (!a->gltf) { ds_log("cine: bad soldier.glb"); return false; }
  a->root = t3_object_new();
  a->model = a->gltf->scene;
  t3_object_add(a->root, a->model);
  t3_object_traverse(a->model, act_mesh_cb, &col);
  /* its height as authored, from the top of the head bone */
  t3_object_update_matrix_world(a->model, true);
  t3_object *head = t3_object_get_by_name(a->model, "Head_end");
  float h = head ? t3_object_get_world_position(head).y * 1.08f : 3.3f;
  a->unit = ARMY_H / (h > 1e-4f ? h : 1);
  a->mixer = t3_animation_mixer_new(a->model);
  t3_object_add(root, a->root); t3_release(a->root);
  a->root->visible = false;
  a->live = true;
  return true;
}
static t3_animation_clip *act_clip(act_t *a, const char *name) {
  for (int i = 0; i < a->gltf->animation_count; i++) {
    const char *nm = a->gltf->animations[i]->name, *bar = strrchr(nm, '|');
    if (!strcmp(bar ? bar + 1 : nm, name)) return a->gltf->animations[i];
  }
  return NULL;
}
/* play a clip, looping, crossfading from the last; the same clip again only changes its speed */
static void act_play(act_t *a, const char *name, float speed) {
  if (!a->live) return;
  t3_animation_clip *c = act_clip(a, name);
  if (!c) return;
  t3_animation_action *ac = t3_animation_mixer_clip_action(a->mixer, c);
  t3_animation_action_set_effective_time_scale(ac, speed);
  if (ac == a->cur) return;
  if (a->prev && a->prev != ac) t3_animation_action_stop(a->prev);
  a->prev = a->cur; a->cur = ac;
  t3_animation_action_play(ac);
  t3_animation_action_set_effective_weight(ac, 0);
  a->fade = 0;
}
/* hold a clip at t01 of its length: standing up, saluting, falling over */
static void act_pose(act_t *a, const char *name, float t01) {
  if (!a->live) return;
  t3_animation_clip *c = act_clip(a, name);
  if (!c) return;
  t3_animation_action *ac = t3_animation_mixer_clip_action(a->mixer, c);
  if (ac != a->cur) {
    if (a->cur) t3_animation_action_stop(a->cur);
    if (a->prev) { t3_animation_action_stop(a->prev); a->prev = NULL; }
    a->cur = ac;
    t3_animation_action_play(ac);
  }
  t3_animation_action_set_effective_time_scale(ac, 0);
  t3_animation_action_set_effective_weight(ac, 1);
  ac->time = ds_clamp(t01, 0, 1) * (c->duration - 0.001f);
  a->fade = 1;
  t3_animation_mixer_update(a->mixer, 0.0001f);
}
static void act_tick(act_t *a, float dt) {
  if (!a->live || !a->root->visible || !a->cur) return;
  a->fade = fminf(1, a->fade + dt * 8);
  t3_animation_action_set_effective_weight(a->cur, a->fade);
  if (a->prev) {
    t3_animation_action_set_effective_weight(a->prev, 1 - a->fade);
    if (a->fade >= 1) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  }
  t3_animation_mixer_update(a->mixer, dt);
}
/* feet at p (world), facing yaw, size in army men (1 = an army man, 2 = the Sergeant) */
static void act_put(act_t *a, v3 p, float yaw, float size) {
  if (!a->live) return;
  a->root->visible = true;
  t3_object_set_position(a->root, p.x, p.y, p.z);
  t3_object_set_rotation(a->root, 0, yaw, 0);
  float s = a->unit * size;
  t3_object_set_scale(a->model, s, s, s);
}
static v3 act_bone(act_t *a, const char *bone) {
  t3_object *b = a->live ? t3_object_get_by_name(a->model, bone) : NULL;
  if (!b) return a->live ? v3_(a->root->position.x, a->root->position.y, a->root->position.z) : v3_(0, 0, 0);
  t3_object_update_matrix_world(a->root, true);
  t3_vec3 w = t3_object_get_world_position(b);
  return v3_(w.x, w.y, w.z);
}
static void cast_hide(void) {
  for (int i = 0; i < NCAST; i++) if (CAST[i].live) CAST[i].root->visible = false;
  if (SARGE.live) SARGE.root->visible = false;
  if (GEN.live) GEN.root->visible = false;
}
static void cast_tick(float dt) {
  for (int i = 0; i < NCAST; i++) act_tick(&CAST[i], dt);
  act_tick(&SARGE, dt); act_tick(&GEN, dt);
}
/* an army man of the cast, placed on whatever is under p */
static void man(int i, v3 p, float yaw) { p.y = ground(p, 1.0f); act_put(&CAST[i], p, yaw, 1); }

/* ── what a cutscene hides and hands back: the level's own army ── */
static t3_object *hidden[64]; static int nhidden;
static void soldier_cb(t3_object *o, void *ctx) {
  int *f = ctx;
  if (o->type == T3_SKINNED_MESH) f[0] = 1;
  if (o->name && !strcmp(o->name, "UpperArmL")) f[1] = 1;
}
static bool is_mine(t3_object *o) { return o == root; }
/* the level's army men and bosses are soldier.glb rigs (a bone named UpperArmL);
 * the heroes' rigs are not, so they stay */
static void hide_army(void) {
  nhidden = 0;
  for (int i = 0; i < scn->base.child_count && nhidden < 64; i++) {
    t3_object *c = scn->base.children[i];
    if (is_mine(c) || !c->visible) continue;
    int f[2] = {0, 0};
    t3_object_traverse(c, soldier_cb, f);
    if (f[0] && f[1]) { hidden[nhidden++] = c; c->visible = false; }
  }
}
static void restore_army(void) {
  for (int i = 0; i < nhidden; i++) hidden[i]->visible = true;
  nhidden = 0;
}

/* ── the heroes ── */
static void hero(cine_view_t *o, hero_id h, v3 p, float yaw, float size, const char *clip, float speed, bool loop) {
  cine_hero_t *c = &o->hero[h];
  c->visible = true; c->pos = p; c->yaw = yaw; c->size = size;
  c->clip = clip; c->clip_speed = speed; c->clip_loop = loop;
  c->aim = 0; c->jetpack = size < 2; c->jet = 0; c->armed = size < 2;
}

/* ═════════════ PROLOGUE: the SCALE button ═════════════ */
static const lab_box_t *LID = &LM_LASER_LID[0];
static float lid_y;                         /* the laser lid's top */
static v3 MO_BIG, LU_BIG, MO_TOY, LU_TOY;   /* full size in front of the laser, then tiny on its lid */
static float t_flash;
static v3 print_floor(int i) {              /* the floor in front of the print rack, two rows */
  const lab_box_t *r = &LM_PRINTER_RACK[0];
  float x = r->min[0] + 3.5f + (float)(i % 6) * ((r->max[0] - r->min[0] - 7) / 5.f);
  return v3_(x + (i / 6) * 1.6f, 0, r->max[2] + 2.0f + (float)(i / 6) * 2.4f);
}
static void prologue_build(void) {
  lid_y = ground(lm_c(LID), 2);
  v3 c = lm_c(LID);
  MO_BIG = v3_(c.x + 5.4f, 0, LID->max[2] + 10.5f);
  LU_BIG = v3_(c.x + 21.0f, 0, LID->max[2] + 15.0f);
  MO_TOY = v3_(c.x - 0.8f, lid_y, c.z + 0.8f);
  LU_TOY = v3_(c.x + 1.0f, lid_y, c.z - 0.4f);
  say("LUIS", "It says SCALE. Someone flashed this laser with firmware off a forum.", 3.2f);   /* 0 */
  say("MOHEEB", "Open hack night. We test things.", 0.4f);                                     /* 1 */
  say("LUIS", "Open hack night ended two hours ago.", 0.4f);                                   /* 2 */
  say("MOHEEB", "Then it's a very small test.", 0.5f);                                         /* 3 */
  say("MOHEEB", "Okay. New project.", 4.6f);                                                   /* 4 */
  say("LUIS", "We are the project.", 0.5f);                                                    /* 5 */
  say("ARMY MAN", "Squad. On your feet. We have ground to take.", 1.2f);                       /* 6 */
  say("LUIS", "The laser resets when the power cycles. The breaker panel is in the machine shop.", 1.0f);   /* 7 */
  say("MOHEEB", "Somebody's drone has four good motors. Two each, and we fly.", 0.5f);        /* 8 */
  say("LUIS", "We label them. We give them back.", 0.5f);                                      /* 9 */
  C.tail = 3.4f;
  finish_lines();
  t_flash = lend(3) - 0.2f;
  v3 btn = v3_(LID->max[0] - 1.5f, lid_y + 0.4f, LID->max[2] - 1.2f);
  v3 mh = v3_add(MO_BIG, v3_(0, 30, 0)), lh = v3_add(LU_BIG, v3_(0, 30, 0));
  /* night in the lab: from the back aisle onto the laser corner */
  shot(-1, 0, pv(c.x - 62, 38, 96), pv(c.x + 6, 20, c.z + 12), 55, pv(c.x - 46, 35, 80), pv(c.x + 6, 22, c.z + 12), 50, 0.03f, 1);
  /* Luis reads the label: low, between him and the laser */
  shot(0, 0, v3_add(btn, v3_(9, 4, 4)), v3_add(lh, v3_(0, 1, 0)), 46, v3_add(btn, v3_(8, 3.5f, 4.5f)), v3_add(lh, v3_(0, 1, 0)), 42, 0.02f, 0);
  /* then they talk it over, each over the other's shoulder, from the open floor side */
  v3 dml = v3_norm(v3_sub(v3_(lh.x, 0, lh.z), v3_(mh.x, 0, mh.z))), side = v3_(-dml.z, 0, dml.x);
  if (side.z < 0) side = v3_scale(side, -1);
  hold(1, 0, v3_add(lh, v3_add(v3_scale(dml, 11), v3_add(v3_scale(side, 7), v3_(0, 3, 0)))), v3_add(mh, v3_(0, 1, 0)), 40);
  hold(2, 0, v3_add(mh, v3_add(v3_scale(dml, -11), v3_add(v3_scale(side, 7), v3_(0, 3, 0)))), v3_add(lh, v3_(0, 1, 0)), 40);
  shot(3, 0, v3_add(btn, v3_(5, 6, 12)), btn, 34, v3_add(btn, v3_(3.5f, 4.5f, 9)), btn, 28, 0.02f, 0);
  /* the flash, then the camera falls to the lid and finds them */
  shot(-1, t_flash + 0.4f, pv(c.x + 5, 46, c.z + 22), pv(c.x, lid_y, c.z), 60, pv(c.x + 4.4f, lid_y + 0.9f, c.z + 4.4f), pv(c.x, lid_y + 1.0f, c.z), 44, 0.0f, 1);
  hold(4, 0, v3_add(MO_TOY, v3_(0.6f, 1.35f, 2.6f)), v3_add(MO_TOY, v3_(0, 1.45f, 0)), 40);
  hold(5, 0, v3_add(LU_TOY, v3_(1.6f, 1.3f, 2.4f)), v3_add(LU_TOY, v3_(0, 1.45f, 0)), 40);
  /* across the room, the print farm sits up */
  const lab_box_t *r = &LM_PRINTER_RACK[0];
  v3 pf = v3_((r->min[0] + r->max[0]) * 0.5f, 0, r->max[2]);
  shot(6, -0.6f, v3_add(pf, v3_(7, 1.8f, 11)), v3_add(pf, v3_(0, 1.6f, 3)), 50, v3_add(pf, v3_(5, 1.2f, 8)), v3_add(pf, v3_(0, 1.4f, 3)), 46, 0.02f, 1);
  hold(7, 0, v3_add(c, v3_(2.8f, lid_y - c.y + 1.7f, 4.4f)), v3_add(c, v3_(0.1f, lid_y - c.y + 1.0f, 0.2f)), 44);
  shot(8, 0, v3_add(MO_TOY, v3_(-2.6f, 1.2f, -1.4f)), v3_add(MO_TOY, v3_(2.4f, 0.8f, -2.4f)), 44, v3_add(MO_TOY, v3_(-2.2f, 1.2f, -1.0f)), v3_add(MO_TOY, v3_(2.4f, 0.9f, -2.4f)), 40, 0.02f, 0);
  hold(9, 0, v3_add(LU_TOY, v3_(1.4f, 1.3f, 2.6f)), v3_add(LU_TOY, v3_(0, 1.4f, 0)), 38);
  shot(-1, lend(9) - 0.1f, v3_add(c, v3_(0.4f, lid_y - c.y + 1.0f, 4.6f)), v3_add(c, v3_(0.1f, lid_y - c.y + 1.2f, 0)), 44,
       v3_add(c, v3_(0.4f, lid_y - c.y + 1.6f, 6.4f)), v3_add(c, v3_(0.1f, lid_y - c.y + 1.4f, 0)), 50, 0.02f, 1);
  resolve_shots();
}
static void prologue_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  v3 c = lm_c(LID);
  bool big = t < t_flash + 0.35f;
  o->lights = 0.62f;
  PROP.button->visible = true;
  PROP.drone->visible = true;
  if (big) {
    v3 btn = v3_(LID->max[0] - 1.5f, 0, LID->max[2] - 1.2f);
    /* Luis reads the button, they turn to talk it over, Moheeb turns back and presses it */
    bool talk = t > lt0(1) - 0.3f && t < lt0(3) + 0.2f;
    float ym = talk ? face(MO_BIG, LU_BIG) : face(MO_BIG, btn), yl = talk ? face(LU_BIG, MO_BIG) : face(LU_BIG, btn);
    const char *mclip = t > lt0(3) + 0.6f ? "Punch_Jab" : "Idle_Loop";
    hero(o, HERO_MOHEEB, MO_BIG, ym, LAB_SCALE, mclip, 0.8f, !strcmp(mclip, "Idle_Loop"));
    hero(o, HERO_LUIS, LU_BIG, yl, LAB_SCALE, "Idle_Loop", 0.8f, true);
    fx_glow(v3_(LID->max[0] - 1.5f, lid_y + 0.7f, LID->max[2] - 1.2f), 1.2f + 0.4f * sinf(t * 6), 0xe8433a);
  } else {
    /* tiny on the lid: they come round, look at each other, then up at the room */
    float u = t - t_flash;
    const char *clip = u < 2.6f ? "KipUp" : (t > lt0(4) - 1.2f && t < lt0(5)) ? "Surprise" : "Idle_Loop";
    bool once = strcmp(clip, "Idle_Loop") != 0;
    float ym = face(MO_TOY, LU_TOY), yl = face(LU_TOY, MO_TOY);
    if (t > lt0(8) - 0.4f) ym = face(MO_TOY, lm_c(LID)) + 0.6f;
    hero(o, HERO_MOHEEB, MO_TOY, ym, 1, clip, 0.9f, !once);
    hero(o, HERO_LUIS, LU_TOY, yl, 1, u < 2.9f ? "KipUp" : "Idle_Loop", 0.85f, u >= 2.9f);
    bool packs = t > lend(9) - 0.2f;
    o->hero[HERO_MOHEEB].jetpack = o->hero[HERO_LUIS].jetpack = packs;
    for (int i = 0; i < 4; i++) PROP.motors[i]->visible = !packs;
    if (packs && t > lend(9) + 1.0f) o->hero[HERO_MOHEEB].clip = "Celebration", o->hero[HERO_MOHEEB].clip_loop = false;
  }
  /* the flash: white for a beat, then a slow fade back */
  float f = t - t_flash;
  o->flash = f < 0 ? 0 : f < 0.12f ? f / 0.12f : f < 0.5f ? 1 : fmaxf(0, 1 - (f - 0.5f) / 1.4f);
  cue(0, t, lt0(3) + 0.8f, SFX_UI_OK, 0.6f);
  cue(1, t, t_flash, SFX_COIL, 0.9f);
  cue(2, t, t_flash + 0.2f, SFX_BOOM, 0.5f);
  cue(3, t, lend(9) + 0.6f, SFX_JET, 0.6f);
  if (!big && f < 3) fx_glow(v3_(c.x, lid_y + 0.6f, c.z), 4 * (1 - f / 3), 0xfff4d8);
  /* the print farm: twelve army men sit up in front of the rack, a beat apart */
  for (int i = 0; i < NCAST; i++) {
    if (big) { CAST[i].root->visible = false; continue; }
    v3 p = print_floor(i);
    man(i, p, 0.15f * (float)((i * 7) % 5 - 2));
    float u = seg(t, lt0(6) - 0.9f + (float)((i * 5) % 12) * 0.16f, lt0(6) + 0.6f + (float)((i * 5) % 12) * 0.16f);
    if (t < lt0(6) + 3.5f) act_pose(&CAST[i], "StandUp", u);
    else act_play(&CAST[i], "Idle", 0.8f);
  }
}

/* ═════════════ the level intros: staged at the level's start ═════════════ */
static void intro_frame(int level) {
  float y; v3 s = mission_start(level, &y);
  C.org = s; C.yaw = y; C.ray = true;
}
static v3 MO_AT, LU_AT;    /* the heroes' marks in the scene's frame */
static void heroes_at_marks(cine_view_t *o, float ym, float yl, const char *mc, const char *lc) {
  v3 m = W(MO_AT), l = W(LU_AT);
  m.y = ground(m, 1.0f); l.y = ground(l, 1.0f);
  hero(o, HERO_MOHEEB, m, WY(ym), 1, mc ? mc : "Idle_Loop", 0.8f, !mc);
  hero(o, HERO_LUIS, l, WY(yl), 1, lc ? lc : "Idle_Loop", 0.8f, !lc);
}
/* a two-shot from the front, a single on each, and a look down the route */
static v3 mk_head(v3 mark) { return v3_add(mark, v3_(0, 1.5f, 0)); }

/* ── CUT_STORAGE: member storage and the army's supply line ── */
static void storage_build(void) {
  intro_frame(1);
  MO_AT = pv(0.9f, 0, 0); LU_AT = pv(-0.9f, 0, -0.4f);
  say("LUIS", "Member storage. Every bin on these shelves has somebody's name on it.", 1.4f);   /* 0 */
  say("MOHEEB", "And the army is running supplies through it. Look at them go.", 0.4f);         /* 1 */
  say("ARMY MAN", "Left, right. Left, right. Glue sticks to the front.", 0.4f);               /* 2 */
  say("LUIS", "Nobody opens a member's bin without asking.", 0.5f);                            /* 3 */
  say("MOHEEB", "I'll ask very politely. Afterwards.", 0.4f);                                   /* 4 */
  C.tail = 2.2f;
  finish_lines();
  shot(-1, 0, pv(0.4f, 0.5f, 5.0f), pv(0, 1.2f, 0), 50, pv(0.2f, 0.9f, 3.6f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(LU_AT, pv(1.4f, 1.3f, 2.4f)), mk_head(LU_AT), 40);
  shot(1, 0, pv(-0.1f, 1.7f, -2.6f), pv(0, 1.5f, 24), 52, pv(-0.1f, 1.8f, -2.0f), pv(0, 2.0f, 24), 48, 0.02f, 1);
  shot(2, 0, pv(-3.0f, 0.5f, 14.0f), pv(0, 0.9f, 20), 44, pv(-2.0f, 0.5f, 15.0f), pv(0, 0.9f, 22), 42, 0.03f, 0);
  hold(3, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  hold(4, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 38);
  resolve_shots();
}
static void storage_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, 0.2f, -0.2f, t > lt0(4) ? "Celebration" : NULL, NULL);
  if (t > lt0(1) && t < lt0(3)) o->hero[HERO_MOHEEB].yaw = WY(0);
  /* a column of six marching across the aisle and into the shelving, two carrying */
  for (int i = 0; i < 6; i++) {
    float x = -14 + t * 1.25f - (float)i * 1.6f;
    v3 p = W(pv(x, 0, 20 + (i & 1) * 1.3f));
    man(i, p, WY(-DS_PI / 2));
    act_play(&CAST[i], i % 3 == 1 ? "Walk_Carry" : "Walk", 1.0f);
  }
}

/* ── CUT_PRINTFARM: the printers are still running ── */
static void printfarm_build(void) {
  intro_frame(2);
  MO_AT = pv(0.9f, 0, 0); LU_AT = pv(-0.9f, 0, -0.3f);
  say("MOHEEB", "Hear that? Stepper motors.", 1.4f);                                                 /* 0 */
  say("LUIS", "The print farm. Still running the job somebody queued for the kids' class.", 0.4f);   /* 1 */
  say("ARMY MAN", "Veterans, up the towers. Hold the high ground.", 0.4f);                         /* 2 */
  say("MOHEEB", "Five hundred in the job. We've met maybe forty.", 0.5f);                         /* 3 */
  say("LUIS", "Then we cancel the other four hundred and sixty.", 0.4f);                           /* 4 */
  C.tail = 2.2f;
  finish_lines();
  const lab_box_t *lg = &LM_LEGO_TOWER[1];
  v3 tw = v3_((lg->min[0] + LM_LEGO_TOWER[2].max[0]) * 0.5f, lg->max[1], (lg->min[2] + lg->max[2]) * 0.5f);
  const lab_box_t *pr = &LM_PRINTER[3];
  v3 pp = v3_((LM_PRINTER[2].min[0] + pr->max[0]) * 0.5f, pr->min[1] + 6, pr->max[2]);
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 40);
  /* the world-space shots: C.org and C.yaw are put back to the lab for them in act */
  hold(1, 0, pv(0, 0, 0), pv(0, 0, 0), 46);
  hold(2, 0, pv(0, 0, 0), pv(0, 0, 0), 42);
  hold(3, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 38);
  hold(4, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  resolve_shots();
  /* shots 2 and 3 look across the room: write them in world space through the frame */
  float c = cosf(-C.yaw), s = sinf(-C.yaw);
  v3 cams[2][2] = {{v3_(pp.x + 12, pp.y + 3, pp.z + 30), pp}, {v3_(tw.x + 4, 17, 34), v3_add(tw, v3_(0, 1, 0))}};
  for (int k = 0; k < 2; k++) for (int e = 0; e < 2; e++) {
    v3 w = v3_sub(cams[k][e], C.org);
    v3 l = v3_(w.x * c + w.z * s, w.y, -w.x * s + w.z * c);
    if (e == 0) C.S[2 + k].p0 = C.S[2 + k].p1 = l; else C.S[2 + k].a0 = C.S[2 + k].a1 = l;
  }
}
static void printfarm_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, 0.15f, -0.25f, NULL, NULL);
  /* veterans on the four LEGO towers: tan, saluting as the order comes */
  for (int i = 0; i < 4; i++) {
    const lab_box_t *b = &LM_LEGO_TOWER[i];
    man(i, v3_((b->min[0] + b->max[0]) * 0.5f, b->max[1] + 0.5f, (b->min[2] + b->max[2]) * 0.5f + 1.5f), 0);
    if (t > lt0(2) + 0.3f + i * 0.2f) act_pose(&CAST[i], "Victory", seg(t, lt0(2) + 0.3f + i * 0.2f, lt0(2) + 2.2f + i * 0.2f));
    else act_play(&CAST[i], "Idle", 0.7f);
  }
}

/* ── CUT_COWORKING: under the desks ── */
static void coworking_build(void) {
  intro_frame(4);
  MO_AT = pv(-0.9f, 0, 0); LU_AT = pv(0.9f, 0, -0.3f);
  say("LUIS", "Coworking. Twelve desks, forty chairs and every cable in the building.", 1.4f);   /* 0 */
  say("MOHEEB", "We go underneath. The desks are a roof.", 0.4f);                                 /* 1 */
  say("ARMY MAN", "Movement by the desks! Gunners to the couch!", 0.4f);                          /* 2 */
  say("LUIS", "They put gunners on the couch.", 0.5f);                                             /* 3 */
  say("MOHEEB", "Then we're taking the couch.", 0.4f);                                             /* 4 */
  C.tail = 2.2f;
  finish_lines();
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(LU_AT, pv(1.2f, 1.3f, 2.4f)), mk_head(LU_AT), 40);
  shot(1, 0, pv(0.5f, 0.7f, 6), pv(0, 2.5f, 40), 56, pv(0.5f, 0.8f, 9), pv(0, 2.5f, 44), 52, 0.02f, 1);
  hold(2, 0, pv(0, 0, 0), pv(0, 0, 0), 44);
  hold(3, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  hold(4, 0, v3_add(MO_AT, pv(-2.2f, 1.2f, 1.6f)), mk_head(MO_AT), 40);
  resolve_shots();
  /* the couch, from the lounge floor: world space through the frame */
  const lab_box_t *so = &LM_SOFA_SEAT[1];
  v3 cp = v3_(so->max[0] + 22, 13, so->min[2] - 14), ct = v3_((so->min[0] + so->max[0]) * 0.5f, 9, (so->min[2] + so->max[2]) * 0.5f);
  float c = cosf(-C.yaw), sn = sinf(-C.yaw);
  v3 w0 = v3_sub(cp, C.org), w1 = v3_sub(ct, C.org);
  C.S[3].p0 = C.S[3].p1 = v3_(w0.x * c + w0.z * sn, w0.y, -w0.x * sn + w0.z * c);
  C.S[3].a0 = C.S[3].a1 = v3_(w1.x * c + w1.z * sn, w1.y, -w1.x * sn + w1.z * c);
}
static void coworking_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, -0.15f, 0.2f, t > lt0(4) + 0.3f ? "Melee_Hook" : NULL, NULL);
  /* the couch: three tan gunners on the seats, a rifleman waving them on */
  for (int i = 0; i < 3; i++) {
    const lab_box_t *b = &LM_SOFA_SEAT[i];
    man(i, v3_((b->min[0] + b->max[0]) * 0.5f, b->max[1] + 0.5f, (b->min[2] + b->max[2]) * 0.5f), -DS_PI / 2);
    act_play(&CAST[i], t > lt0(2) ? "Shoot_OneHanded" : "Idle", 0.6f);
  }
  const lab_box_t *ct = &LM_COFFEE_TABLE[0];
  man(3, v3_(ct->max[0] + 1.5f, 0, (ct->min[2] + ct->max[2]) * 0.5f), -DS_PI / 2 + 0.4f);
  act_play(&CAST[3], t > lt0(2) ? "Punch" : "Idle", 0.8f);
}

/* ── CUT_WOODSHOP: the electronics bench, and a megaphone through the wall ── */
static void woodshop_build(void) {
  intro_frame(5);
  MO_AT = pv(-0.9f, 0, 0); LU_AT = pv(0.9f, 0, -0.3f);
  say("MOHEEB", "Electronics bench. The good soldering iron is right up there.", 1.4f);            /* 0 */
  say("LUIS", "Past the woodshop is the machine shop. And the breaker.", 0.4f);                    /* 1 */
  say("THE SERGEANT", "ALL UNITS. THE WOODSHOP IS NOW A FORWARD BASE. SAFETY GLASSES ON.", 0.8f); /* 2 */
  say("MOHEEB", "A megaphone. That's new.", 0.5f);                                                 /* 3 */
  say("LUIS", "Somebody printed a sergeant.", 0.4f);                                               /* 4 */
  C.tail = 2.2f;
  finish_lines();
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 40);
  hold(1, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 40);
  shot(2, 0, pv(-1.0f, 1.0f, -1.0f), pv(63, 4, 70), 50, pv(-1.0f, 1.0f, -0.5f), pv(63, 4.5f, 70), 44, 0.05f, 0);
  hold(3, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 38);
  hold(4, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  resolve_shots();
}
static void woodshop_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  bool turn = t > lt0(2) - 0.2f && t < lt0(3) + 0.8f;
  heroes_at_marks(o, turn ? 0.9f : -0.15f, turn ? 0.9f : 0.2f, t > lt0(2) && t < lt0(2) + 1.5f ? "Surprise" : NULL, NULL);
  cue(0, t, lt0(2) - 0.1f, SFX_WHISTLE, 0.4f);
}

/* ── the bosses ── */
static v3 sarge_at(void) {
  const lab_box_t *b = &LM_WOOD_BENCH[0];
  v3 p = v3_((b->min[0] + b->max[0]) * 0.5f, b->max[1], (b->min[2] + b->max[2]) * 0.5f - 8);
  p.y = ground(p, 2.5f);
  return p;
}
static v3 general_at(void) {
  const lab_box_t *b = &LM_LATHE_BED[0];
  v3 p = v3_((b->min[0] + b->max[0]) * 0.5f - 13.5f, 0, (b->min[2] + b->max[2]) * 0.5f - 21);   /* the clear floor in front of the lathe (mission.c spawns him here) */
  p.y = ground(p, 3);
  return p;
}
/* the heroes on two marks a few steps in front of the boss, on the side the
 * player came in from; the scene's frame faces the boss from there */
static void boss_frame(v3 boss, float back) {
  v3 d = have_hint ? v3_sub(hint_player, boss) : v3_(0, 0, -1);
  d.y = 0; d = v3_norm(d);
  if (v3_len(d) < 0.5f) d = v3_(-1, 0, 0);
  v3 at = v3_add(boss, v3_scale(d, back));
  C.org = v3_(at.x, ground(v3_(at.x, 0, at.z), 3), at.z);
  C.yaw = face(C.org, boss);
  C.ray = false;
}
static void place_heroes_hint(cine_view_t *o, v3 boss, const char *mc, const char *lc) {
  v3 m = W(pv(-0.9f, 0, 0)), l = W(pv(0.9f, 0, -0.4f));
  m.y = ground(m, 1); l.y = ground(l, 1);
  hero_id p = C.player, b = (hero_id)(C.player ^ 1);
  hero(o, p, m, face(m, boss), 1, mc ? mc : "Idle_Loop", 0.8f, !mc);
  hero(o, b, l, face(l, boss), 1, lc ? lc : "Idle_Loop", 0.8f, !lc);
}
static void sarge_props(float t) {
  /* the megaphone in his right fist, pointed where he faces */
  v3 f = act_bone(&SARGE, "FistR");
  PROP.megaphone->visible = true;
  t3_object_set_position(PROP.megaphone, f.x, f.y, f.z);
  t3_object_set_rotation(PROP.megaphone, -0.2f + sinf(t * 2) * 0.05f, SARGE.root->rotation.y, 0);
  t3_object_set_scale(PROP.megaphone, 0.7f, 0.7f, 0.7f);
}
static void general_props(void) {
  /* the pull-string box between his shoulder blades */
  v3 tor = act_bone(&GEN, "Torso");
  float y = GEN.root->rotation.y;
  PROP.pullbox->visible = true;
  t3_object_set_position(PROP.pullbox, tor.x - sinf(y) * 1.3f, tor.y + 0.6f, tor.z - cosf(y) * 1.3f);
  t3_object_set_rotation(PROP.pullbox, 0, y, 0);
}

/* ── CUT_SERGEANT: the forward base on the eight foot bench ── */
static void sergeant_build(void) {
  v3 s = sarge_at();
  boss_frame(s, 13);
  C.fi = 0.4f; C.fo = 0.5f;
  say("THE SERGEANT", "ATTENTION. THIS WOODSHOP IS NOW A FORWARD BASE.", 1.6f);    /* 0 */
  say("THE SERGEANT", "NOBODY GETS PAST THE EIGHT FOOT BENCH.", 0.3f);             /* 1 */
  say("MOHEEB", "He's twice the size of the others.", 0.4f);                       /* 2 */
  say("LUIS", "Printed at two hundred percent. Nobody checked the slicer.", 0.4f); /* 3 */
  say("THE SERGEANT", "SQUAD. TAKE THAT HILL.", 0.4f);                             /* 4 */
  say("LUIS", "That's a dust collector.", 0.4f);                                   /* 5 */
  C.tail = 1.8f;
  finish_lines();
  /* world space shots around the bench, through the frame */
  float c = cosf(-C.yaw), sn = sinf(-C.yaw);
  #define LOC(w) v3_(((w).x - C.org.x) * c + ((w).z - C.org.z) * sn, (w).y - C.org.y, -((w).x - C.org.x) * sn + ((w).z - C.org.z) * c)
  v3 head = v3_add(s, v3_(0, ARMY_H * 2 * 0.9f, 0));
  v3 low = v3_add(s, v3_(-16, -14, 6)), low2 = v3_add(s, v3_(-12, -11, 4));
  shot(-1, 0, LOC(low), LOC(head), 46, LOC(low2), LOC(head), 40, 0.03f, 1);
  shot(0, 0.4f, LOC(v3_add(s, v3_(-4.5f, 1.6f, 1.5f))), LOC(head), 40, LOC(v3_add(s, v3_(-3.8f, 1.8f, 1.2f))), LOC(head), 36, 0.03f, 0);
  hold(1, 0, LOC(v3_add(s, v3_(-3.0f, 3.2f, -2.5f))), LOC(head), 38);
  hold(2, 0, pv(-1.6f, 1.2f, 2.4f), pv(0, 1.4f, 0), 40);
  hold(3, 0, pv(1.6f, 1.2f, 2.0f), pv(-0.9f, 1.4f, -0.4f), 40);
  shot(4, 0, LOC(v3_add(s, v3_(-5.0f, 1.4f, 1.6f))), LOC(head), 40, LOC(v3_add(s, v3_(-4.4f, 1.6f, 1.4f))), LOC(head), 36, 0.05f, 0);
  hold(5, 0, pv(1.4f, 1.2f, 2.2f), pv(-0.9f, 1.4f, -0.4f), 38);
  #undef LOC
  resolve_shots();
}
static void sergeant_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  v3 s = sarge_at();
  place_heroes_hint(o, s, t > lt0(2) && t < lt0(2) + 1.4f ? "Surprise" : NULL, NULL);
  float ys = face(s, o->hero[C.player].pos);
  act_put(&SARGE, s, ys, 2);
  act_play(&SARGE, t > lt0(4) ? "Punch" : t > lt0(0) && t < lt0(2) ? "Shoot_OneHanded" : "Idle", 0.7f);
  sarge_props(t);
  /* his squad on the bench, saluting him */
  for (int i = 0; i < 4; i++) {
    float side = (i & 1 ? 1.f : -1.f) * (2.4f + (i >> 1) * 1.6f);
    v3 p = v3_add(s, v3_(cosf(ys) * side - sinf(ys) * 1.2f, 0, -sinf(ys) * side - cosf(ys) * 1.2f));
    man(i, v3_add(p, v3_(0, 1, 0)), ys);
    if (t > lt0(4) + 0.3f) act_play(&CAST[i], "Walk", 1.1f);
    else act_pose(&CAST[i], "Victory", 0.35f + 0.05f * sinf(t * 2 + i));
  }
  cue(0, t, lt0(0), SFX_WHISTLE, 0.5f);
}

/* ── CUT_MACHINESHOP: the breaker panel, and something in front of it ── */
static void machineshop_build(void) {
  intro_frame(8);
  MO_AT = pv(-0.9f, 0, 0); LU_AT = pv(0.9f, 0, -0.3f);
  say("LUIS", "Machine shop. The breaker panel is on the far wall, above the lathe.", 1.4f);   /* 0 */
  say("MOHEEB", "Flip the main, the laser resets, we get our height back.", 0.4f);            /* 1 */
  say("LUIS", "Something is standing in front of the lathe.", 0.6f);                          /* 2 */
  say("MOHEEB", "Something tall.", 0.5f);                                                     /* 3 */
  C.tail = 2.4f;
  finish_lines();
  const lab_box_t *pn = &LM_PANEL[0];
  v3 panel = v3_((pn->min[0] + pn->max[0]) * 0.5f, (pn->min[1] + pn->max[1]) * 0.5f, pn->min[2]);
  v3 g = general_at();
  float c = cosf(-C.yaw), sn = sinf(-C.yaw);
  #define LOC(w) v3_(((w).x - C.org.x) * c + ((w).z - C.org.z) * sn, (w).y - C.org.y, -((w).x - C.org.x) * sn + ((w).z - C.org.z) * c)
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  shot(0, 0.2f, pv(0.8f, 1.0f, 3.0f), LOC(v3_add(panel, v3_(0, -10, 0))), 58, pv(0.8f, 1.1f, 2.4f), LOC(v3_add(panel, v3_(0, -6, 0))), 52, 0.02f, 1);
  hold(1, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 40);
  shot(2, 0, pv(-0.6f, 0.8f, 3.0f), LOC(v3_add(g, v3_(0, 4, 0))), 46, pv(-0.6f, 0.8f, 2.6f), LOC(v3_add(g, v3_(0, 8, 0))), 42, 0.02f, 1);
  hold(3, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 36);
  #undef LOC
  resolve_shots();
}
static void machineshop_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, -0.15f, 0.2f, NULL, t > lt0(2) && t < lt0(2) + 1.5f ? "Surprise" : NULL);
  v3 g = general_at();
  act_put(&GEN, g, face(g, C.org), 8);
  act_play(&GEN, "Idle", 0.4f);
  general_props();
}

/* ── CUT_GENERAL: the twelve-inch display figure ── */
static void general_build(void) {
  v3 g = general_at();
  have_hint = false;   /* he stands in front of the lathe: the heroes face him from the open floor on his front */
  boss_frame(g, 15);
  C.fi = 0.4f; C.fo = 0.5f;
  say("THE GENERAL", "THIS IS MY BENCH NOW.", 1.8f);                  /* 0 */
  say("MOHEEB", "His battery hatch has a screw missing.", 0.5f);     /* 1 */
  say("LUIS", "Everyone's does.", 0.4f);                              /* 2 */
  say("THE GENERAL", "PULL MY STRING.", 0.5f);                        /* 3 */
  say("MOHEEB", "Nobody pull his string.", 0.4f);                     /* 4 */
  say("THE GENERAL", "HOLD THE LINE.", 0.4f);                         /* 5 */
  C.tail = 1.8f;
  finish_lines();
  float c = cosf(-C.yaw), sn = sinf(-C.yaw);
  #define LOC(w) v3_(((w).x - C.org.x) * c + ((w).z - C.org.z) * sn, (w).y - C.org.y, -((w).x - C.org.x) * sn + ((w).z - C.org.z) * c)
  float H8 = ARMY_H * 8;
  v3 head = v3_add(g, v3_(0, H8 * 0.88f, 0)), boots = v3_add(g, v3_(0, 1.0f, 0));
  /* from his boots up to his face */
  shot(-1, 0, pv(0.6f, 0.3f, 3.0f), LOC(boots), 50, pv(0.6f, 0.4f, 3.0f), LOC(head), 46, 0.02f, 1);
  shot(0, 0, pv(0.5f, 0.4f, 4.0f), LOC(head), 42, pv(0.3f, 0.4f, 5.0f), LOC(head), 38, 0.03f, 0);
  /* behind him: the box on his back, the hatch */
  float yg = face(g, C.org);
  v3 back = v3_add(g, v3_(-sinf(yg) * 7, H8 * 0.62f, -cosf(yg) * 7));
  shot(1, 0, LOC(v3_add(back, v3_(cosf(yg) * 3, 0.5f, -sinf(yg) * 3))), LOC(v3_add(g, v3_(0, H8 * 0.6f, 0))), 36, LOC(back), LOC(v3_add(g, v3_(0, H8 * 0.6f, 0))), 32, 0.02f, 0);
  hold(2, 0, pv(1.5f, 1.2f, 2.2f), pv(-0.9f, 1.4f, -0.4f), 40);
  shot(3, 0, LOC(v3_add(g, v3_(sinf(yg) * 9, H8 * 0.7f, cosf(yg) * 9))), LOC(head), 38, LOC(v3_add(g, v3_(sinf(yg) * 7, H8 * 0.72f, cosf(yg) * 7))), LOC(head), 34, 0.03f, 0);
  hold(4, 0, pv(-1.5f, 1.2f, 2.4f), pv(0, 1.4f, 0), 40);
  shot(5, 0, pv(-3.0f, 0.6f, -3.0f), LOC(head), 52, pv(-3.6f, 0.8f, -4.0f), LOC(head), 56, 0.05f, 1);
  #undef LOC
  resolve_shots();
}
static void general_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  v3 g = general_at();
  place_heroes_hint(o, g, NULL, t > lt0(2) && t < lt0(2) + 1.2f ? "Surprise" : NULL);
  act_put(&GEN, g, face(g, C.org), 8);
  act_play(&GEN, t > lt0(5) ? "Punch" : t > lt0(3) && t < lt0(4) ? "Victory" : "Idle", 0.5f);
  general_props();
  cue(0, t, lt0(0) - 0.2f, SFX_ALARM, 0.3f);
}

/* ═════════════ ENDING: the main breaker ═════════════ */
static float t_flip, t_back;
static v3 MO_END, LU_END;
static v3 lying(int i) {   /* the army after the lights: scattered on the machine shop floor */
  const lab_area_t *ms = &LAB_AREAS[AREA_MACHINE_SHOP];
  float x = ms->min[0] + 58 + (float)((i * 37) % 40), z = ms->min[2] + 28 + (float)((i * 23) % 34);
  return v3_(x, 0, z);
}
static void ending_build(void) {
  C.org = v3_(0, 0, 0); C.yaw = 0; C.fo = 1.6f;
  const lab_area_t *ms = &LAB_AREAS[AREA_MACHINE_SHOP];
  MO_END = v3_(ms->min[0] + 69, 0, ms->min[2] + 43);
  LU_END = v3_(ms->min[0] + 86, 0, ms->min[2] + 41);
  say_as("THE GENERAL", "HOLD THE LI...", "Hold the, lie", 1.4f);                                 /* 0 */
  say("MOHEEB", "Panel. Go.", 1.4f);                                                             /* 1 */
  say("LUIS", "Main breaker. Three, two, one.", 1.2f);                                           /* 2 */
  say("MOHEEB", "We're tall again.", 5.2f);                                                      /* 3 */
  say("LUIS", "And they're just toys again.", 0.5f);                                             /* 4 */
  say("MOHEEB", "I'm keeping this one.", 0.6f);                                                  /* 5 */
  say_as("LUIS", "There's a note on the laser. PLEASE RETURN THE SCALE TO 1:1 WHEN YOU ARE DONE.",
         "There's a note on the laser. Please return the scale to one to one when you are done.", 1.6f);   /* 6 */
  say("MOHEEB", "Fair.", 0.4f);                                                                  /* 7 */
  say_as("PRINTER", "ARMY_MEN_x500.gcode  62 PERCENT", "Army men, five hundred. Sixty two percent.", 1.8f);   /* 8 */
  C.tail = 3.0f;
  finish_lines();
  t_flip = lend(2) - 0.3f; t_back = t_flip + 2.6f;
  v3 g = general_at(), head = v3_add(g, v3_(0, ARMY_H * 8 * 0.8f, 0));
  const lab_box_t *pn = &LM_PANEL[0];
  v3 panel = v3_((pn->min[0] + pn->max[0]) * 0.5f, pn->min[1] + 9.5f, pn->min[2]);
  shot(-1, 0, v3_add(g, v3_(8, 5, -15)), head, 46, v3_add(g, v3_(7, 4, -13)), v3_add(g, v3_(0, 3, 0)), 44, 0.03f, 1);
  shot(1, -0.3f, v3_add(panel, v3_(4, -8, -22)), v3_add(panel, v3_(-6, -13, -4)), 56, v3_add(panel, v3_(3, -4, -20)), v3_add(panel, v3_(0, -4, -1)), 50, 0.02f, 1);
  shot(2, 0, v3_add(panel, v3_(-3.5f, 1.5f, -6)), panel, 40, v3_add(panel, v3_(-2.8f, 1.2f, -5)), panel, 36, 0.02f, 0);
  /* the lights come back on two full size people */
  v3 mid = v3_lerp(MO_END, LU_END, 0.5f);
  shot(-1, t_back - 0.2f, v3_add(mid, v3_(-4, 22, -24)), v3_add(mid, v3_(0, 12, 0)), 66, v3_add(mid, v3_(-4, 24, -22)), v3_add(mid, v3_(0, 16, 0)), 62, 0.02f, 1);
  hold(4, 0, v3_add(LU_END, v3_(-6, 31, -18)), v3_add(LU_END, v3_(0, 30, 0)), 44);
  shot(5, 0, v3_add(MO_END, v3_(8, 28, -18)), v3_add(MO_END, v3_(0, 24, 0)), 46, v3_add(MO_END, v3_(6, 27, -16)), v3_add(MO_END, v3_(0, 24, 0)), 42, 0.02f, 0);
  /* the laser corner, the note */
  v3 note = note_at();
  shot(6, -0.3f, v3_add(note, v3_(-4, 6, 16)), note, 46, v3_add(note, v3_(-1, 1.5f, 10)), note, 32, 0.02f, 1);
  hold(7, 0, v3_add(note, v3_(26, 20, 50)), v3_add(note, v3_(0, 12, 16)), 50);
  /* and the print farm */
  const lab_box_t *pr = &LM_PRINTER[2];
  v3 scr = v3_(pr->max[0] - 2.9f, pr->min[1] + 2.2f, pr->max[2] + 0.14f);
  shot(8, -0.4f, v3_add(scr, v3_(1.5f, 1.4f, 9)), scr, 44, v3_add(scr, v3_(0.4f, 0.4f, 4.6f)), scr, 38, 0.0f, 1);
  resolve_shots();
}
static void ending_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  v3 g = general_at();
  const lab_box_t *pn = &LM_PANEL[0], *lb = &LM_LATHE_BED[0];
  v3 panel = v3_((pn->min[0] + pn->max[0]) * 0.5f, pn->min[1] + 9.5f, pn->min[2]);
  o->lights = 1;
  if (t < t_back - 0.3f) {
    /* the General runs down; they jet from the lathe up to the panel */
    act_put(&GEN, g, face(g, v3_(g.x - 10, 0, g.z - 20)), 8);
    act_pose(&GEN, "Defeat", seg(t, lt0(0) + 0.6f, lt0(0) + 3.4f));
    general_props();
    float u = sm(seg(t, lt0(1) + 0.2f, t_flip - 0.4f));
    v3 a = v3_(lb->min[0] + 6, ground(v3_(lb->min[0] + 6, 30, lm_c(lb).z), 0), lm_c(lb).z - 1);
    v3 b = v3_(panel.x - 1.0f, panel.y - 2.6f, pn->min[2] - 1.4f);
    for (int h = 0; h < HERO_COUNT; h++) {
      v3 p0 = v3_add(a, v3_(h ? 1.4f : -1.4f, 0, 0)), p1 = v3_add(b, v3_(h ? 1.8f : -1.0f, 0, 0));
      v3 p = v3_lerp(p0, p1, u);
      p.y += sinf(u * DS_PI) * 1.5f;
      bool fly = u > 0.02f && u < 0.98f;
      hero(o, (hero_id)h, p, face(p, v3_(p.x, 0, pn->max[2] + 5)), 1, fly || u >= 0.98f ? "Jump_Loop" : "Idle_Loop", 1, true);
      o->hero[h].jet = fly ? 1 : u >= 0.98f ? 0.5f : 0;
      if (u >= 0.98f && t > lt0(2) + 0.8f && h == HERO_LUIS) o->hero[h].clip = "Punch_Cross", o->hero[h].clip_loop = false;
    }
    if (u > 0.02f) sfx(SFX_JET, 0, 0.25f);
    PROP.breaker->visible = true;
    float k = sm(seg(t, t_flip - 0.4f, t_flip));
    t3_object_set_rotation(PROP.handle, -0.9f + 1.8f * k, 0, 0);
    float out = seg(t, t_flip, t_flip + 0.25f);
    o->lights = 1 - 0.999f * out;
    o->fade = out * 0.82f;
  } else {
    /* the lights come back: two full size people among very still army men */
    float back = seg(t, t_back - 0.3f, t_back + 0.5f);
    o->lights = 0.001f + back * 0.999f;
    o->fade = 0.82f * (1 - back);
    bool at_laser = t > lt0(6) - 0.4f;
    if (!at_laser) {
      hero(o, HERO_MOHEEB, MO_END, face(MO_END, LU_END) + 0.9f, LAB_SCALE, t > lt0(5) - 0.3f ? "Punch_Jab" : t < lt0(4) ? "Surprise" : "Idle_Loop", 0.7f, t >= lt0(4) && t < lt0(5) - 0.3f);
      hero(o, HERO_LUIS, LU_END, face(LU_END, MO_END) - 0.9f, LAB_SCALE, t < lt0(4) ? "Surprise" : "Idle_Loop", 0.7f, t >= lt0(4));
      for (int i = 0; i < NCAST; i++) {
        v3 p = lying(i);
        if (i == 0 && t > lt0(5) + 0.5f) { CAST[i].root->visible = false; continue; }   /* in a pocket now */
        man(i, p, (float)i * 1.7f);
        act_pose(&CAST[i], "Defeat", 1);
      }
    } else {
      const lab_box_t *lid = &LM_LASER_LID[0];
      v3 lp = v3_(lm_c(lid).x + 18, 0, lid->max[2] + 14);
      if (t < lt0(8) - 0.4f) {
        hero(o, HERO_LUIS, lp, face(lp, v3_(lm_c(lid).x + 4, 0, lid->max[2])), LAB_SCALE, "Idle_Loop", 0.6f, true);
        v3 mp = v3_(lm_c(lid).x - 12, 0, lid->max[2] + 22);
        hero(o, HERO_MOHEEB, mp, face(mp, lp), LAB_SCALE, t > lt0(7) ? "Celebration" : "Idle_Loop", 0.7f, t <= lt0(7));
      }
      PROP.note->visible = true;
      PROP.screen->visible = t > lt0(8) - 0.5f;
      for (int i = 0; i < NCAST; i++) CAST[i].root->visible = false;
    }
  }
  cue(0, t, t_flip, SFX_PRINT_STOP, 0.9f);
  cue(1, t, t_back - 0.2f, SFX_UI_OK, 0.6f);
  cue(2, t, lt0(0) + 1.6f, SFX_TOPPLE, 0.9f);
}

/* world points to the scene's frame, for shots that look across the lab */
static v3 loc(v3 w) {
  float c = cosf(-C.yaw), sn = sinf(-C.yaw);
  v3 d = v3_sub(w, C.org);
  return v3_(d.x * c + d.z * sn, d.y, -d.x * sn + d.z * c);
}

/* ═════════════ CUT_WINDOWS: the storefront, and the duck battalion ═════════════ */
static void windows_build(void) {
  intro_frame(3);
  MO_AT = pv(0.9f, 0, 0); LU_AT = pv(-0.9f, 0, -0.3f);
  say("LUIS", "My props are chewed. I clipped a printer gantry on the way out.", 1.4f);       /* 0 */
  say("MOHEEB", "The front windows are full of robots. Robots have spare props.", 0.4f);       /* 1 */
  say("ARMY MAN", "Duck battalion, fall in! Eyes front!", 0.5f);                               /* 2 */
  say("LUIS", "They've enlisted the ducks.", 0.5f);                                            /* 3 */
  say("MOHEEB", "The ducks were always going to pick a side.", 0.4f);                          /* 4 */
  C.tail = 2.2f;
  finish_lines();
  const lab_box_t *dk = &LM_DUCK_RISER[1], *ct = &LM_ROBOT_COUNTER[0];
  v3 ducks = v3_((dk->min[0] + dk->max[0]) * 0.5f, dk->max[1] + 1, (dk->min[2] + dk->max[2]) * 0.5f);
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 40);
  shot(1, 0, loc(v3_(ct->max[0] + 14, 22, ct->min[2] + 4)), loc(v3_(ct->min[0] + 6, ct->max[1] + 2, ct->min[2] + 6)), 52,
       loc(v3_(ct->max[0] + 12, 23, ct->min[2] + 6)), loc(v3_(ct->min[0] + 6, ct->max[1] + 2, ct->min[2] + 6)), 48, 0.02f, 1);
  shot(2, 0, loc(v3_(ducks.x + 40, 24, ducks.z + 2)), loc(ducks), 48, loc(v3_(ducks.x + 36, 22, ducks.z + 2)), loc(ducks), 44, 0.02f, 0);
  hold(3, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  hold(4, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 38);
  resolve_shots();
}
static void windows_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, 0.2f, -0.2f, t > lt0(4) ? "Celebration" : NULL, t > lt0(3) && t < lt0(3) + 1.2f ? "Surprise" : NULL);
  /* the battalion: six army men in a rank in front of the duck risers, saluting on the order */
  const lab_box_t *dk = &LM_DUCK_RISER[0];
  for (int i = 0; i < 6; i++) {
    v3 p = v3_(dk->min[0] + 2 + (float)i * 2.6f, dk->max[1] + 0.5f, dk->min[2] - 1.5f);
    man(i, p, DS_PI / 2);
    if (t > lt0(2) + 0.3f) act_pose(&CAST[i], "Victory", 0.35f);
    else act_play(&CAST[i], "Idle", 0.7f + 0.05f * i);
  }
}

/* ═════════════ CUT_KITCHEN: the long way round ═════════════ */
static void kitchen_build(void) {
  intro_frame(6);
  MO_AT = pv(-0.9f, 0, 0); LU_AT = pv(0.9f, 0, -0.3f);
  say("LUIS", "Our batteries won't make the climb to the panel. Not from here.", 1.4f);           /* 0 */
  say("MOHEEB", "There's a charger in the kitchen. Sixty seconds and we're full.", 0.4f);         /* 1 */
  say("ARMY MAN", "Raiding party, to the kitchen! Secure the snacks!", 0.5f);                     /* 2 */
  say("LUIS", "They're after the fridge.", 0.5f);                                                 /* 3 */
  say("MOHEEB", "Nobody touches the labelled yogurt.", 0.4f);                                     /* 4 */
  C.tail = 2.2f;
  finish_lines();
  const lab_box_t *kt = &LM_KITCHEN_TABLE[0];
  v3 tb = v3_((kt->min[0] + kt->max[0]) * 0.5f, kt->max[1] + 1, (kt->min[2] + kt->max[2]) * 0.5f);
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 40);
  shot(1, 0, pv(-0.2f, 1.7f, -2.6f), pv(0, 2.5f, 60), 50, pv(-0.2f, 1.8f, -2.0f), pv(0, 3.0f, 60), 46, 0.02f, 1);
  shot(2, 0, loc(v3_(tb.x - 14, tb.y + 2, tb.z + 4)), loc(tb), 48, loc(v3_(tb.x - 11, tb.y + 1.5f, tb.z + 3)), loc(tb), 44, 0.03f, 0);
  hold(3, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  hold(4, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 38);
  resolve_shots();
}
static void kitchen_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, -0.15f, 0.2f, t > lt0(4) ? "Punch_Jab" : NULL, NULL);
  /* the raiding party on the kitchen table */
  const lab_box_t *kt = &LM_KITCHEN_TABLE[0];
  for (int i = 0; i < 4; i++) {
    v3 p = v3_(kt->min[0] + 4 + (float)(i % 2) * 6, kt->max[1] + 0.5f, kt->min[2] + 5 + (float)(i / 2) * 6);
    man(i, p, (float)i * 1.4f);
    act_play(&CAST[i], i == 1 ? "Walk_Carry" : t > lt0(2) ? "PickUp" : "Idle", 0.8f);
  }
}

/* ═════════════ CUT_METALFAB: the supply dump ═════════════ */
static void metalfab_build(void) {
  intro_frame(7);
  MO_AT = pv(-0.9f, 0, 0); LU_AT = pv(0.9f, 0, -0.3f);
  say("MOHEEB", "Metal fab bay. Welders, an anvil, and a lot of cardboard that isn't ours.", 1.4f);   /* 0 */
  say("LUIS", "Supply dump. Spare sprues, spare rifles, spare everything.", 0.4f);                     /* 1 */
  say("ARMY MAN", "Guard the crates! Nobody touches the spare parts!", 0.5f);                          /* 2 */
  say("LUIS", "Wreck the dump and the General runs out of reinforcements.", 0.5f);                    /* 3 */
  say("MOHEEB", "The hot work permit says we're allowed.", 0.4f);                                      /* 4 */
  C.tail = 2.2f;
  finish_lines();
  const lab_box_t *wt = &LM_WELD_TABLE[0];
  v3 wc = v3_((wt->min[0] + wt->max[0]) * 0.5f, 3, (wt->min[2] + wt->max[2]) * 0.5f - 16);   /* the first crate, by the weld table */
  shot(-1, 0, pv(0.3f, 0.6f, 4.8f), pv(0, 1.2f, 0), 50, pv(0.1f, 0.9f, 3.4f), pv(0, 1.3f, 0), 46, 0.02f, 1);
  hold(0, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 40);
  shot(1, 0, pv(-0.2f, 1.7f, -2.6f), loc(wc), 46, pv(-0.2f, 1.8f, -2.0f), loc(wc), 40, 0.02f, 1);
  shot(2, 0, loc(v3_(wc.x + 9, 4, wc.z - 8)), loc(wc), 40, loc(v3_(wc.x + 7, 3.5f, wc.z - 6)), loc(wc), 36, 0.03f, 0);
  hold(3, 0, v3_add(LU_AT, pv(1.3f, 1.3f, 2.4f)), mk_head(LU_AT), 38);
  hold(4, 0, v3_add(MO_AT, pv(-1.2f, 1.3f, 2.4f)), mk_head(MO_AT), 38);
  resolve_shots();
}
static void metalfab_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  heroes_at_marks(o, -0.15f, 0.2f, NULL, NULL);
  /* the crate guards, around the first stack */
  const lab_box_t *wt = &LM_WELD_TABLE[0];
  v3 wc = v3_((wt->min[0] + wt->max[0]) * 0.5f - 6, 0, (wt->min[2] + wt->max[2]) * 0.5f - 16);
  for (int i = 0; i < 4; i++) {
    float a = (float)i * DS_PI / 2 + 0.4f;
    man(i, v3_(wc.x + sinf(a) * 3.2f, 0, wc.z + cosf(a) * 3.2f), a);
    act_play(&CAST[i], t > lt0(2) ? "Shoot_OneHanded" : "Idle", 0.6f);
  }
}

/* ═════════════ the director ═════════════ */
static void restore_all(void) {
  restore_army();
  cast_hide();
  props_hide();
  root->visible = false;
  have_hint = false;
}

void cutscene_init(t3_scene *scene) {
  scn = scene;
  root = t3_object_new();
  t3_object_add(&scene->base, root); t3_release(root);
  props_build();
  for (int i = 0; i < NCAST; i++) act_load(&CAST[i], (i % 4 == 3) ? C_TAN : C_ARMY);
  act_load(&SARGE, C_TAN);
  act_load(&GEN, 0x5a5e34);   /* olive drab */
  restore_all();
}

void cutscene_hint(v3 player, float yaw, v3 buddy) { (void)yaw; (void)buddy; hint_player = player; have_hint = true; }

void cutscene_start(cut_id id, hero_id player) {
  if (!scn || C.active || (unsigned)id >= CUT_COUNT) return;
  cut_reset();
#ifdef CINE_FORCE
  /* testing: whatever was asked for, run this one; the boss scenes stand where their arenas begin */
  id = CINE_FORCE;
  if (id == CUT_SERGEANT) { have_hint = true; hint_player = v3_(LAB_AREAS[AREA_WOODSHOP].min[0] + 6, 0, LAB_AREAS[AREA_WOODSHOP].min[2] + 1); }
  if (id == CUT_GENERAL) { float y; have_hint = true; hint_player = v3_add(mission_start(8, &y), v3_(-12, 0, 6)); }
#endif
  C.id = id; C.player = player < HERO_COUNT ? player : HERO_MOHEEB;
  switch (id) {
    case CUT_PROLOGUE: prologue_build(); C.act = prologue_act; break;
    case CUT_STORAGE: storage_build(); C.act = storage_act; break;
    case CUT_PRINTFARM: printfarm_build(); C.act = printfarm_act; break;
    case CUT_COWORKING: coworking_build(); C.act = coworking_act; break;
    case CUT_WOODSHOP: woodshop_build(); C.act = woodshop_act; break;
    case CUT_SERGEANT: sergeant_build(); C.act = sergeant_act; break;
    case CUT_MACHINESHOP: machineshop_build(); C.act = machineshop_act; break;
    case CUT_GENERAL: general_build(); C.act = general_act; break;
    case CUT_ENDING: ending_build(); C.act = ending_act; break;
    case CUT_WINDOWS: windows_build(); C.act = windows_act; break;
    case CUT_KITCHEN: kitchen_build(); C.act = kitchen_act; break;
    case CUT_METALFAB: metalfab_build(); C.act = metalfab_act; break;
    default: return;
  }
  memset(cued, 0, sizeof cued);
  C.last_line = -1; C.active = true; C.t = 0; C.skip = 0; C.armed = false; C.last_chars = 0;
  C.prep = 0;   /* the lines are synthesised one a frame in cutscene_update, not all at once here */
#ifdef CINE_T0
  C.t = CINE_T0;
#endif
#ifdef CINE_DEBUG
  for (int i = 0; i < C.nl; i++) ds_log("cine %d line %d t0 %.1f dur %.1f", id, i, C.L[i].t0, C.L[i].dur);
  ds_log("cine %d length %.1f, %d shots", id, C.end, C.ns);
#endif
  root->visible = true;
  hide_army();
  cast_hide();
  props_hide();
}

bool cutscene_active(void) { return C.active; }

vo_speaker cutscene_voice_for(const char *who) {
  if (!who) return VO_ALERT;
  if (!strcmp(who, "MOHEEB")) return VO_MOHEEB;
  if (!strcmp(who, "LUIS")) return VO_LUIS;
  if (strstr(who, "SERGEANT")) return VO_SERGEANT;
  if (strstr(who, "GENERAL")) return VO_GENERAL;
  if (strstr(who, "ARMY")) return VO_ARMY;
  return VO_ALERT;
}

/* in the lab a shelf or a wall can sit between the camera and its subject: pull the camera in front of it */
static void cam_clear(v3 *cam, v3 target) {
  v3 d = v3_sub(*cam, target);
  float l = v3_len(d);
  if (l < 0.5f) return;
  d = v3_scale(d, 1 / l);
  float t; v3 n;
  if (world_raycast(target, d, l, &t, &n) && t < l) *cam = v3_add(target, v3_scale(d, fmaxf(t - 0.2f, 0.3f)));
}
static float shake_n(float t, float k) { return sinf(t * 1.7f * k + 1) * 0.6f + sinf(t * 3.1f * k + 4) * 0.3f + sinf(t * 5.3f * k) * 0.1f; }

bool cutscene_update(float dt, const intent_t *I, cine_view_t *o) {
  if (!C.active) return false;
  memset(o, 0, sizeof *o);
  if (C.prep < C.nl) { voice_prepare(cutscene_voice_for(C.L[C.prep].who), spoken(&C.L[C.prep])); C.prep++; }
  /* skip: hold START, Enter or A; a tap does nothing, and the press that began the scene is ignored */
  bool held = I->jump_held || input_key_down(WC_KEY_ENTER) || input_start_held();
  if (!held) C.armed = true;
  if (C.armed && held) C.skip += dt / SKIP_HOLD; else C.skip -= dt * 2.5f;
  C.skip = ds_clamp(C.skip, 0, 1);
  /* a line's voice may run past the time its caption was given: hold the
   * clock at the end of the line until it finishes, rather than cut it off */
  bool hold_clock = false;
  if (C.last_line >= 0 && voice_busy()) {
    const line_t *l = &C.L[C.last_line];
    if (C.t + dt >= l->t0 + l->dur - 0.4f) hold_clock = true;
  }
  if (!hold_clock) C.t += dt;
  if (C.t >= C.end || C.skip >= 1) { C.active = false; voice_stop(); restore_all(); return false; }
  float t = C.t;

  /* the camera: the shot in force, interpolated and shaken */
  int k = 0;
  for (int i = 0; i < C.ns; i++) if (C.S[i].s0 <= t) k = i;
  shot_t *s = &C.S[k];
#ifdef CINE_DEBUG
  { static int last_k = -1, frame; frame++;
    if (C.t < 0.05f) last_k = -1;
    if (k != last_k) { last_k = k; ds_log("cine shot %d at t %.2f, frame %d", k, C.t, frame); } }
#endif
  float s1 = k + 1 < C.ns ? C.S[k + 1].s0 : C.end;
  float u = ds_clamp((t - s->s0) / fmaxf(0.1f, s1 - s->s0), 0, 1);
  if (s->ease) u = sm(u);
  v3 p = v3_lerp(s->p0, s->p1, u), a = v3_lerp(s->a0, s->a1, u);
  float sh = s->shake;
  p.x += shake_n(t, 1.0f) * sh; p.y += shake_n(t + 9, 1.3f) * sh * 0.7f; p.z += shake_n(t + 5, 0.9f) * sh;
  a.x += shake_n(t + 2, 1.1f) * sh * 1.5f; a.y += shake_n(t + 7, 1.2f) * sh * 1.5f;
  o->cam_pos = W(p); o->cam_target = W(a);
  if (C.ray) cam_clear(&o->cam_pos, o->cam_target);
  o->fov = ds_lerp(s->f0, s->f1, u);
  o->lights = 1;

  cast_tick(dt);
  if (C.act) C.act(t, dt, o);

  /* captions */
  int li = -1;
  for (int i = 0; i < C.nl; i++) if (C.L[i].t0 <= t) li = i;
  if (li >= 0 && t < C.L[li].t0 + C.L[li].dur - 0.35f) {
    line_t *l = &C.L[li];
    int ch = (int)((t - l->t0) * TYPE_CPS);
    if (ch > l->n) ch = l->n;
    o->speaker = l->who; o->text = l->text; o->chars = ch;
    if (li != C.last_line) { C.last_line = li; C.last_chars = 0; voice_say(cutscene_voice_for(l->who), spoken(l)); }
    for (int c = C.last_chars + 1; c <= ch; c++)
      if (c % 3 == 0 && l->text[c - 1] != ' ') sfx(SFX_UI, 0, 0.12f);   /* a soft tick every few characters */
    C.last_chars = ch;
  }

  /* the bars stay full; the fade is in and out of black at the ends */
  float fade = fmaxf(1 - seg(t, 0.1f, 0.1f + C.fi), seg(t, C.end - C.fo, C.end - 0.05f));
  o->fade = fmaxf(o->fade, fade);
  o->letterbox = 1;
  o->skip = C.skip;
  return true;
}

/* ═════════════ the quips: the death screen and the loading card ═════════════ */
const char *story_quip(int seed) {
  static const char *const Q[] = {
    "Please return the scale to 1:1 when you are done.",
    "Clean your bench. Including the army on it.",
    "Label everything. Especially yourself.",
    "Do-ocracy: if you see an army, you are the one who deals with it.",
    "Leave it better than you found it.",
    "No running in the shop. Jetpacks are a grey area.",
    "Ask before borrowing another member's tools. Or their drone.",
    "The buddy system is not optional.",
    "Safety glasses are required past this point.",
    "If it isn't labelled, it's a free part. The army agrees.",
    "Every print fails once.",
    "Small problem.",
  };
  return Q[(unsigned)seed % (sizeof Q / sizeof Q[0])];
}
