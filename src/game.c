/* game.c: the state machine, the player and the buddy, the camera, and the
 * glue between the modules. world.c owns the lab, mission.c the levels,
 * enemy.c the army, weapon.c the player's weapons, hero.c the two heroes'
 * bodies, fx.c the particles, hud.c everything 2D, audio.c the sound. */
#include "game.h"
#include "input.h"
#include "audio.h"
#include "world.h"
#include "mission.h"
#include "enemy.h"
#include "weapon.h"
#include "hero.h"
#include "fx.h"
#include "hud.h"
#include "cutscene.h"
#include "voice.h"
#include <stdlib.h>
#include <string.h>
#include "gen/lab_areas.h"

typedef enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_DEAD, ST_CLEAR, ST_END, ST_CUT } state_t;
typedef enum { AFTER_PLAY, AFTER_BOSS, AFTER_END } after_t;

static t3_scene *scene;
static t3_camera *cam;
static t3_light *hemi, *moon;
static state_t st;
static float st_t;
static int level;
static hud_t H;
static after_t cut_after;
static bool cut_seen[CUT_COUNT];
static bool boss_cut_done, boss_dead_on;
static float boss_dead_t;
static cine_view_t CV;
static int radio_prep = 99;
static float reel_t0;          /* the title reel's clock and its scene */
static int reel_cur = -1;   /* the next walkie line to synthesise ahead of time */

/* ── the player ── */
#define HP_MAX 100.f
#define RUN_SPEED 7.0f
#define AIM_SPEED 3.6f
#define JUMP_V 9.5f
#define GRAVITY 24.f
#define JET_ACCEL 40.f        /* against GRAVITY: a net climb of 16 */
#define JET_MAX_UP 9.f
#define JET_DRAIN 0.34f       /* battery per second of thrust: about three seconds from full */
#define JET_CHARGE 0.55f      /* per second on the ground, after JET_DELAY */
#define JET_DELAY 0.35f
#define DODGE_T 0.28f
#define DODGE_V 15.f

static hero_id who = HERO_MOHEEB;
static toy_t *heroes[HERO_COUNT];
static toy_t *me, *pal;
static v3 pos, vel;
static float yaw;
static bool grounded, jetting;
static float hp, jet, jet_delay, dodge_t, dodge_cd, hurt_iframes, combat_t, swing_t;
static v3 dodge_dir;
static int kills, score;
static float level_time;
static v3 checkpoint;

/* ── the buddy ── */
static v3 bpos, bvel;
static float byaw, bhp, bdown_t, bfire_cd, bjet;
static bool bgrounded, bjetting;

/* ── the camera ── */
static float cam_yaw, cam_pitch = -0.15f;
static v3 cam_pos, cam_fwd;
static bool first_person, scoped;
static int zoom = 1;
static float fov = 70, shake;
static const float ZOOM_FOV[4] = {70, 32, 16, 8};
#define CAM_DIST 3.4f

static void banner(const char *s) { if (s) { H.banner = s; H.banner_t = 2.2f; } }
static const char *const SPOKEN[] = {"CHECKPOINT", "BATTERY LOW", "OUT OF GLUE", "OUT OF CHARGE"};
static void alert(const char *s) {
  banner(s);
  if (!voice_busy()) voice_say(VO_ALERT, s);
}

static void save_write(void) {
  if (DS_SAVE.magic != DS_SAVE_MAGIC) {
    memset(&DS_SAVE, 0, sizeof DS_SAVE);
    DS_SAVE.magic = DS_SAVE_MAGIC; DS_SAVE.music = 1; DS_SAVE.shake = 1;
  }
}

static void pick_hero(hero_id h) {
  who = h;
  me = heroes[who]; pal = heroes[who ^ 1];
  DS_SAVE.hero = (uint8_t)who;
}

/* a spot beside a hero facing y: side units to the right (negative: left), a little ahead,
 * so the buddy stands in view rather than between the shoulder camera and the player */
static v3 beside(v3 p, float y, float side) {
  return v3_add(p, v3_(cosf(y) * side + sinf(y) * 0.6f, 0, -sinf(y) * side + cosf(y) * 0.6f));
}

static void reset_player(v3 at, float face) {
  pos = at; vel = v3_(0, 0, 0); yaw = face;
  hp = HP_MAX; jet = 1; jet_delay = 0; dodge_t = dodge_cd = 0; hurt_iframes = 1.5f;
  grounded = false; jetting = false;
  bpos = beside(at, face, -1.4f); bvel = v3_(0, 0, 0); byaw = face; bhp = HP_MAX; bdown_t = 0; bjet = 1;
  cam_yaw = face + DS_PI; cam_pitch = -0.15f;
  cam_fwd = v3_(-sinf(cam_yaw), 0, -cosf(cam_yaw));
}

/* run a cutscene once per session, then carry on as `after` says */
static void play_cut(cut_id id, after_t after) {
  if (cut_seen[id]) {
    if (after == AFTER_BOSS) enemy_boss_wake();
    if (after == AFTER_END) { st = ST_END; st_t = 0; music_mood(MOOD_TITLE); }
    return;
  }
  cut_seen[id] = true;
  cutscene_hint(pos, yaw, bpos);
  cutscene_start(id, who);
  if (!cutscene_active()) {
    cut_seen[id] = false;
    if (after == AFTER_BOSS) enemy_boss_wake();
    if (after == AFTER_END) { st = ST_END; st_t = 0; }
    return;
  }
  cut_after = after;
  st = ST_CUT; st_t = 0; scoped = false;
  memset(&CV, 0, sizeof CV);
  music_mood(MOOD_CINE);
}

static void cut_finish(void) {
  st_t = 0;
  switch (cut_after) {
    case AFTER_PLAY: st = ST_PLAY; music_mood(MOOD_EXPLORE); break;
    case AFTER_BOSS: st = ST_PLAY; enemy_boss_wake(); music_mood(MOOD_BOSS); break;
    case AFTER_END: st = ST_END; music_mood(MOOD_TITLE); break;
  }
  for (int i = 0; i < HERO_COUNT; i++) { toy_visible(heroes[i], true); toy_jetpack_visible(heroes[i], true); }
}

static void start_level(int l) {
  level = l;
  fx_clear();
  weapon_clear();
  enemy_clear();
  mission_load(level);
  weapon_reset(level);
  radio_prep = 0;
  float face; v3 at = mission_start(level, &face);
  checkpoint = at;
  reset_player(at, face);
  kills = 0; score = 0; level_time = 0;
  scoped = false; zoom = 1;
  enemy_take_score();
  st = ST_PLAY; st_t = 0;
  H.radio_t = 0;
  boss_cut_done = false; boss_dead_on = false; boss_dead_t = 0;
  music_mood(MOOD_EXPLORE);
  int intro = mission_intro_cut(level);
  if (intro >= 0) play_cut((cut_id)intro, AFTER_PLAY);
}

