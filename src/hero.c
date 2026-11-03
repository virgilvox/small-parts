/* hero.c: see hero.h. The Coffee & Kill rigs (moheeb.glb, luis.glb) with
 * their baked clips (rig_*.bin, through anim.c's port of three.js's
 * AnimationMixer), and everything the toy-sized heroes carry built from
 * boxes and cylinders: the four weapons and the drone-motor jetpack.
 *
 * The aim layer runs after the mixer: it turns the upper arms and forearms in
 * world space so their bone axis (+y, toward the child bone) points down the
 * aim, blended over the clip, and the weapon prop is placed in the hero's own
 * frame at the right hand pointing along the aim, so it always points where
 * the shot goes whatever the clip is doing. */
#include "hero.h"
#include "anim.h"
#include "weapon.h"
#include <stdlib.h>
#include <string.h>

struct toy {
  hero_id who;
  t3_object *root, *model, *gun, *pack, *props[2], *exhaust[2], *weapons[WP_COUNT];
  t3_gltf *gltf;
  ck_rig *rig;
  ck_mixer mixer;
  ck_action *cur;
  int cur_clip;
  t3_object *b_spine, *b_head, *b_rarm, *b_rfore, *b_rhand, *b_larm, *b_lfore;
  float model_scale;        /* the model's own height to HERO_H */
  v3 pos; float yaw, size;
  float aim_pitch, aim_amt, jet, prop_spin;
  int weapon;
  v3 muzzle, head;
};

static const char *NAMES[HERO_COUNT] = {"MOHEEB", "LUIS"};
const char *toy_name(hero_id w) { return (unsigned)w < HERO_COUNT ? NAMES[w] : ""; }

static t3_mesh *part(t3_geometry *g, uint32_t color, float rough, float emissive) {
  t3_material *m = t3_mesh_standard_material_new(color);
  m->roughness = rough; m->metalness = 0;
  if (emissive > 0) { m->emissive = t3_color_hex(color); m->emissive_intensity = emissive; }
  t3_mesh *me = t3_mesh_new(g, m);
  ((t3_object *)me)->frustum_culled = false;
  return me;
}
static t3_mesh *boxp(t3_object *parent, float w, float h, float d, float x, float y, float z, uint32_t c, float e) {
  t3_mesh *m = part(t3_box_geometry_new(w, h, d, 1, 1, 1), c, 0.7f, e);
  t3_object_set_position(m, x, y, z);
  t3_object_add(parent, m);
  return m;
}
/* a cylinder along z (the barrel axis) */
static t3_mesh *cylz(t3_object *parent, float r, float len, float x, float y, float z, uint32_t c, float e) {
  t3_mesh *m = part(t3_cylinder_geometry_new(r, r, len, 8, 1, false), c, 0.5f, e);
  t3_object_set_rotation(m, DS_PI / 2, 0, 0);
  t3_object_set_position(m, x, y, z);
  t3_object_add(parent, m);
  return m;
}

/* the weapons, in the hand frame: x right, y up, z down the barrel; the grip at the origin.
 * Sizes are for a 1.8 unit hero (a 9 cm person holding a 2 cm glue gun). */