static void to_title(void) {
  save_write();
  reel_t0 = (float)ds_now(); reel_cur = -1;
  st = ST_TITLE; st_t = 0;
  level = DS_SAVE.level_reached < MISSION_N ? DS_SAVE.level_reached : MISSION_N - 1;
  fx_clear(); weapon_clear(); enemy_clear();
  float face; v3 at = mission_start(0, &face);
  reset_player(at, face);
  H.menu_sel = 0;
  music_mood(MOOD_TITLE);
}

void game_init(void) {
  save_write();
  scene = t3_scene_new();
  scene->background = t3_color_hex(C_SKY); scene->has_background = true;
  scene->fog = t3_fog_linear(C_SKY, 90, 520);
  cam = t3_perspective_camera_new(70, (float)DS_W / DS_H, 0.06f, 1200);
  hemi = t3_hemisphere_light_new(0xe6eaff, 0x4a3c2c, 1.5f);
  t3_object_add(&scene->base, hemi);
  moon = t3_directional_light_new(0xfff0d8, 1.6f);
  t3_object_set_position(moon, -200, 400, 120);
  t3_object_add(&scene->base, moon);
  world_init(scene);
  for (int i = 0; i < HERO_COUNT; i++) heroes[i] = toy_new((hero_id)i, scene);
  pick_hero(DS_SAVE.hero < HERO_COUNT ? (hero_id)DS_SAVE.hero : HERO_MOHEEB);
  first_person = DS_SAVE.first_person;
  enemy_init(scene);
  weapon_init(scene);
  mission_init(scene);
  cutscene_init(scene);
  for (unsigned i = 0; i < sizeof SPOKEN / sizeof SPOKEN[0]; i++) voice_prepare(VO_ALERT, SPOKEN[i]);
  fx_init(scene);
  hud_init();
  audio_set_music(DS_SAVE.music);
  to_title();
}

static void hurt(float dmg, v3 from) {
  if (dmg <= 0 || dodge_t > 0 || hurt_iframes > 0 || st != ST_PLAY) return;
  hp -= dmg;
  hurt_iframes = 0.3f;
  combat_t = 4;
  H.hurt = 0.5f;
  v3 to = v3_sub(from, pos);
  H.hurt_angle = ds_wrap_pi(atan2f(to.x, to.z) - atan2f(cam_fwd.x, cam_fwd.z));
  shake = fmaxf(shake, 0.2f);
  sfx(SFX_HURT, 0, 1);
  if (hp <= 0) {
    hp = 0; st = ST_DEAD; st_t = 0; scoped = false;
    toy_play(me, "Hit_Head", 0.7f, false);
    H.quip = story_quip((int)(level_time * 7) + kills);
    music_mood(MOOD_SILENT);
  }
}

static void update_camera(float dt, const intent_t *I) {
  float inv = DS_SAVE.invert_y ? -1.f : 1.f;
  float sens = scoped ? 1.5f / (float)(1 << (zoom - 1)) : 2.8f;
  cam_yaw -= I->ax * sens * dt;
  cam_pitch = ds_clamp(cam_pitch + I->ay * inv * sens * 0.7f * dt, -1.35f, 1.3f);
  cam_fwd = v3_(-sinf(cam_yaw) * cosf(cam_pitch), sinf(cam_pitch), -cosf(cam_yaw) * cosf(cam_pitch));
  v3 right = v3_norm(v3_cross(cam_fwd, v3_(0, 1, 0)));
  v3 want;
  float want_fov = ZOOM_FOV[0];
  if (scoped) { want = v3_add(pos, v3_(0, HERO_H * 0.9f, 0)); want_fov = ZOOM_FOV[zoom]; }
  else if (first_person) { want = v3_add(pos, v3_(0, HERO_H * 0.92f, 0)); want = v3_add(want, v3_scale(v3_(cam_fwd.x, 0, cam_fwd.z), 0.15f)); }
  else {
    /* over the right shoulder, pulled in when something is behind */
    v3 focus = v3_add(v3_add(pos, v3_(0, HERO_H * 0.85f, 0)), v3_scale(right, 0.55f));
    float dist = CAM_DIST + (jetting ? 0.8f : 0);
    v3 back = v3_scale(cam_fwd, -1);
    float t; v3 n;
    if (world_raycast(focus, back, dist + 0.3f, &t, &n)) dist = fmaxf(0.35f, t - 0.3f);
    want = v3_add(focus, v3_scale(back, dist));
    want_fov += dodge_t > 0 ? 6 : 0;
  }
  fov = ds_damp(fov, want_fov, scoped ? 18 : 10, dt);
  cam_pos = (scoped || first_person) ? want : v3_lerp(cam_pos, want, 1 - expf(-18 * dt));
  v3 p = cam_pos;
  if (shake > 0 && DS_SAVE.shake) p = v3_add(p, v3_(ds_rr(-1, 1) * shake * 0.15f, ds_rr(-1, 1) * shake * 0.15f, ds_rr(-1, 1) * shake * 0.15f));
  shake = fmaxf(0, shake - dt * 1.5f);
  cam->fov = fov;
  t3_camera_update_projection_matrix(cam);
  t3_object_set_position(cam, p.x, p.y, p.z);
  t3_object_look_at(cam, p.x + cam_fwd.x, p.y + cam_fwd.y, p.z + cam_fwd.z);
  audio_listener(p.x, p.y, p.z, cam_yaw);
}

/* the buddy: keeps to the player's left shoulder, jets up after them, shoots
 * what is nearest and in sight, and gets back up a few seconds after a fall */