static t3_object *build_weapon(int w) {
  t3_object *o = t3_object_new();
  switch (w) {
    case WP_BANDS:   /* laser-cut plywood repeater: a flat frame, a charred edge, a band stretched over the nose */
      boxp(o, 0.06f, 0.16f, 0.62f, 0, 0.05f, 0.22f, 0xc89b62, 0);
      boxp(o, 0.062f, 0.02f, 0.62f, 0, 0.135f, 0.22f, 0x3a2a1a, 0);
      boxp(o, 0.05f, 0.2f, 0.08f, 0, -0.08f, 0, 0xb4884f, 0);
      boxp(o, 0.08f, 0.03f, 0.03f, 0, 0.1f, 0.53f, 0xd8503a, 0);
      cylz(o, 0.035f, 0.1f, 0, 0.12f, -0.04f, 0x9a7444, 0);
      break;
    case WP_GLUE:    /* hot glue gun: grey body, trigger, a yellow-white stick out the back, a brass nozzle */
      boxp(o, 0.1f, 0.14f, 0.36f, 0, 0.07f, 0.1f, 0x5b6068, 0);
      boxp(o, 0.08f, 0.22f, 0.09f, 0, -0.07f, 0, 0x3c4046, 0);
      cylz(o, 0.03f, 0.22f, 0, 0.09f, -0.16f, C_GOO, 0.2f);
      cylz(o, 0.018f, 0.12f, 0, 0.06f, 0.33f, 0xc8a050, 0);
      break;
    case WP_COIL:    /* coilgun: a copper-wound tube, a capacitor bank under it, a teal charge light */
      cylz(o, 0.03f, 0.8f, 0, 0.1f, 0.3f, 0x8a8f96, 0);
      for (int i = 0; i < 3; i++) cylz(o, 0.055f, 0.1f, 0, 0.1f, 0.12f + i * 0.2f, 0xb8642c, 0);
      for (int i = 0; i < 3; i++) {
        t3_mesh *cap = part(t3_cylinder_geometry_new(0.04f, 0.04f, 0.12f, 8, 1, false), 0x2a3a8a, 0.5f, 0);
        t3_object_set_position(cap, 0, -0.02f, 0.05f + i * 0.1f);
        t3_object_add(o, cap);
      }
      boxp(o, 0.06f, 0.2f, 0.08f, 0, -0.09f, -0.02f, 0x2b2f36, 0);
      boxp(o, 0.03f, 0.03f, 0.03f, 0.04f, 0.16f, -0.02f, C_TEAL, 1.5f);
      break;
    case WP_IRON:    /* soldering iron: a blue grip, a steel shaft, a glowing tip */
      cylz(o, 0.04f, 0.26f, 0, 0, 0.05f, 0x2a5aa8, 0);
      cylz(o, 0.016f, 0.3f, 0, 0, 0.32f, 0xaab0b8, 0);
      cylz(o, 0.012f, 0.08f, 0, 0, 0.5f, 0xff6a2a, 2.0f);
      break;
  }
  return o;
}

/* the jetpack: a LiPo pack between two brushless motors on arms, props on top */
static void build_pack(toy_t *t) {
  t->pack = t3_object_new();
  boxp(t->pack, 0.34f, 0.42f, 0.14f, 0, 0, 0, 0x2b2f36, 0);
  boxp(t->pack, 0.3f, 0.12f, 0.12f, 0, 0.12f, -0.02f, 0xe8c43a, 0.1f);   /* the battery's label */
  for (int s = 0; s < 2; s++) {
    float x = s ? 0.3f : -0.3f;
    boxp(t->pack, 0.28f, 0.04f, 0.05f, x * 0.55f, 0.1f, 0, 0x1d2026, 0);
    t3_mesh *mot = part(t3_cylinder_geometry_new(0.07f, 0.07f, 0.12f, 10, 1, false), 0x9aa2ac, 0.35f, 0);
    t3_object_set_position(mot, x, 0.12f, 0);
    t3_object_add(t->pack, mot);
    t3_object *prop = t3_object_new();
    boxp(prop, 0.46f, 0.012f, 0.05f, 0, 0, 0, s ? 0xff7a1a : 0x2ec4b6, 0);
    t3_object_set_position(prop, x, 0.2f, 0);
    t3_object_add(t->pack, prop);
    t->props[s] = prop;
    t3_mesh *ex = part(t3_cone_geometry_new(0.06f, 0.36f, 8, 1, false), 0xffc06a, 0.5f, 2.5f);
    t3_object_set_rotation(ex, DS_PI, 0, 0);
    t3_object_set_position(ex, x, -0.08f, 0);
    ex->base.visible = false;
    t3_object_add(t->pack, ex);
    t->exhaust[s] = (t3_object *)ex;
  }
  t3_object_add(t->root, t->pack);
}

static void mesh_setup(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  o->frustum_culled = false; o->cast_shadow = false; o->receive_shadow = false;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) {
    t3_material *M = m->materials[i];
    if (!M) continue;
    M->metalness = 0.02f; M->roughness = 0.85f;
    if (M->map) { M->map->mag_filter = T3_NEAREST; M->map->version++; }   /* PS1: point sampled */
    M->version++;
  }
}

toy_t *toy_new(hero_id who, t3_scene *scene) {
  toy_t *t = calloc(1, sizeof *t);
  t->who = who; t->weapon = -1; t->cur_clip = -1; t->size = 1;
  const char *glb = who == HERO_LUIS ? "luis.glb" : "moheeb.glb";
  const char *rig = who == HERO_LUIS ? "rig_luis.bin" : "rig_moheeb.bin";
  t->root = t3_object_new();
  size_t n; uint8_t *b = ds_asset(glb, &n);
  if (b) {
    t->gltf = t3_gltf_parse(b, n, NULL, NULL);
    free(b);
  }
  if (t->gltf) {
    t->model = t->gltf->scene;
    t3_object_add(t->root, t->model);
    t3_object_traverse(t->model, mesh_setup, NULL);
    t->rig = rig_load(rig);
    if (t->rig) mixer_init(&t->mixer, t->rig, t->model);
    t->b_spine = t3_object_get_by_name(t->model, "Spine02");
    t->b_head = t3_object_get_by_name(t->model, "Head");
    t->b_rarm = t3_object_get_by_name(t->model, "RightArm");
    t->b_rfore = t3_object_get_by_name(t->model, "RightForeArm");
    t->b_rhand = t3_object_get_by_name(t->model, "RightHand");
    t->b_larm = t3_object_get_by_name(t->model, "LeftArm");
    t->b_lfore = t3_object_get_by_name(t->model, "LeftForeArm");
  }
  t->model_scale = HERO_H / 1.74f;   /* both models stand 1.74 m as authored */
  for (int w = 0; w < WP_COUNT; w++) {
    t->weapons[w] = build_weapon(w);
    t->weapons[w]->visible = false;
    t3_object_add(t->root, t->weapons[w]);
  }
  build_pack(t);
  t3_object_add(&scene->base, t->root);
  toy_play(t, "Idle_Loop", 1, true);
  return t;
}

void toy_place(toy_t *t, v3 pos, float yaw, float size) {
  t->pos = pos; t->yaw = yaw; t->size = size;
  t3_object_set_position(t->root, pos.x, pos.y, pos.z);
  t3_object_set_rotation(t->root, 0, yaw, 0);
  t3_object_set_scale(t->root, size, size, size);
  if (t->model) t3_object_set_scale(t->model, t->model_scale, t->model_scale, t->model_scale);
}

void toy_visible(toy_t *t, bool on) { t->root->visible = on; }
void toy_jetpack_visible(toy_t *t, bool on) { t->pack->visible = on; }
void toy_set_weapon(toy_t *t, int w) {
  t->weapon = w;
  for (int i = 0; i < WP_COUNT; i++) t->weapons[i]->visible = i == w;
}
void toy_jet(toy_t *t, float thrust) { t->jet = ds_clamp(thrust, 0, 1); }
void toy_aim(toy_t *t, float pitch, float amount) { t->aim_pitch = pitch; t->aim_amt = ds_clamp(amount, 0, 1); }

void toy_play(toy_t *t, const char *clip, float speed, bool loop) {
  if (!t->rig) return;
  int c = rig_clip_index(t->rig, clip);
  if (c < 0) return;
  ck_action *a = mixer_action(&t->mixer, c);
  if (c == t->cur_clip && t->cur) { a->time_scale = speed; return; }
  action_reset(a);
  a->loop = loop ? LOOP_REPEAT : LOOP_ONCE;
  a->clamp_when_finished = !loop;
  a->time_scale = speed;
  action_play(&t->mixer, a);
  if (t->cur) action_cross_fade_from(&t->mixer, a, t->cur, 0.15f);
  else action_set_effective_weight(a, 1);
  t->cur = a; t->cur_clip = c;
}

bool toy_clip_done(const toy_t *t) {
  return t->cur && t->cur->loop == LOOP_ONCE && t->cur->time >= t->mixer.rig->clips[t->cur->clip].dur - 1e-3f;
}

/* ── the aim layer ── */
static t3_quat world_rot(t3_object *o) {
  t3_mat4 m = o->matrix_world;
  for (int c = 0; c < 3; c++) {
    float l = sqrtf(m.e[c * 4] * m.e[c * 4] + m.e[c * 4 + 1] * m.e[c * 4 + 1] + m.e[c * 4 + 2] * m.e[c * 4 + 2]);
    if (l > 1e-8f) for (int r = 0; r < 3; r++) m.e[c * 4 + r] /= l;
  }
  return t3_quat_from_rotation_matrix(&m);
}
static t3_quat qinv(t3_quat q) { t3_quat r = {-q.x, -q.y, -q.z, q.w}; return r; }
static t3_quat from_to(t3_vec3 a, t3_vec3 b) {
  float r = t3_vec3_dot(a, b) + 1;
  t3_quat q;
  if (r < 1e-6f) { q = fabsf(a.x) > fabsf(a.z) ? (t3_quat){-a.y, a.x, 0, 0} : (t3_quat){0, -a.z, a.y, 0}; }
  else { t3_vec3 c = t3_vec3_cross(a, b); q = (t3_quat){c.x, c.y, c.z, r}; }
  return t3_quat_normalize(q);
}
/* turn bone o so its +y axis points along dir (world), by amount */
static void point_bone(t3_object *o, t3_vec3 dir, float amount) {
  if (!o || !o->parent || amount <= 0) return;
  t3_object_update_matrix_world(o, true);
  t3_quat qw = world_rot(o), qp = world_rot(o->parent);
  t3_vec3 axis = t3_vec3_normalize(t3_vec3_apply_quat(t3_v3(0, 1, 0), qw));
  t3_quat want = t3_quat_multiply(from_to(axis, t3_vec3_normalize(dir)), qw);
  t3_quat nw = t3_quat_slerp(qw, want, amount);
  t3_object_set_quaternion(o, t3_quat_normalize(t3_quat_multiply(qinv(qp), nw)));
  t3_object_update_matrix_world(o, true);
}