static void update_buddy(float dt) {
  if (bdown_t > 0) {
    bdown_t -= dt;
    bvel.x = bvel.z = 0; bvel.y -= GRAVITY * dt;
    world_move(&bpos, &bvel, HERO_R, HERO_H, dt, &bgrounded);
    if (bdown_t <= 0) { bhp = HP_MAX * 0.6f; toy_play(pal, "KipUp", 1, false); }
    toy_jet(pal, 0);
    return;
  }
  v3 goal = beside(pos, yaw, -2.2f);
  v3 to = v3_sub(goal, bpos);
  float flat = sqrtf(to.x * to.x + to.z * to.z);
  if (flat > 60 || (st == ST_PLAY && v3_len(v3_sub(pos, bpos)) > 80)) {   /* lost: catch up out of sight */
    bpos = v3_add(beside(pos, yaw, -1.6f), v3_(0, 0.2f, 0)); bvel = v3_(0, 0, 0); return;
  }
  float sp = flat > 6 ? RUN_SPEED * 1.15f : flat > 1.2f ? RUN_SPEED * 0.7f : 0;
  v3 dir = flat > 0.01f ? v3_(to.x / flat, 0, to.z / flat) : v3_(0, 0, 0);
  bvel.x = ds_damp(bvel.x, dir.x * sp, bgrounded ? 12 : 4, dt);
  bvel.z = ds_damp(bvel.z, dir.z * sp, bgrounded ? 12 : 4, dt);
  /* up after the player: a hop, then the jetpack while they are still above */
  bool want_up = pos.y - bpos.y > 1.2f && flat < 14;
  if (bgrounded && (want_up || (flat > 2 && bvel.x * bvel.x + bvel.z * bvel.z < 1))) { bvel.y = JUMP_V; bgrounded = false; }
  bjetting = !bgrounded && want_up && bjet > 0.05f && bvel.y < JET_MAX_UP;
  if (bjetting) { bvel.y += JET_ACCEL * dt; bjet -= JET_DRAIN * dt; }
  else if (bgrounded) bjet = fminf(1, bjet + JET_CHARGE * dt);
  bvel.y -= GRAVITY * dt;
  world_move(&bpos, &bvel, HERO_R, HERO_H, dt, &bgrounded);
  if (bpos.y < -70) bpos = beside(pos, yaw, -1.6f);
  /* fight */
  bfire_cd -= dt;
  v3 eye = v3_add(bpos, v3_(0, HERO_H * 0.8f, 0));
  int target = st == ST_PLAY ? enemy_nearest(eye, 45, true) : -1;
  float face = flat > 1.2f ? atan2f(dir.x, dir.z) : yaw;
  if (target >= 0) {
    v3 tp = v3_add(enemy_pos(target), v3_(0, 0.9f, 0));
    v3 d = v3_norm(v3_sub(tp, eye));
    face = atan2f(d.x, d.z);
    toy_aim(pal, asinf(ds_clamp(d.y, -1, 1)), 1);
    if (bfire_cd <= 0) {
      bfire_cd = ds_rr(0.35f, 0.6f);
      v3 m = toy_muzzle(pal);
      int id; float t; bool weak;
      float wt = 60; v3 n;
      bool hw = world_raycast(m, d, 60, &wt, &n);
      v3 jit = v3_norm(v3_add(d, v3_(ds_rr(-0.03f, 0.03f), ds_rr(-0.03f, 0.03f), ds_rr(-0.03f, 0.03f))));
      if (enemy_raycast(m, jit, hw ? wt : 60, &id, &t, &weak)) {
        if (enemy_damage(id, 7, v3_add(m, v3_scale(jit, t)), weak, WP_BANDS)) kills++;
        fx_tracer(m, v3_add(m, v3_scale(jit, t)), 0xd8503a, 0.025f);
      } else fx_tracer(m, v3_add(m, v3_scale(jit, hw ? wt : 30)), 0xd8503a, 0.025f);
      sfx_at(SFX_BAND, m.x, m.y, m.z, 0.5f);
    }
  } else toy_aim(pal, 0, 0);
  byaw += ds_wrap_pi(face - byaw) * fminf(1, dt * 10);
  /* hurt */
  v3 from;
  float dmg = enemy_hurt(1, &from);
  if (dmg > 0 && st == ST_PLAY) {
    bhp -= dmg * 0.6f;
    if (bhp <= 0) { bhp = 0; bdown_t = 6; toy_play(pal, "Hit_Chest", 0.8f, false); voice_say(cutscene_voice_for(toy_name(who ^ 1)), "I'm fine. Give me a second."); }
  }
  /* animation */
  float hs = sqrtf(bvel.x * bvel.x + bvel.z * bvel.z);
  if (!bgrounded) toy_play(pal, "Jump_Loop", 1, true);
  else if (hs > 4.5f) toy_play(pal, "Sprint_Loop", hs / 8, true);
  else if (hs > 0.6f) toy_play(pal, "Jog_Fwd_Loop", fmaxf(0.6f, hs / 5), true);
  else toy_play(pal, "Idle_Loop", 1, true);
  toy_jet(pal, bjetting ? 1 : 0);
  if (bjetting) sfx_at(SFX_JET, bpos.x, bpos.y, bpos.z, 0.4f);
}

static void update_play(float dt, const intent_t *I) {
  level_time += dt;
#ifdef DS_TRACE
  if (input_key_down(WC_KEY_8)) hp = HP_MAX;
  /* test only: 7 onto the basement hatch gap, 6 into member storage's U */
  if (input_key_down(WC_KEY_7)) { pos = v3_(683.5f, 0.5f, 14.6f); vel = v3_(0, 0, 0); cam_yaw = DS_PI * 0.5f; }
  if (input_key_down(WC_KEY_6)) { pos = v3_(372, 0.2f, 60); vel = v3_(0, 0, 0); cam_yaw = DS_PI * 0.0f; }
#endif
  combat_t -= dt; hurt_iframes -= dt; swing_t -= dt;
  /* the view */
  if (I->view) { first_person = !first_person; DS_SAVE.first_person = first_person; sfx(SFX_UI, 0, 0.6f); }
  /* movement relative to the camera */
  v3 fwd = v3_(-sinf(cam_yaw), 0, -cosf(cam_yaw)), right = v3_(cosf(cam_yaw), 0, -sinf(cam_yaw));
  v3 wish = v3_add(v3_scale(fwd, I->my), v3_scale(right, I->mx));
  float wl = v3_len(wish);
  if (wl > 1) { wish = v3_scale(wish, 1 / wl); wl = 1; }
  float speed = (I->aim ? AIM_SPEED : RUN_SPEED) * weapon_glue_slow(pos);
  if (I->dodge && dodge_cd <= 0 && (grounded || jet > 0.15f)) {
    dodge_t = DODGE_T; dodge_cd = 0.6f;
    dodge_dir = wl > 0.1f ? v3_norm(wish) : v3_(sinf(yaw), 0, cosf(yaw));
    if (!grounded) { jet -= 0.15f; vel.y = fmaxf(vel.y, 2); }
    sfx(SFX_DODGE, 0, 1);
  }
  dodge_cd -= dt;
  bool aiming = I->aim || I->fire_held || swing_t > 0;
  if (dodge_t > 0) {
    dodge_t -= dt;
    vel.x = dodge_dir.x * DODGE_V; vel.z = dodge_dir.z * DODGE_V;
    yaw = atan2f(dodge_dir.x, dodge_dir.z);
  } else {
    float ctl = grounded ? 14 : jetting ? 5 : 3;
    vel.x = ds_damp(vel.x, wish.x * speed, ctl, dt);
    vel.z = ds_damp(vel.z, wish.z * speed, ctl, dt);
    float face = (aiming || first_person) ? atan2f(cam_fwd.x, cam_fwd.z) : (wl > 0.1f ? atan2f(wish.x, wish.z) : yaw);
    yaw += ds_wrap_pi(face - yaw) * fminf(1, dt * 16);
  }
  /* jump, then the jetpack while A is held in the air */
  if (grounded && I->jump) { vel.y = JUMP_V; grounded = false; sfx(SFX_JUMP, 0, 0.8f); jet_delay = 0.18f; }
  jet_delay -= dt;
  bool was_jet = jetting;
  jetting = !grounded && I->jump_held && jet > 0 && jet_delay <= 0 && dodge_t <= 0;
  if (jetting) {
    vel.y = fminf(JET_MAX_UP, vel.y + JET_ACCEL * dt);
    jet = fmaxf(0, jet - JET_DRAIN * dt);
    sfx(SFX_JET, 0, 0.7f);
    if (jet <= 0) { sfx(SFX_JET_EMPTY, 0, 1); alert("BATTERY LOW"); }
  } else if (grounded) {
    static float charge_wait;
    charge_wait = was_jet ? JET_DELAY : charge_wait - dt;
    if (charge_wait <= 0) jet = fminf(1, jet + JET_CHARGE * dt);
  }
  vel.y -= GRAVITY * dt;
  float vy0 = vel.y;
  bool was_grounded = grounded;
  world_move(&pos, &vel, HERO_R, HERO_H, dt, &grounded);
  if (grounded && !was_grounded && vy0 < -8) { sfx(SFX_LAND, 0, fminf(1, -vy0 / 25)); shake = fmaxf(shake, fminf(0.2f, -vy0 / 120)); }
  if (pos.y < -70) { hurt_iframes = 0;   /* below the basement */ hurt(20, pos); if (st == ST_PLAY) { pos = checkpoint; vel = v3_(0, 0, 0); } }
  /* scope: the coilgun aimed */
  bool want_scope = I->aim && weapon_current() == WP_COIL && dodge_t <= 0;
  if (want_scope != scoped) { scoped = want_scope; if (scoped) zoom = 1; }
  if (scoped && I->zoom) { int z = zoom + I->zoom; if (z >= 1 && z <= 3) { zoom = z; sfx(SFX_UI, 0, 0.4f); } }
  /* weapons */
  weapon_in_t wi = {0};
  wi.eye = cam_pos; wi.dir = cam_fwd;
  if (!first_person && !scoped) {
    /* start the aim ray at the hero, not behind them: what the reticle is on, seen from the shoulder */
    float t; v3 n; v3 far = v3_add(cam_pos, v3_scale(cam_fwd, 400));
    if (world_raycast(cam_pos, cam_fwd, 400, &t, &n)) far = v3_add(cam_pos, v3_scale(cam_fwd, t));
    v3 o = v3_add(cam_pos, v3_scale(cam_fwd, fmaxf(0, v3_dot(v3_sub(pos, cam_pos), cam_fwd))));
    wi.eye = o; wi.dir = v3_norm(v3_sub(far, o));
  }
  wi.muzzle = toy_muzzle(me);
  wi.body = v3_add(pos, v3_(0, HERO_H * 0.6f, 0));
  wi.yaw = yaw;
  wi.fire = I->fire; wi.fire_held = I->fire_held; wi.fire_released = I->fire_released;
  bool used = I->melee && mission_interact(pos);
  wi.aim = I->aim; wi.throw_lipo = I->lipo; wi.melee = I->melee && !used;
  wi.next = I->next; wi.prev = I->prev; wi.select = I->select;
  weapon_out_t wo = {0};
  weapon_update(dt, &wi, &wo);
#ifdef DS_TRACE
  if (wi.fire || wo.recoil > 0 || wo.threw || wo.swing || wo.hit) ds_log("[fire] w%d fire%d held%d ammo%d hit%d swing%d threw%d charge %.2f", weapon_current(), wi.fire, wi.fire_held, weapon_ammo(weapon_current()), wo.hit, wo.swing, wo.threw, wo.charge);
#endif
  if (wo.recoil > 0) shake = fmaxf(shake, wo.recoil * 0.3f);
  if (wo.hit) { H.hit_marker = 0.2f; H.hit_weak = wo.hit_weak; combat_t = 4; }
  if (wo.killed) kills++;
  if (wo.swing) { swing_t = 0.35f; toy_play(me, "Punch_Cross", 1.6f, false); }
  if (I->fire_held) combat_t = 4;
  H.charge = wo.charge;
  /* the army */
  v3 from;
  float dmg = enemy_hurt(0, &from);
  if (dmg > 0) hurt(dmg, from);
  if (st != ST_PLAY) return;
  /* the mission */
  mission_out_t mo; memset(&mo, 0, sizeof mo); mo.radio_beat = mo.pickup = mo.weapon_found = -1;
  mission_update(dt, pos, &mo);
  H.objective = mo.objective;
  H.prompt = mo.prompt;
  H.marker_on = false;
  if (mo.marker) {
    v3 rel = v3_sub(mo.marker_pos, cam_pos);
    v3 rgt = v3_norm(v3_cross(cam_fwd, v3_(0, 1, 0))), up = v3_cross(rgt, cam_fwd);
    float z = v3_dot(rel, cam_fwd), f = 1 / tanf(fov * DS_PI / 360);
    H.marker_dist = v3_len(rel);
    H.marker_behind = z < 0.1f;
    float zz = fmaxf(fabsf(z), 0.1f);
    H.marker_x = DS_W * 0.5f + v3_dot(rel, rgt) / zz * f * DS_H * 0.5f;
    H.marker_y = DS_H * 0.5f - v3_dot(rel, up) / zz * f * DS_H * 0.5f;
    H.marker_on = true;
  }
  if (mo.banner) banner(mo.banner);
  if (mo.checkpoint) { checkpoint = mission_checkpoint(); alert("CHECKPOINT"); sfx(SFX_UI_OK, 0, 0.8f); }
  if (mo.pickup >= 0) {
    sfx(SFX_PICKUP, 0, 1);
    if (mo.pickup == PK_HEALTH) { hp = fminf(HP_MAX, hp + 35); bhp = fminf(HP_MAX, bhp + 35); banner("ELECTRICAL TAPE. GOOD AS NEW."); }
    else if (mo.pickup == PK_BATTERY) { jet = 1; banner("FRESH BATTERY"); }
    else weapon_give(mo.pickup);
  }
  if (mo.weapon_found >= 0) { sfx(SFX_WEAPON, 0, 1); weapon_unlock(mo.weapon_found); banner(weapon_name(mo.weapon_found)); }
  if (mo.radio_beat >= 0) {
    const char *spk, *line = mission_radio(level, mo.radio_beat, &spk);
    if (line) { H.radio_speaker = spk; H.radio_text = line; H.radio_t = 4 + (float)strlen(line) * 0.045f; voice_say(cutscene_voice_for(spk), line); }
  }
  { const char *spk, *line = enemy_take_shout(&spk);
    if (line) { H.radio_speaker = spk; H.radio_text = line; H.radio_t = 3; voice_say(cutscene_voice_for(spk), line); } }
  /* bosses: the reveal when the arena is entered, the ending once the General is down */
  if (mo.boss_arena && !boss_cut_done && mission_boss_cut(level) >= 0) {
    boss_cut_done = true;
    play_cut((cut_id)mission_boss_cut(level), AFTER_BOSS);
    return;
  }
  if (enemy_boss_active()) music_mood(MOOD_BOSS);
  else music_mood(combat_t > 0 ? MOOD_FIGHT : MOOD_EXPLORE);
  if (mission_is_final(level) && enemy_boss_dead()) {
    if (!boss_dead_on) { boss_dead_on = true; boss_dead_t = 0; music_mood(MOOD_CLEAR); }
    boss_dead_t += dt;
  }
  bool done = mo.done && (!mission_is_final(level) || (boss_dead_on && boss_dead_t > 3));
  if (done) {
    st = ST_CLEAR; st_t = 0; scoped = false;
    music_mood(MOOD_CLEAR);
    int ms = (int)(level_time * 1000);
    H.new_best = false;
    if (!DS_SAVE.best_ms[level] || ms < (int)DS_SAVE.best_ms[level]) { DS_SAVE.best_ms[level] = ms; H.new_best = true; }
    if ((uint32_t)score > DS_SAVE.best_score[level]) { DS_SAVE.best_score[level] = score; H.new_best = true; }
    if (DS_SAVE.level_reached < level + 1 && level + 1 < MISSION_N) DS_SAVE.level_reached = level + 1;
    toy_play(me, "Celebration", 1, true);
  }
  /* animation: the clip from the legs, the aim layer from the weapon */
  float hs = sqrtf(vel.x * vel.x + vel.z * vel.z);
  if (st != ST_PLAY) {}
  else if (swing_t > 0) {}
  else if (dodge_t > 0) {
    float side = vel.x * cosf(yaw) - vel.z * sinf(yaw);
    toy_play(me, side < 0 ? "Dodge_Left" : "Dodge_Right", 1.6f, false);
  }
  else if (!grounded) toy_play(me, "Jump_Loop", 1, true);
  else if (hs > 0.6f) {
    if (aiming) {
      /* strafing while aiming: the jog that matches the direction relative to the facing */
      float lf = vel.x * sinf(yaw) + vel.z * cosf(yaw), ls = vel.x * cosf(yaw) - vel.z * sinf(yaw);
      const char *c = fabsf(lf) >= fabsf(ls) ? (lf >= 0 ? "Jog_Fwd_Loop" : "Jog_Bwd_Loop") : (ls >= 0 ? "Jog_Left_Loop" : "Jog_Right_Loop");
      toy_play(me, c, fmaxf(0.6f, hs / 4.5f), true);
    } else toy_play(me, hs > 5.5f ? "Sprint_Loop" : "Jog_Fwd_Loop", fmaxf(0.6f, hs / (hs > 5.5f ? 8.f : 5.f)), true);
  }
  else toy_play(me, "Idle_Loop", 1, true);
  toy_aim(me, asinf(ds_clamp(cam_fwd.y, -1, 1)), (aiming && swing_t <= 0) ? 1 : 0.0f);
  toy_set_weapon(me, weapon_current());
  toy_jet(me, jetting ? 1 : 0);
  update_buddy(dt);
}