void toy_update(toy_t *t, float dt) {
  if (t->rig) mixer_update(&t->mixer, dt);
  t3_object_update_matrix_world(t->root, true);
  float cy = cosf(t->yaw), sy = sinf(t->yaw), cp = cosf(t->aim_pitch), sp = sinf(t->aim_pitch);
  t3_vec3 fwd = t3_v3(sy * cp, sp, cy * cp), right = t3_v3(cy, 0, -sy);
  if (t->aim_amt > 0.01f && t->weapon >= 0) {
    /* lean the chest a little into the pitch, then both arms down the aim; the
     * support arm angles in from the left shoulder toward the barrel */
    if (t->b_spine) {
      t3_object_update_matrix_world(t->b_spine, true);
      t3_quat qw = world_rot(t->b_spine), qp = world_rot(t->b_spine->parent);
      t3_quat bend = t3_quat_from_axis_angle(right, -t->aim_pitch * 0.35f * t->aim_amt);
      t3_object_set_quaternion(t->b_spine, t3_quat_normalize(t3_quat_multiply(qinv(qp), t3_quat_multiply(bend, qw))));
    }
    float one_hand = t->weapon == WP_IRON ? 1 : 0;
    point_bone(t->b_rarm, t3_vec3_add(fwd, t3_vec3_scale(right, -0.15f)), t->aim_amt);
    point_bone(t->b_rfore, fwd, t->aim_amt);
    if (!one_hand) {
      point_bone(t->b_larm, t3_vec3_add(fwd, t3_vec3_scale(right, 0.55f)), t->aim_amt * 0.9f);
      point_bone(t->b_lfore, t3_vec3_add(fwd, t3_vec3_scale(right, 0.75f)), t->aim_amt * 0.9f);
    }
  }
  /* the weapon at the right hand, in the root's frame, pointing down the aim */
  float s = t->size;
  v3 hand = t->pos;
  if (t->b_rhand) { t3_object_update_matrix_world(t->b_rhand, true); t3_vec3 w = t3_object_get_world_position(t->b_rhand); hand = v3_(w.x, w.y, w.z); }
  if (t->weapon >= 0) {
    t3_object *g = t->weapons[t->weapon];
    v3 rel = v3_scale(v3_sub(hand, t->pos), 1 / s);
    float lx = rel.x * cy - rel.z * sy, lz = rel.x * sy + rel.z * cy;
    t3_object_set_position(g, lx, rel.y, lz);
    float pitch = t->aim_amt > 0.01f ? t->aim_pitch * t->aim_amt : 0.35f;   /* lowered when not aiming */
    t3_object_set_rotation(g, -pitch, 0, 0);
    t3_object_update_matrix_world(g, true);
    t3_vec3 tip = t3_vec3_apply_mat4(t3_v3(0, 0.08f, t->weapon == WP_COIL ? 0.72f : 0.6f), &g->matrix_world);
    t->muzzle = v3_(tip.x, tip.y, tip.z);
  } else t->muzzle = hand;
  /* the pack on the back, between the shoulders */
  t3_object_set_position(t->pack, 0, HERO_H * 0.68f, -0.22f);
  t->prop_spin += dt * (8 + t->jet * 70);
  for (int i = 0; i < 2; i++) {
    t3_object_set_rotation(t->props[i], 0, i ? t->prop_spin : -t->prop_spin, 0);
    t->exhaust[i]->visible = t->jet > 0.05f && t->pack->visible;
    float l = 0.5f + t->jet * (0.8f + 0.3f * sinf(t->prop_spin * 3 + i));
    t3_object_set_scale(t->exhaust[i], 1, l, 1);
  }
  if (t->b_head) { t3_object_update_matrix_world(t->b_head, true); t3_vec3 w = t3_object_get_world_position(t->b_head); t->head = v3_(w.x, w.y, w.z); }
  else t->head = v3_add(t->pos, v3_(0, HERO_H * 0.92f * s, 0));
}

v3 toy_muzzle(const toy_t *t) { return t->muzzle; }
v3 toy_head(const toy_t *t) { return t->head; }