static void toggle_setting(int which) {
  if (which == 0) { DS_SAVE.music ^= 1; audio_set_music(DS_SAVE.music); }
  else if (which == 1) DS_SAVE.shake ^= 1;
  else if (which == 2) DS_SAVE.invert_y ^= 1;
  else { first_person = !first_person; DS_SAVE.first_person = first_person; }
  sfx(SFX_UI_OK, 0, 0.8f);
}

/* ── the title's attract reel ──
 * Short scenes of the two of them around the lab while the menu is up: each
 * is a pair of paths over one surface (a landmark's top or the floor), a
 * movement style, and a camera that tracks the pair. A few army men patrol
 * some of them (enemy_update with no live target: they march, they never shoot). */
typedef enum { MV_RUN, MV_JET, MV_IDLE, MV_DANCE, MV_FIRE } reel_move;
typedef struct {
  const char *place;
  v3 a0, a1, b0, b1;           /* Moheeb from a0 to a1, Luis from b0 to b1 (y is the surface) */
  reel_move ma, mb;
  v3 cam_off; float cam_swing; /* camera offset from the pair's midpoint, and how far it swings over the scene */
  int army;                    /* riflemen patrolling near the pair */
} reel_scene;
#define REEL_N 6
#define REEL_T 7.5f
static reel_scene reel[REEL_N];
static float reel_fire;
static float reel_fire_t[HERO_COUNT];

static v3 box_top(const lab_box_t *b, float fx, float fz) {
  return v3_(ds_lerp(b->min[0], b->max[0], fx), b->max[1] + 0.02f, ds_lerp(b->min[2], b->max[2], fz));
}
static v3 on_floor(float x, float z) { float y = world_floor_y(v3_(x, 2, z)); return v3_(x, y > -1e8f ? y : 0, z); }

static void reel_build(void) {
  const lab_box_t *lid = &LM_LASER_LID[0], *rack = &LM_PRINTER_RACK[0], *desk = &LM_COWORK_TABLE[1];
  const lab_box_t *sofa = &LM_SOFA_SEAT[1], *bench = &LM_BACK_BENCH[0], *lathe = &LM_LATHE_BED[0];
  /* the laser lid: a race along it, then off the end on the jetpacks */
  reel[0] = (reel_scene){"THE LASER CORNER", box_top(lid, 0.9f, 0.35f), box_top(lid, 0.05f, 0.35f), box_top(lid, 0.95f, 0.7f), box_top(lid, 0.1f, 0.7f),
                         MV_RUN, MV_RUN, {0, 2.2f, 7}, 0.5f, 0};
  /* the print farm: up the front of the printer rack */
  v3 rf = on_floor((rack->min[0] + rack->max[0]) * 0.5f, rack->max[2] + 8);
  reel[1] = (reel_scene){"THE PRINT FARM", v3_add(rf, v3_(-2, 0, 0)), v3_(rf.x - 2, rack->max[1] + 2, rack->max[2] - 2), v3_add(rf, v3_(2.5f, 0, 1)), v3_(rf.x + 3, rack->max[1] + 2, rack->max[2] - 2),
                         MV_JET, MV_JET, {7, 3, 14}, 0.4f, 3};
  /* coworking: a sprint under the tables, between the chair legs */
  v3 d0 = on_floor(desk->min[0] + 4, (desk->min[2] + desk->max[2]) * 0.5f), d1 = on_floor(desk->max[0] - 4, d0.z);
  reel[2] = (reel_scene){"COWORKING, UNDER THE TABLES", d0, d1, v3_add(d0, v3_(-3, 0, 1.5f)), v3_add(d1, v3_(-3, 0, 1.5f)),
                         MV_RUN, MV_RUN, {-3, 1.4f, -5}, 0.3f, 0};
  /* the lounge: the couch as a stage */
  v3 c0 = box_top(sofa, 0.5f, 0.35f), c1 = box_top(sofa, 0.5f, 0.7f);
  reel[3] = (reel_scene){"THE LOUNGE", c0, c0, c1, c1, MV_DANCE, MV_IDLE, {6, 4.5f, 6}, 1.0f, 0};
  /* the woodshop: holding the bench against a patrol */
  v3 w0 = box_top(bench, 0.4f, 0.3f), w1 = box_top(bench, 0.4f, 0.5f);
  reel[4] = (reel_scene){"THE WOODSHOP", w0, w1, v3_add(w0, v3_(0.6f, 0, -1.6f)), v3_add(w1, v3_(0.6f, 0, -1.6f)), MV_FIRE, MV_FIRE, {-7, 3, -5}, 0.5f, 4};
  /* the machine shop: a walk past the lathe, the breaker panel ahead */
  v3 l0 = on_floor(lathe->max[0] - 2, lathe->min[2] - 8), l1 = on_floor(lathe->min[0] + 6, lathe->min[2] - 8);
  reel[5] = (reel_scene){"THE MACHINE SHOP", l0, l1, v3_add(l0, v3_(1.5f, 0, -2)), v3_add(l1, v3_(1.5f, 0, -2)), MV_RUN, MV_RUN, {2, 2, -9}, 0.4f, 3};
}

static void reel_move_hero(toy_t *t, v3 p0, v3 p1, reel_move m, float u, v3 *p, float *face, float *jet_out, int who_i) {
  v3 d = v3_sub(p1, p0);
  *face = (fabsf(d.x) + fabsf(d.z) > 0.1f) ? atan2f(d.x, d.z) : *face;
  *jet_out = 0;
  switch (m) {
    case MV_RUN: {
      *p = v3_lerp(p0, p1, u);
      float f = world_floor_y(v3_(p->x, p0.y + 1.2f, p->z));
      if (f > p0.y - 1.5f) p->y = f;
      toy_play(t, v3_len(d) > 30 ? "Sprint_Loop" : "Jog_Fwd_Loop", 1.1f, true);
      toy_aim(t, 0, 0);
      break; }
    case MV_JET: {
      /* a hop, then thrust up the face, then a landing on top */
      float k = ds_clamp((u - 0.12f) / 0.7f, 0, 1);
      *p = v3_(ds_lerp(p0.x, p1.x, k * k), p0.y + (p1.y - p0.y) * (k * k * (3 - 2 * k)), ds_lerp(p0.z, p1.z, k * k));
      bool air = u > 0.08f && u < 0.86f;
      if (u < 0.12f) { *face = atan2f(-d.x, -d.z) + DS_PI; }
      *jet_out = air ? 1 : 0;
      toy_play(t, air ? "Jump_Loop" : u >= 0.86f ? "Celebration" : "Idle_Loop", 1, true);
      toy_aim(t, 0, 0);
      break; }
    case MV_DANCE:
      *p = p0; *face = (float)ds_now() * 0.4f;
      toy_play(t, "Celebration", 1, true);
      toy_aim(t, 0, 0);
      break;
    case MV_IDLE:
      *p = p0; *face = atan2f(p1.x - p0.x + 1, p1.z - p0.z) + DS_PI * 0.5f;
      toy_play(t, u < 0.5f ? "Idle_Loop" : "Surprise", 1, u < 0.5f);
      toy_aim(t, 0, 0);
      break;
    case MV_FIRE: {
      /* strafe along the bench, shooting down at the patrol */
      *p = v3_lerp(p0, p1, 0.5f - 0.5f * cosf(u * DS_PI * 2));
      int e = enemy_nearest(v3_add(*p, v3_(0, 1.4f, 0)), 60, false);
      float pitch = -0.35f;
      if (e >= 0) {
        v3 to = v3_sub(v3_add(enemy_pos(e), v3_(0, 0.8f, 0)), v3_add(*p, v3_(0, 1.4f, 0)));
        *face = atan2f(to.x, to.z);
        pitch = atan2f(to.y, sqrtf(to.x * to.x + to.z * to.z));
      } else *face = atan2f(-1, 0);
      float side = sinf(u * DS_PI * 2);
      toy_play(t, fabsf(side) < 0.2f ? "Idle_Loop" : side > 0 ? "Jog_Left_Loop" : "Jog_Right_Loop", 0.9f, true);
      toy_aim(t, pitch, 1);
      reel_fire_t[who_i] -= 1.f / 60;
      if (reel_fire_t[who_i] <= 0 && e >= 0) {
        reel_fire_t[who_i] = ds_rr(0.25f, 0.5f);
        v3 mz = toy_muzzle(t), tgt = v3_add(enemy_pos(e), v3_(ds_rr(-0.4f, 0.4f), ds_rr(0.4f, 1.2f), ds_rr(-0.4f, 0.4f)));
        fx_tracer(mz, tgt, 0xd8503a, 0.025f);
        fx_sparks(tgt, v3_(0, 1, 0), C_ARMY, 3);
      }
      break; }
  }
  (void)reel_fire;
}

static void title_reel(float t) {
  if (!reel[0].place) reel_build();
  int sc = (int)(t / REEL_T) % REEL_N;
  float u = fmodf(t, REEL_T) / REEL_T;
  const reel_scene *R = &reel[sc];
  if (sc != reel_cur) {
    reel_cur = sc;
    enemy_clear(); fx_clear();
    v3 mid = v3_scale(v3_add(R->a1, R->b1), 0.5f);
    for (int i = 0; i < R->army; i++) {
      /* a patrol on the floor in front of the scene (on its surface for the rack) */
      v3 sp = sc == 4 ? on_floor(mid.x - 12 - i * 2.5f, mid.z + ds_rr(-6, 6)) : on_floor(mid.x + ds_rr(-8, 8), mid.z + ds_rr(-6, 6) + (sc == 1 ? 10 : 0));
      enemy_spawn(EN_RIFLE, sp, ds_rr(-DS_PI, DS_PI));
    }
  }
  float ja, jb;
  reel_move_hero(heroes[HERO_MOHEEB], R->a0, R->a1, R->ma, u, &pos, &yaw, &ja, 0);
  reel_move_hero(heroes[HERO_LUIS], R->b0, R->b1, R->mb, ds_clamp(u * 1.04f - 0.04f, 0, 1), &bpos, &byaw, &jb, 1);
  /* game_frame places me at pos and the buddy at bpos: give each hero its own path whoever is picked */
  if (who == HERO_LUIS) { v3 tp = pos; pos = bpos; bpos = tp; float ty = yaw; yaw = byaw; byaw = ty; float tj = ja; ja = jb; jb = tj; }
  toy_jet(me, ja); toy_jet(pal, jb);
  toy_set_weapon(me, WP_BANDS); toy_set_weapon(pal, WP_BANDS);
  /* the army marches, nobody to shoot at */
  v3 mid = v3_scale(v3_add(pos, bpos), 0.5f);
  enemy_target_t none = {v3_add(mid, v3_(0, 0, 0)), HERO_R, HERO_H, false};
  enemy_update(1.f / 60, &none, &none);
  /* the camera: off the pair's midpoint, swinging slowly across the scene */
  float sw = (u - 0.5f) * R->cam_swing;
  v3 off = v3_(R->cam_off.x * cosf(sw) - R->cam_off.z * sinf(sw), R->cam_off.y, R->cam_off.x * sinf(sw) + R->cam_off.z * cosf(sw));
  v3 look = v3_add(mid, v3_(0, 1.1f, 0));
  /* stay out of the furniture: the offset, or its mirror across the pair if that has more room */
  v3 want = v3_add(mid, off);
  float room = 0;
  for (int k = 0; k < 2; k++) {
    v3 o2 = k ? v3_(-off.x, off.y, -off.z) : off;
    v3 w2 = v3_add(mid, o2), dir = v3_sub(w2, look);
    float dl = v3_len(dir), tt; v3 n;
    float got = world_raycast(look, v3_scale(dir, 1 / dl), dl, &tt, &n) ? fmaxf(1.2f, tt - 0.3f) : dl;
    if (got > room + 0.5f) { room = got; want = v3_add(look, v3_scale(dir, got / dl)); }
  }
  cam_pos = u < 0.02f ? want : v3_lerp(cam_pos, want, 0.12f);
#ifdef SP_AIMTEST   /* a fixed side view of the aim pose */
  { float face; v3 at = mission_start(0, &face); pos = at; bpos = beside(at, face, -1.4f); yaw = byaw = face;
    cam_pos = v3_add(at, v3_(-0.5f, 1.4f, 4.0f)); look = v3_add(at, v3_(0, 1.1f, 0));
    toy_aim(me, 0.25f, 1); toy_aim(pal, -0.2f, 1); }
#endif
#ifdef SP_VIEW   /* test only: fixed views of the model's corrections, a few seconds each */
  { static const float V[][6] = {{690, 14, 4, 683, -30, 18}, {700, 9, 50, 683, 0, 26}, {735, -20, 95, 683, -36, 26}, {660, -38, 20, 700, -46, 60}, {350, 40, 95, 375, 0, 30}, {395, 12, 70, 382, 8, 30}};
    int k = (int)(t / 2.5f) % 6; cam_pos = v3_(V[k][0], V[k][1], V[k][2]); look = v3_(V[k][3], V[k][4], V[k][5]);
    toy_visible(me, false); toy_visible(pal, false); }
#endif
  cam->fov = 52; t3_camera_update_projection_matrix(cam);
  t3_object_set_position(cam, cam_pos.x, cam_pos.y, cam_pos.z);
  t3_object_look_at(cam, look.x, look.y, look.z);
  audio_listener(cam_pos.x, cam_pos.y, cam_pos.z, 0);
#ifdef DS_TRACE
  { static int fr; if (++fr % 60 == 0) ds_log("[reel] sc%d u%.2f cam %.1f %.1f %.1f look %.1f %.1f %.1f me %.1f %.1f %.1f pal %.1f %.1f %.1f", sc, u, cam_pos.x, cam_pos.y, cam_pos.z, look.x, look.y, look.z, pos.x, pos.y, pos.z, bpos.x, bpos.y, bpos.z); }
#endif
  H.title_place = R->place;
  H.title_scene = sc;
}

/* title rows (hud_screens.c): 0 the hero (left / right picks), then the
 * unlocked levels, then music, shake, invert and the view; no wrapping */
static void update_title(const intent_t *I) {
  int unlocked = DS_SAVE.level_reached + 1;
  if (unlocked > MISSION_N) unlocked = MISSION_N;
  int rows = 1 + unlocked + 4;
  if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 0 && m < rows) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
  if (H.menu_sel == 0 && I->nav_x) { pick_hero((hero_id)(I->nav_x > 0 ? HERO_LUIS : HERO_MOHEEB)); sfx(SFX_UI, 0, 0.6f); }
  if (I->ok && st_t > 0.4f) {
    if (H.menu_sel == 0) { H.menu_sel = 1; sfx(SFX_UI_OK, 0, 0.8f); }
    else if (H.menu_sel <= unlocked) { sfx(SFX_UI_OK, 0, 1); start_level(H.menu_sel - 1); return; }
    else toggle_setting(H.menu_sel - 1 - unlocked);
  }
  H.unlocked = unlocked;
  title_reel((float)ds_now() - reel_t0);
}

/* pause rows 0..3 (resume, restart, settings, quit); settings opens rows 4..7 */
static void update_pause(const intent_t *I) {
  if (H.menu_sel >= 4) {
    if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 4 && m < 8) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
    if (I->ok) toggle_setting(H.menu_sel - 4);
    if (I->back) { H.menu_sel = 2; sfx(SFX_UI, 0, 0.6f); }
    return;
  }
  if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 0 && m < 4) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
  if (I->ok && H.menu_sel == 2) { H.menu_sel = 4; sfx(SFX_UI_OK, 0, 0.8f); return; }
  if (I->back || I->pause || (I->ok && H.menu_sel == 0)) { st = ST_PLAY; sfx(SFX_UI, 0, 0.6f); return; }
  if (I->ok && H.menu_sel == 1) { start_level(level); return; }
  if (I->ok && H.menu_sel == 3) { to_title(); return; }
}

#ifdef DS_TRACE
static void trace_count(t3_object *o, void *ctx) { int *c = ctx; c[0]++; if (o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH) c[1]++; }
#endif

void game_frame(float dt) {
  const intent_t *I = input_get();
  st_t += dt;
  if (radio_prep < mission_radio_count(level) && st != ST_CUT && st != ST_TITLE) {
    const char *spk, *line = mission_radio(level, radio_prep++, &spk);
    if (line) voice_prepare(cutscene_voice_for(spk), line);
  }
#ifdef DS_TRACE
  { static int fr; if (++fr % 60 == 0) {
    ds_log("[trace] f%d st%d lvl%d pos %.1f %.1f %.1f vy %.1f hp %.0f jet %.2f g%d alive %d area %s", fr, st, level, pos.x, pos.y, pos.z, vel.y, hp, jet, grounded, enemy_alive(), world_area_name(world_area_at(pos)));
    const t3_render_info *ri = t3_renderer_info(DS_R);
    int cnt[2] = {0, 0}; t3_object_traverse(&scene->base, trace_count, cnt);
    ds_log("[trace] draw calls %u tris %u objects %d meshes %d", ri->calls, ri->triangles, cnt[0], cnt[1]); } }
#endif
  if (st == ST_PAUSE) update_pause(I);
  else {
    switch (st) {
      case ST_TITLE: update_title(I); break;
      case ST_PLAY:
        if (I->pause) { st = ST_PAUSE; H.menu_sel = 0; scoped = false; sfx(SFX_UI, 0, 0.6f); break; }
        update_play(dt, I);
        break;
      case ST_DEAD:
        vel.x = vel.z = 0; vel.y -= GRAVITY * dt;
        world_move(&pos, &vel, HERO_R, HERO_H, dt, &grounded);
        if (st_t > 1.2f && (I->ok || I->jump)) {
          st = ST_PLAY; st_t = 0;
          float face = yaw;
          reset_player(checkpoint, face);
          music_mood(MOOD_EXPLORE);
        }
        break;
      case ST_CLEAR:
        if (st_t > 1.5f && (I->ok || I->jump)) {
          if (level + 1 < MISSION_N) start_level(level + 1);
          else play_cut(CUT_ENDING, AFTER_END);
        }
        break;
      case ST_END:
        if (st_t > 3 && (I->ok || I->jump)) to_title();
        break;
      case ST_CUT:
        if (!cutscene_update(dt, I, &CV)) cut_finish();
        break;
      default: break;
    }
    if (st == ST_PLAY || st == ST_DEAD || st == ST_CLEAR) {
      world_update(dt, pos);
      enemy_target_t P = {pos, HERO_R, HERO_H, st == ST_PLAY && hp > 0};
      enemy_target_t B = {bpos, HERO_R, HERO_H, bdown_t <= 0};
      enemy_update(dt, &P, &B);
      score += enemy_take_score();
    }
  }
  if (st != ST_PAUSE) {
    toy_place(me, pos, yaw, 1);
    toy_place(pal, bpos, byaw, 1);
    toy_visible(me, !(first_person || scoped) || st != ST_PLAY);
    /* the buddy steps out of the picture rather than filling it when the camera is on top of him */
    toy_visible(pal, st != ST_PLAY || v3_len(v3_sub(v3_add(bpos, v3_(0, HERO_H * 0.6f, 0)), cam_pos)) > 1.4f);
    if (st == ST_CUT) {
      /* the cutscene drives the camera and both heroes */
      for (int i = 0; i < HERO_COUNT; i++) {
        const cine_hero_t *ch = &CV.hero[i];
        toy_visible(heroes[i], ch->visible);
        toy_place(heroes[i], ch->pos, ch->yaw, ch->size > 0 ? ch->size : 1);
        if (ch->clip) toy_play(heroes[i], ch->clip, ch->clip_speed > 0 ? ch->clip_speed : 1, ch->clip_loop);
        toy_aim(heroes[i], 0, ch->aim);
        toy_jetpack_visible(heroes[i], ch->jetpack);
        toy_jet(heroes[i], ch->jet);
        toy_set_weapon(heroes[i], ch->armed ? WP_BANDS : -1);
      }
      cam->fov = CV.fov > 1 ? CV.fov : 50;
      t3_camera_update_projection_matrix(cam);
      t3_object_set_position(cam, CV.cam_pos.x, CV.cam_pos.y, CV.cam_pos.z);
      t3_object_look_at(cam, CV.cam_target.x, CV.cam_target.y, CV.cam_target.z);
      cam_pos = CV.cam_pos;
      hemi->intensity = 1.5f * (0.25f + 0.75f * (CV.lights > 0 ? CV.lights : 1));
    } else {
      hemi->intensity = 1.5f;
      if (st != ST_TITLE) update_camera(dt, I);
    }
    for (int i = 0; i < HERO_COUNT; i++) toy_update(heroes[i], dt);
    fx_update(dt, cam);
  }

  /* the HUD */
  H.t = st_t;
  H.level = level;
  H.level_name = mission_name(level);
  H.level_blurb = mission_blurb(level);
  H.hero = who; H.hero_name = toy_name(who); H.buddy_name = toy_name(who ^ 1);
  H.mode = st == ST_TITLE ? HUD_TITLE : st == ST_PAUSE ? HUD_PAUSE : st == ST_DEAD ? HUD_DEAD : st == ST_CLEAR ? HUD_CLEAR
         : st == ST_END ? HUD_END : st == ST_CUT ? HUD_CINE : (st_t < 3.5f ? HUD_INTRO : HUD_PLAY);
  H.cine = st == ST_CUT ? CV.letterbox : 0;
  H.cap_speaker = st == ST_CUT ? CV.speaker : NULL; H.cap_text = st == ST_CUT ? CV.text : NULL; H.cap_chars = CV.chars;
  H.skip = st == ST_CUT ? CV.skip : 0;
  H.flash = st == ST_CUT ? CV.flash : 0; H.fade = st == ST_CUT ? CV.fade : 0;
  if (st != ST_PAUSE) H.radio_t = fmaxf(0, H.radio_t - dt);
  if (H.radio_t <= 0) H.radio_text = NULL;
  H.health = hp / HP_MAX; H.buddy_health = bdown_t > 0 ? 0 : bhp / HP_MAX;
  H.jet = jet; H.jetting = jetting;
  H.weapon = weapon_current(); H.weapon_name = weapon_name(H.weapon); H.ammo = weapon_ammo(H.weapon);
  for (int w = 0; w < WP_COUNT; w++) H.owned[w] = weapon_unlocked(w);
  H.lipos = weapon_lipos();
  H.score = score; H.time_s = level_time;
  H.hit_marker = fmaxf(0, H.hit_marker - dt);
  H.hurt = fmaxf(0, H.hurt - dt);
  H.used_pad = I->used_pad;
  H.first_person = first_person;
  H.banner_t = fmaxf(0, H.banner_t - dt);
  if (H.banner_t <= 0) H.banner = NULL;
  H.scoped = scoped; H.zoom = zoom;
  H.on_enemy = H.on_weak = false; H.weak_mark = false; H.range = 0;
  if (scoped) {
    float t, et; v3 n; int id; bool weak;
    bool hw = world_raycast(cam_pos, cam_fwd, 800, &t, &n);
    H.range = hw ? t : 0;
    if (enemy_raycast(cam_pos, cam_fwd, hw ? t : 800, &id, &et, &weak)) { H.on_enemy = true; H.on_weak = weak; H.range = et; }
    v3 at;
    if (enemy_weak_point_near(cam_pos, cam_fwd, cosf(ZOOM_FOV[zoom] * DS_PI / 180 * 0.5f), &at)) {
      v3 rel = v3_sub(at, cam_pos);
      v3 rgt = v3_norm(v3_cross(cam_fwd, v3_(0, 1, 0))), up = v3_cross(rgt, cam_fwd);
      float z = v3_dot(rel, cam_fwd);
      if (z > 0.1f) {
        float f = 1 / tanf(fov * DS_PI / 360);
        H.weak_x = DS_W * 0.5f + v3_dot(rel, rgt) / z * f * DS_H * 0.5f;
        H.weak_y = DS_H * 0.5f - v3_dot(rel, up) / z * f * DS_H * 0.5f;
        H.weak_mark = true;
      }
    }
  }
  H.boss = enemy_boss_active(); H.boss_health = enemy_boss_health(); H.boss_phase = enemy_boss_phase();
  H.boss_name = enemy_boss_name();
  H.music_on = DS_SAVE.music; H.shake_on = DS_SAVE.shake; H.invert_y = DS_SAVE.invert_y;
  H.kills = kills; H.kills_total = kills + enemy_alive(); H.clear_time_s = level_time;
  H.best_time_ms = DS_SAVE.best_ms[level]; H.best_score = DS_SAVE.best_score[level];
}

void game_draw(void) {
  render_scene(scene, cam);
#ifndef SP_NOHUD   /* test only: clean frames for the web page and the station image */
  hud_draw(&H);
#endif
}
