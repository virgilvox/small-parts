/* game.c: the state machine, the players and the buddy, the cameras, and the
 * glue between the modules. world.c owns the lab, mission.c the levels,
 * enemy.c the army, weapon.c the players' weapons, hero.c the two heroes'
 * bodies, fx.c the particles, hud.c everything 2D, audio.c the sound.
 *
 * Two heroes are always in the lab: PL[0] is player 1, PL[1] the other hero.
 * With one player PL[1] is the buddy (update_buddy); a second pad can drop in
 * and drive PL[1] with the same rules as player 1 (two-player mode, coop),
 * and the screen splits in two. */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gen/lab_areas.h"

typedef enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_DEAD, ST_CLEAR, ST_END, ST_CUT } state_t;
typedef enum { AFTER_PLAY, AFTER_BOSS, AFTER_END } after_t;

static t3_scene *scene;
static t3_camera *cam;          /* player 1's, and the title's and the cutscenes' */
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

/* ── the players ── */
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
#define DOWN_T 6.f            /* the buddy, or a player in two-player mode, gets up after this */

typedef struct {
  toy_t *toy;
  t3_camera *cam;
  v3 pos, vel;
  float yaw;
  bool grounded, jetting, aiming;
  float hp, jet, jet_delay, charge_wait, dodge_t, dodge_cd, hurt_iframes, swing_t;
  v3 dodge_dir;
  float down_t;                /* on the floor: seconds until they get up */
  /* the camera */
  float cam_yaw, cam_pitch;
  v3 cam_pos, cam_fwd;
  bool first_person, scoped;
  int zoom;
  float fov, shake;
  /* the buddy (PL[1] with one player) */
  float fire_cd;
  float apart_t;               /* seconds the buddy has been too far, out of sight or stuck below the player */
  hud_player_t hud;            /* what the HUD shows of this player */
} player_t;

static hero_id who = HERO_MOHEEB;   /* player 1's hero */
static toy_t *heroes[HERO_COUNT];
static player_t PL[2];
static bool coop;                   /* player 2 is driving PL[1] */
static bool coop_want;              /* the title's PLAYERS row: start levels with two */
static bool split;                  /* this frame draws two views */
static float fx_dt;                 /* split: the particles age after both views are drawn */
static float combat_t;
static int kills, score;
static float level_time;
static v3 checkpoint;
static char join_buf[40];

static const float ZOOM_FOV[4] = {70, 32, 16, 8};
#define CAM_DIST 3.4f

static player_t *other(const player_t *p) { return p == &PL[0] ? &PL[1] : &PL[0]; }

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
  PL[0].toy = heroes[who]; PL[1].toy = heroes[who ^ 1];
  DS_SAVE.hero = (uint8_t)who;
}

/* a spot beside a hero facing y: side units to the right (negative: left), a little ahead,
 * so the buddy stands in view rather than between the shoulder camera and the player */
static v3 beside(v3 p, float y, float side) {
  return v3_add(p, v3_(cosf(y) * side + sinf(y) * 0.6f, 0, -sinf(y) * side + cosf(y) * 0.6f));
}

static void reset_one(player_t *p, v3 at, float face) {
  p->pos = at; p->vel = v3_(0, 0, 0); p->yaw = face;
  p->hp = HP_MAX; p->jet = 1; p->jet_delay = 0; p->dodge_t = p->dodge_cd = 0; p->hurt_iframes = 1.5f;
  p->grounded = false; p->jetting = false;
  p->down_t = 0; p->apart_t = 0;
  p->cam_yaw = face + DS_PI; p->cam_pitch = -0.15f;
  p->cam_fwd = v3_(-sinf(p->cam_yaw), 0, -cosf(p->cam_yaw));
}

/* player 1 at `at`; the other hero (the buddy, or player 2) at their left shoulder */
static void reset_player(v3 at, float face) {
  reset_one(&PL[0], at, face);
  bool g = PL[1].grounded, j = PL[1].jetting;   /* the buddy's legs carry over, as they always have */
  reset_one(&PL[1], beside(at, face, -1.4f), face);
  PL[1].grounded = g; PL[1].jetting = j;
}

static void set_scoped_off(void) { PL[0].scoped = PL[1].scoped = false; }

/* run a cutscene once per session, then carry on as `after` says */
static void play_cut(cut_id id, after_t after) {
  if (cut_seen[id]) {
    if (after == AFTER_BOSS) enemy_boss_wake();
    if (after == AFTER_END) { st = ST_END; st_t = 0; music_mood(MOOD_TITLE); }
    return;
  }
  cut_seen[id] = true;
  cutscene_hint(PL[0].pos, PL[0].yaw, PL[1].pos);
  cutscene_start(id, who);
  if (!cutscene_active()) {
    cut_seen[id] = false;
    if (after == AFTER_BOSS) enemy_boss_wake();
    if (after == AFTER_END) { st = ST_END; st_t = 0; }
    return;
  }
  cut_after = after;
  st = ST_CUT; st_t = 0; set_scoped_off();
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

/* ── two players ── */
/* player 2 takes the other hero from the buddy, where it stands */
static void coop_join(void) {
  player_t *p = &PL[1];
  coop = true; input_set_coop(true);
  p->first_person = false; p->scoped = false; p->zoom = 1; p->fov = ZOOM_FOV[0]; p->shake = 0;
  p->cam_yaw = p->yaw + DS_PI; p->cam_pitch = -0.15f;
  p->cam_fwd = v3_(-sinf(p->cam_yaw), 0, -cosf(p->cam_yaw));
  p->cam_pos = v3_add(p->pos, v3_add(v3_(0, HERO_H * 0.85f, 0), v3_scale(p->cam_fwd, -CAM_DIST)));
  p->dodge_t = p->dodge_cd = p->swing_t = p->jet_delay = p->charge_wait = 0; p->hurt_iframes = 1;
  memset(&p->hud, 0, sizeof p->hud);
  snprintf(join_buf, sizeof join_buf, "PLAYER 2 IS %s", toy_name(who ^ 1));
  banner(join_buf);
  sfx(SFX_UI_OK, 0, 1);
}

/* player 2 leaves (the pause menu, or their pad went away): the buddy takes the hero back */
static void coop_leave(void) {
  if (!coop) return;
  player_t *p = &PL[1];
  coop = false; input_set_coop(false);
  p->scoped = false; p->apart_t = 0; p->fire_cd = 0; p->jetting = false;
  weapon_set_player(1); weapon_cancel(); weapon_set_player(0);
  toy_set_weapon(p->toy, WP_BANDS);
  /* player 1 on the floor has no DOWN card with one player: up now */
  if (PL[0].down_t > 0) { PL[0].down_t = 0; PL[0].hp = HP_MAX * 0.6f; PL[0].hurt_iframes = 1.5f; toy_play(PL[0].toy, "KipUp", 1, false); }
  if (st == ST_PAUSE) H.menu_sel = 0;   /* the pause menu loses its LEAVE CO-OP row */
  banner("PLAYER 2 OUT");
  sfx(SFX_UI, 0, 0.8f);
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
  for (int i = 0; i < 2; i++) { PL[i].scoped = false; PL[i].zoom = 1; }
  enemy_take_score();
  st = ST_PLAY; st_t = 0;
  H.radio_t = 0;
  boss_cut_done = false; boss_dead_on = false; boss_dead_t = 0;
  music_mood(MOOD_EXPLORE);
  if (coop_want && input_pad2() && !coop) coop_join();
  int intro = mission_intro_cut(level);
  if (intro >= 0) play_cut((cut_id)intro, AFTER_PLAY);
}

static void to_title(void) {
  save_write();
  reel_t0 = (float)ds_now(); reel_cur = -1;
  if (coop) { coop = false; input_set_coop(false); coop_want = true; }
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
  PL[0].cam = cam;
  PL[1].cam = t3_perspective_camera_new(70, (float)(DS_LOW_W / 2) / DS_LOW_H, 0.06f, 1200);
  for (int i = 0; i < 2; i++) { PL[i].cam_pitch = -0.15f; PL[i].zoom = 1; PL[i].fov = 70; }
  hemi = t3_hemisphere_light_new(0xe6eaff, 0x4a3c2c, 1.5f);
  t3_object_add(&scene->base, hemi);
  moon = t3_directional_light_new(0xfff0d8, 1.6f);
  t3_object_set_position(moon, -200, 400, 120);
  t3_object_add(&scene->base, moon);
  world_init(scene);
  for (int i = 0; i < HERO_COUNT; i++) heroes[i] = toy_new((hero_id)i, scene);
  pick_hero(DS_SAVE.hero < HERO_COUNT ? (hero_id)DS_SAVE.hero : HERO_MOHEEB);
  PL[0].first_person = DS_SAVE.first_person;
  enemy_init(scene);
  weapon_init(scene);
  mission_init(scene);
  cutscene_init(scene);
  for (unsigned i = 0; i < sizeof SPOKEN / sizeof SPOKEN[0]; i++) voice_prepare(VO_ALERT, SPOKEN[i]);
  fx_init(scene);
  hud_init();
  audio_set_music(DS_SAVE.music);
#ifdef SP_DUMP   /* tools: every line the game can speak, for tools/voices.mjs */
  for (int h = 0; h < HERO_COUNT; h++) {
    for (int c = 0; c < CUT_COUNT; c++) {
      cutscene_start((cut_id)c, (hero_id)h);
      for (int k = 0; k < 400 && cutscene_active(); k++) cutscene_update(0.0001f, input_get(), &CV);
    }
    for (int l = 0; l < MISSION_N; l++)
      for (int b = 0; b < mission_radio_count(l); b++) { const char *spk, *line = mission_radio(l, b, &spk); if (line) voice_prepare(cutscene_voice_for(spk), line); }
    voice_prepare(cutscene_voice_for(toy_name((hero_id)(h ^ 1))), "I'm fine. Give me a second.");
  }
  ds_log("[vo] done");
#endif
  to_title();
}

static hero_id hero_of(const player_t *p) { return p == &PL[0] ? who : (hero_id)(who ^ 1); }

static void hurt(player_t *p, float dmg, v3 from) {
  if (dmg <= 0 || p->dodge_t > 0 || p->hurt_iframes > 0 || st != ST_PLAY || p->down_t > 0) return;
  p->hp -= dmg;
  p->hurt_iframes = 0.3f;
  combat_t = 4;
  p->hud.hurt = 0.5f;
  v3 to = v3_sub(from, p->pos);
  p->hud.hurt_angle = ds_wrap_pi(atan2f(to.x, to.z) - atan2f(p->cam_fwd.x, p->cam_fwd.z));
  p->shake = fmaxf(p->shake, 0.2f);
  sfx(SFX_HURT, 0, 1);
  if (p->hp > 0) return;
  p->hp = 0; p->scoped = false;
  toy_play(p->toy, "Hit_Head", 0.7f, false);
  if (coop && other(p)->down_t <= 0) {
    /* two players: down for a few seconds, up again at the other one's side */
    p->down_t = DOWN_T;
    weapon_set_player(p == &PL[1]); weapon_cancel(); weapon_set_player(0);
    voice_say(cutscene_voice_for(toy_name(hero_of(p))), "I'm fine. Give me a second.");
    return;
  }
  st = ST_DEAD; st_t = 0; set_scoped_off();
  H.quip = story_quip((int)(level_time * 7) + kills);
  music_mood(MOOD_SILENT);
}

static void update_camera(player_t *p, float dt, const intent_t *I) {
  float inv = DS_SAVE.invert_y ? -1.f : 1.f;
  float sens = p->scoped ? 1.5f / (float)(1 << (p->zoom - 1)) : 2.8f;
  p->cam_yaw -= I->ax * sens * dt;
  p->cam_pitch = ds_clamp(p->cam_pitch + I->ay * inv * sens * 0.7f * dt, -1.35f, 1.3f);
  p->cam_fwd = v3_(-sinf(p->cam_yaw) * cosf(p->cam_pitch), sinf(p->cam_pitch), -cosf(p->cam_yaw) * cosf(p->cam_pitch));
  v3 right = v3_norm(v3_cross(p->cam_fwd, v3_(0, 1, 0)));
  v3 want;
  float want_fov = ZOOM_FOV[0];
  bool fp = p->first_person && p->down_t <= 0;   /* down: over the shoulder, to see yourself on the floor */
  if (p->scoped) { want = v3_add(p->pos, v3_(0, HERO_H * 0.9f, 0)); want_fov = ZOOM_FOV[p->zoom]; }
  else if (fp) { want = v3_add(p->pos, v3_(0, HERO_H * 0.92f, 0)); want = v3_add(want, v3_scale(v3_(p->cam_fwd.x, 0, p->cam_fwd.z), 0.15f)); }
  else {
    /* over the right shoulder, pulled in when something is behind */
    v3 focus = v3_add(v3_add(p->pos, v3_(0, HERO_H * 0.85f, 0)), v3_scale(right, 0.55f));
    float dist = CAM_DIST + (p->jetting ? 0.8f : 0);
    v3 back = v3_scale(p->cam_fwd, -1);
    float t; v3 n;
    if (world_raycast(focus, back, dist + 0.3f, &t, &n)) dist = fmaxf(0.35f, t - 0.3f);
    want = v3_add(focus, v3_scale(back, dist));
    want_fov += p->dodge_t > 0 ? 6 : 0;
  }
  p->fov = ds_damp(p->fov, want_fov, p->scoped ? 18 : 10, dt);
  p->cam_pos = (p->scoped || fp) ? want : v3_lerp(p->cam_pos, want, 1 - expf(-18 * dt));
  v3 c = p->cam_pos;
  if (p->shake > 0 && DS_SAVE.shake) c = v3_add(c, v3_(ds_rr(-1, 1) * p->shake * 0.15f, ds_rr(-1, 1) * p->shake * 0.15f, ds_rr(-1, 1) * p->shake * 0.15f));
  p->shake = fmaxf(0, p->shake - dt * 1.5f);
  /* a half of the screen in two-player mode, the whole of it with one */
  p->cam->aspect = split ? (float)(DS_LOW_W / 2) / DS_LOW_H : (float)DS_W / DS_H;
  p->cam->fov = p->fov;
  t3_camera_update_projection_matrix(p->cam);
  t3_object_set_position(p->cam, c.x, c.y, c.z);
  t3_object_look_at(p->cam, c.x + p->cam_fwd.x, c.y + p->cam_fwd.y, c.z + p->cam_fwd.z);
  if (p == &PL[0]) audio_listener(c.x, c.y, c.z, p->cam_yaw);
}

/* put a hero beside another: the left shoulder, else the right, else behind,
 * else on the other's own spot; somewhere with a floor, room to stand, and in sight */
static void warp_beside(player_t *b, const player_t *p) {
  v3 chest = v3_add(p->pos, v3_(0, HERO_H * 0.6f, 0));
  v3 cand[4] = {beside(p->pos, p->yaw, -2.2f), beside(p->pos, p->yaw, 2.2f), v3_add(p->pos, v3_(-sinf(p->yaw) * 2, 0, -cosf(p->yaw) * 2)), p->pos};
  v3 to = p->pos;
  for (int i = 0; i < 4; i++) {
    v3 c = cand[i];
    float f = world_floor_y(v3_(c.x, p->pos.y + 1.0f, c.z));
    if (f < p->pos.y - 1.5f || f > p->pos.y + 1.0f) continue;
    c.y = f;
    v3 body = v3_add(c, v3_(0, HERO_H * 0.6f, 0)), n, probe = body;
    if (world_push_sphere(&probe, HERO_R, &n) || !world_visible(chest, body)) continue;
    to = c; break;
  }
  fx_poof(v3_add(b->pos, v3_(0, 0.9f, 0)), C_CREAM, 0.8f);
  b->pos = v3_add(to, v3_(0, 0.05f, 0)); b->vel = v3_(0, 0, 0); b->yaw = p->yaw; b->apart_t = 0;
  fx_poof(v3_add(b->pos, v3_(0, 0.9f, 0)), C_CREAM, 0.8f);
}

/* the buddy: keeps to the player's left shoulder, jets up after them, shoots
 * what is nearest and in sight, and gets back up a few seconds after a fall */
static void update_buddy(float dt) {
  player_t *P = &PL[0], *b = &PL[1];
  if (b->down_t > 0) {
    b->down_t -= dt;
    b->vel.x = b->vel.z = 0; b->vel.y -= GRAVITY * dt;
    world_move(&b->pos, &b->vel, HERO_R, HERO_H, dt, &b->grounded);
    if (b->down_t <= 0) { b->hp = HP_MAX * 0.6f; toy_play(b->toy, "KipUp", 1, false); }
    toy_jet(b->toy, 0);
    return;
  }
  v3 goal = beside(P->pos, P->yaw, -2.2f);
  v3 to = v3_sub(goal, b->pos);
  float flat = sqrtf(to.x * to.x + to.z * to.z);
  /* separated: too far, out of sight, or left below while the player climbed. Past a moment
   * of that the buddy warps to the player rather than wandering the lab looking for a way */
  float apart = v3_len(v3_sub(P->pos, b->pos));
  v3 chest = v3_add(P->pos, v3_(0, HERO_H * 0.6f, 0)), bchest = v3_add(b->pos, v3_(0, HERO_H * 0.6f, 0));
  bool far_off = apart > 26, hidden = !world_visible(chest, bchest), below = P->pos.y - b->pos.y > 5 && P->grounded;
  b->apart_t = (far_off || hidden || below) ? b->apart_t + dt : 0;
  if (apart > 60 || b->apart_t > (far_off ? 1.0f : below ? 2.5f : 2.0f)) { warp_beside(b, P); return; }
  float sp = flat > 6 ? RUN_SPEED * 1.15f : flat > 1.2f ? RUN_SPEED * 0.7f : 0;
  v3 dir = flat > 0.01f ? v3_(to.x / flat, 0, to.z / flat) : v3_(0, 0, 0);
  b->vel.x = ds_damp(b->vel.x, dir.x * sp, b->grounded ? 12 : 4, dt);
  b->vel.z = ds_damp(b->vel.z, dir.z * sp, b->grounded ? 12 : 4, dt);
  /* up after the player: a hop, then the jetpack while they are still above */
  bool want_up = P->pos.y - b->pos.y > 1.2f && flat < 14;
  if (b->grounded && (want_up || (flat > 2 && b->vel.x * b->vel.x + b->vel.z * b->vel.z < 1))) { b->vel.y = JUMP_V; b->grounded = false; }
  b->jetting = !b->grounded && want_up && b->jet > 0.05f && b->vel.y < JET_MAX_UP;
  if (b->jetting) { b->vel.y += JET_ACCEL * dt; b->jet -= JET_DRAIN * dt; }
  else if (b->grounded) b->jet = fminf(1, b->jet + JET_CHARGE * dt);
  b->vel.y -= GRAVITY * dt;
  world_move(&b->pos, &b->vel, HERO_R, HERO_H, dt, &b->grounded);
  if (b->pos.y < -70) b->pos = beside(P->pos, P->yaw, -1.6f);
  /* fight */
  b->fire_cd -= dt;
  v3 eye = v3_add(b->pos, v3_(0, HERO_H * 0.8f, 0));
  int target = st == ST_PLAY ? enemy_nearest(eye, 45, true) : -1;
  float face = flat > 1.2f ? atan2f(dir.x, dir.z) : P->yaw;
  if (target >= 0) {
    v3 tp = v3_add(enemy_pos(target), v3_(0, 0.9f, 0));
    v3 d = v3_norm(v3_sub(tp, eye));
    face = atan2f(d.x, d.z);
    toy_aim(b->toy, asinf(ds_clamp(d.y, -1, 1)), 1);
    if (b->fire_cd <= 0) {
      b->fire_cd = ds_rr(0.35f, 0.6f);
      v3 m = toy_muzzle(b->toy);
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
  } else toy_aim(b->toy, 0, 0);
  b->yaw += ds_wrap_pi(face - b->yaw) * fminf(1, dt * 10);
  /* hurt */
  v3 from;
  float dmg = enemy_hurt(1, &from);
  if (dmg > 0 && st == ST_PLAY) {
    b->hp -= dmg * 0.6f;
    if (b->hp <= 0) { b->hp = 0; b->down_t = DOWN_T; toy_play(b->toy, "Hit_Chest", 0.8f, false); voice_say(cutscene_voice_for(toy_name(who ^ 1)), "I'm fine. Give me a second."); }
  }
  /* animation */
  float hs = sqrtf(b->vel.x * b->vel.x + b->vel.z * b->vel.z);
  if (!b->grounded) toy_play(b->toy, "Jump_Loop", 1, true);
  else if (hs > 4.5f) toy_play(b->toy, "Sprint_Loop", hs / 8, true);
  else if (hs > 0.6f) toy_play(b->toy, "Jog_Fwd_Loop", fmaxf(0.6f, hs / 5), true);
  else toy_play(b->toy, "Idle_Loop", 1, true);
  toy_jet(b->toy, b->jetting ? 1 : 0);
  if (b->jetting) sfx_at(SFX_JET, b->pos.x, b->pos.y, b->pos.z, 0.4f);
}

/* a player's legs: the view toggle, running, the dodge, the jump and the jetpack.
 * Down (two-player mode), they lie still until they get up at the other's side */
static void move_player(player_t *p, float dt, const intent_t *I) {
  p->hurt_iframes -= dt; p->swing_t -= dt;
  if (p->down_t > 0) {
    p->down_t -= dt;
    p->vel.x = p->vel.z = 0; p->vel.y -= GRAVITY * dt;
    world_move(&p->pos, &p->vel, HERO_R, HERO_H, dt, &p->grounded);
    p->jetting = false; p->aiming = false;
    toy_jet(p->toy, 0); toy_aim(p->toy, 0, 0);
    /* the fall, then flat on the floor: the kip-up's first frame, held until it plays */
    if (toy_clip_done(p->toy)) toy_play(p->toy, "KipUp", 0, false);
    if (p->down_t <= 0) {
      warp_beside(p, other(p));
      p->hp = HP_MAX * 0.6f; p->hurt_iframes = 1.5f; p->down_t = 0;
      toy_play(p->toy, "KipUp", 1, false);
    }
    return;
  }
  /* the view */
  if (I->view) { p->first_person = !p->first_person; if (p == &PL[0]) DS_SAVE.first_person = p->first_person; sfx(SFX_UI, 0, 0.6f); }
  /* movement relative to the camera */
  v3 fwd = v3_(-sinf(p->cam_yaw), 0, -cosf(p->cam_yaw)), right = v3_(cosf(p->cam_yaw), 0, -sinf(p->cam_yaw));
  v3 wish = v3_add(v3_scale(fwd, I->my), v3_scale(right, I->mx));
  float wl = v3_len(wish);
  if (wl > 1) { wish = v3_scale(wish, 1 / wl); wl = 1; }
  float speed = (I->aim ? AIM_SPEED : RUN_SPEED) * weapon_glue_slow(p->pos);
  if (I->dodge && p->dodge_cd <= 0 && (p->grounded || p->jet > 0.15f)) {
    p->dodge_t = DODGE_T; p->dodge_cd = 0.6f;
    p->dodge_dir = wl > 0.1f ? v3_norm(wish) : v3_(sinf(p->yaw), 0, cosf(p->yaw));
    if (!p->grounded) { p->jet -= 0.15f; p->vel.y = fmaxf(p->vel.y, 2); }
    sfx(SFX_DODGE, 0, 1);
  }
  p->dodge_cd -= dt;
  p->aiming = I->aim || I->fire_held || p->swing_t > 0;
  if (p->dodge_t > 0) {
    p->dodge_t -= dt;
    p->vel.x = p->dodge_dir.x * DODGE_V; p->vel.z = p->dodge_dir.z * DODGE_V;
    p->yaw = atan2f(p->dodge_dir.x, p->dodge_dir.z);
  } else {
    float ctl = p->grounded ? 14 : p->jetting ? 5 : 3;
    p->vel.x = ds_damp(p->vel.x, wish.x * speed, ctl, dt);
    p->vel.z = ds_damp(p->vel.z, wish.z * speed, ctl, dt);
    float face = (p->aiming || p->first_person) ? atan2f(p->cam_fwd.x, p->cam_fwd.z) : (wl > 0.1f ? atan2f(wish.x, wish.z) : p->yaw);
    p->yaw += ds_wrap_pi(face - p->yaw) * fminf(1, dt * 16);
  }
  /* jump, then the jetpack while A is held in the air */
  if (p->grounded && I->jump) { p->vel.y = JUMP_V; p->grounded = false; sfx(SFX_JUMP, 0, 0.8f); p->jet_delay = 0.18f; }
  p->jet_delay -= dt;
  bool was_jet = p->jetting;
  p->jetting = !p->grounded && I->jump_held && p->jet > 0 && p->jet_delay <= 0 && p->dodge_t <= 0;
  if (p->jetting) {
    p->vel.y = fminf(JET_MAX_UP, p->vel.y + JET_ACCEL * dt);
    p->jet = fmaxf(0, p->jet - JET_DRAIN * dt);
    sfx(SFX_JET, 0, 0.7f);
    if (p->jet <= 0) { sfx(SFX_JET_EMPTY, 0, 1); alert("BATTERY LOW"); }
  } else if (p->grounded) {
    p->charge_wait = was_jet ? JET_DELAY : p->charge_wait - dt;
    if (p->charge_wait <= 0) p->jet = fminf(1, p->jet + JET_CHARGE * dt);
  }
  p->vel.y -= GRAVITY * dt;
  float vy0 = p->vel.y;
  bool was_grounded = p->grounded;
  world_move(&p->pos, &p->vel, HERO_R, HERO_H, dt, &p->grounded);
  if (p->grounded && !was_grounded && vy0 < -8) { sfx(SFX_LAND, 0, fminf(1, -vy0 / 25)); p->shake = fmaxf(p->shake, fminf(0.2f, -vy0 / 120)); }
  if (p->pos.y < -70) { p->hurt_iframes = 0;   /* below the basement */ hurt(p, 20, p->pos); if (st == ST_PLAY) { p->pos = checkpoint; p->vel = v3_(0, 0, 0); } }
}

/* a player's hands: the scope, then their kit (weapon.c) along their camera's ray */
static void arm_player(player_t *p, float dt, const intent_t *I, weapon_out_t *wo) {
  /* scope: the coilgun aimed */
  weapon_set_player(p == &PL[1]);
  bool want_scope = I->aim && weapon_current() == WP_COIL && p->dodge_t <= 0;
  if (want_scope != p->scoped) { p->scoped = want_scope; if (p->scoped) p->zoom = 1; }
  if (p->scoped && I->zoom) { int z = p->zoom + I->zoom; if (z >= 1 && z <= 3) { p->zoom = z; sfx(SFX_UI, 0, 0.4f); } }
  /* weapons */
  weapon_in_t wi = {0};
  wi.eye = p->cam_pos; wi.dir = p->cam_fwd;
  if (!p->first_person && !p->scoped) {
    /* start the aim ray at the hero, not behind them: what the reticle is on, seen from the shoulder */
    float t; v3 n; v3 far = v3_add(p->cam_pos, v3_scale(p->cam_fwd, 400));
    if (world_raycast(p->cam_pos, p->cam_fwd, 400, &t, &n)) far = v3_add(p->cam_pos, v3_scale(p->cam_fwd, t));
    v3 o = v3_add(p->cam_pos, v3_scale(p->cam_fwd, fmaxf(0, v3_dot(v3_sub(p->pos, p->cam_pos), p->cam_fwd))));
    wi.eye = o; wi.dir = v3_norm(v3_sub(far, o));
  }
  wi.muzzle = toy_muzzle(p->toy);
  wi.body = v3_add(p->pos, v3_(0, HERO_H * 0.6f, 0));
  wi.yaw = p->yaw;
  wi.fire = I->fire; wi.fire_held = I->fire_held; wi.fire_released = I->fire_released;
  bool used = I->melee && mission_interact(p->pos);
  wi.aim = I->aim; wi.throw_lipo = I->lipo; wi.melee = I->melee && !used;
  wi.next = I->next; wi.prev = I->prev; wi.select = I->select;
  weapon_update(dt, &wi, wo);
#ifdef DS_TRACE
  if (wi.fire || wo->recoil > 0 || wo->threw || wo->swing || wo->hit) ds_log("[fire] p%d w%d fire%d held%d ammo%d hit%d swing%d threw%d charge %.2f", p == &PL[1], weapon_current(), wi.fire, wi.fire_held, weapon_ammo(weapon_current()), wo->hit, wo->swing, wo->threw, wo->charge);
#endif
  weapon_set_player(0);
}

/* what the kit did this frame, for the camera, the HUD and the hero */
static void arm_effects(player_t *p, const weapon_out_t *wo, const intent_t *I) {
  if (wo->recoil > 0) p->shake = fmaxf(p->shake, wo->recoil * 0.3f);
  if (wo->hit) { p->hud.hit_marker = 0.2f; p->hud.hit_weak = wo->hit_weak; combat_t = 4; }
  if (wo->killed) kills++;
  if (wo->swing) { p->swing_t = 0.35f; toy_play(p->toy, "Punch_Cross", 1.6f, false); }
  if (I->fire_held) combat_t = 4;
  p->hud.charge = wo->charge;
}

/* the objective arrow: where the mission's marker is on this player's view, in
 * pixels of their part of the screen (w wide, DS_H tall) */
static void marker_project(player_t *p, v3 at, float w) {
  v3 rel = v3_sub(at, p->cam_pos);
  v3 rgt = v3_norm(v3_cross(p->cam_fwd, v3_(0, 1, 0))), up = v3_cross(rgt, p->cam_fwd);
  float z = v3_dot(rel, p->cam_fwd), f = 1 / tanf(p->fov * DS_PI / 360);
  p->hud.marker_dist = v3_len(rel);
  p->hud.marker_behind = z < 0.1f;
  float zz = fmaxf(fabsf(z), 0.1f);
  p->hud.marker_x = w * 0.5f + v3_dot(rel, rgt) / zz * f * DS_H * 0.5f;
  p->hud.marker_y = DS_H * 0.5f - v3_dot(rel, up) / zz * f * DS_H * 0.5f;
  p->hud.marker_on = true;
}

/* the clip from the legs, the aim layer from the weapon */
static void animate(player_t *p) {
  float hs = sqrtf(p->vel.x * p->vel.x + p->vel.z * p->vel.z);
  bool aiming = p->aiming;
  if (st != ST_PLAY) {}
  else if (p->down_t > 0) {}
  else if (p->swing_t > 0) {}
  else if (p->dodge_t > 0) {
    float side = p->vel.x * cosf(p->yaw) - p->vel.z * sinf(p->yaw);
    toy_play(p->toy, side < 0 ? "Dodge_Left" : "Dodge_Right", 1.6f, false);
  }
  else if (!p->grounded) toy_play(p->toy, "Jump_Loop", 1, true);
  else if (hs > 0.6f) {
    if (aiming) {
      /* strafing while aiming: the jog that matches the direction relative to the facing */
      float lf = p->vel.x * sinf(p->yaw) + p->vel.z * cosf(p->yaw), ls = p->vel.x * cosf(p->yaw) - p->vel.z * sinf(p->yaw);
      const char *c = fabsf(lf) >= fabsf(ls) ? (lf >= 0 ? "Jog_Fwd_Loop" : "Jog_Bwd_Loop") : (ls >= 0 ? "Jog_Left_Loop" : "Jog_Right_Loop");
      toy_play(p->toy, c, fmaxf(0.6f, hs / 4.5f), true);
    } else toy_play(p->toy, hs > 5.5f ? "Sprint_Loop" : "Jog_Fwd_Loop", fmaxf(0.6f, hs / (hs > 5.5f ? 8.f : 5.f)), true);
  }
  else toy_play(p->toy, "Idle_Loop", 1, true);
  if (p->down_t > 0) return;
  toy_aim(p->toy, asinf(ds_clamp(p->cam_fwd.y, -1, 1)), (aiming && p->swing_t <= 0) ? 1 : 0.0f);
  weapon_set_player(p == &PL[1]);
  toy_set_weapon(p->toy, weapon_current());
  weapon_set_player(0);
  toy_jet(p->toy, p->jetting ? 1 : 0);
}

static void update_play(float dt, const intent_t *I) {
  player_t *P = &PL[0];
#ifdef DS_TRACE
  if (input_key_down(WC_KEY_8)) P->hp = HP_MAX;
  /* test only: 7 onto the basement hatch gap, 6 onto the hatch gap */
  if (input_key_down(WC_KEY_7)) { P->pos = v3_(690, 0.5f, 33); P->cam_yaw = DS_PI * 0.5f; P->vel = v3_(0, 0, 0); P->cam_yaw = DS_PI * 0.5f; }
  if (input_key_down(WC_KEY_5)) { P->pos = v3_(666, -10.4f, 27.5f); P->vel = v3_(0, 0, 0); P->cam_yaw = 0; }   /* the top shelf's front edge, under the gap */
  if (input_key_down(WC_KEY_6)) { P->pos = v3_(666, 0.5f, 22); P->vel = v3_(0, 0, 0); P->cam_yaw = DS_PI * 0.0f; }
  /* test only: 9 knocks player 2 down, 0 player 1, 4 puts player 2 on the objective arrow (two players) */
  static v3 arrow_at;
  if (coop && input_key_down(WC_KEY_4)) { PL[1].pos = v3_add(arrow_at, v3_(0, -1, 0)); PL[1].vel = v3_(0, 0, 0); }
  if (coop && input_key_down(WC_KEY_9)) { PL[1].hurt_iframes = PL[1].dodge_t = 0; hurt(&PL[1], 500, PL[1].pos); }
  if (coop && input_key_down(WC_KEY_0)) { P->hurt_iframes = P->dodge_t = 0; hurt(P, 500, P->pos); }
#endif
  level_time += dt;
  combat_t -= dt;
  int n = coop ? 2 : 1;
  const intent_t *IN[2] = {I, input_get_player(1)};
  for (int i = 0; i < n; i++) move_player(&PL[i], dt, IN[i]);
  /* the players' kits, then the shots in flight */
  weapon_out_t wo[2];
  memset(wo, 0, sizeof wo);
  weapon_tick(dt);
  for (int i = 0; i < n; i++) if (PL[i].down_t <= 0) arm_player(&PL[i], dt, IN[i], &wo[i]);
  weapon_step(dt, wo);
  for (int i = 0; i < n; i++) arm_effects(&PL[i], &wo[i], IN[i]);
  /* the army */
  for (int i = 0; i < n; i++) {
    v3 from;
    float dmg = enemy_hurt(i, &from);
    if (dmg > 0) hurt(&PL[i], dmg, from);
  }
  if (st != ST_PLAY) return;
  /* the mission: player 1, and player 2 alongside; when player 1 is down the mission follows player 2 */
  int first = coop && P->down_t > 0 && PL[1].down_t <= 0 ? 1 : 0, second = coop && first == 0 && PL[1].down_t <= 0 ? 1 : -1;
  mission_out_t mo; memset(&mo, 0, sizeof mo); mo.radio_beat = mo.pickup = mo.weapon_found = -1;
  mission_set_second(second >= 0, second >= 0 ? PL[second].pos : v3_(0, 0, 0));
  mission_update(dt, PL[first].pos, &mo);
  H.objective = mo.objective;
  for (int i = 0; i < n; i++) PL[i].hud.prompt = NULL;
  PL[first].hud.prompt = mo.prompt;
  if (second >= 0) PL[second].hud.prompt = mo.prompt2;
  for (int i = 0; i < n; i++) {
    PL[i].hud.marker_on = false;
    if (mo.marker) marker_project(&PL[i], mo.marker_pos, coop ? DS_W / 2 : DS_W);
  }
#ifdef DS_TRACE
  if (mo.marker) arrow_at = mo.marker_pos;
#endif
  if (mo.banner) banner(mo.banner);
  if (mo.checkpoint) { checkpoint = mission_checkpoint(); alert("CHECKPOINT"); sfx(SFX_UI_OK, 0, 0.8f); }
  if (mo.pickup >= 0) {
    player_t *tk = &PL[mo.taker == 1 && second >= 0 ? second : first];
    sfx(SFX_PICKUP, 0, 1);
    if (mo.pickup == PK_HEALTH) { for (int i = 0; i < 2; i++) if (PL[i].down_t <= 0) PL[i].hp = fminf(HP_MAX, PL[i].hp + 35); banner("ELECTRICAL TAPE. GOOD AS NEW."); }
    else if (mo.pickup == PK_BATTERY) { tk->jet = 1; banner("FRESH BATTERY"); }
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
    st = ST_CLEAR; st_t = 0; set_scoped_off();
    music_mood(MOOD_CLEAR);
    int ms = (int)(level_time * 1000);
    H.new_best = false;
    if (!DS_SAVE.best_ms[level] || ms < (int)DS_SAVE.best_ms[level]) { DS_SAVE.best_ms[level] = ms; H.new_best = true; }
    if ((uint32_t)score > DS_SAVE.best_score[level]) { DS_SAVE.best_score[level] = score; H.new_best = true; }
    if (DS_SAVE.level_reached < level + 1 && level + 1 < MISSION_N) DS_SAVE.level_reached = level + 1;
    for (int i = 0; i < n; i++) toy_play(PL[i].toy, "Celebration", 1, true);
  }
  for (int i = 0; i < n; i++) animate(&PL[i]);
  if (!coop) update_buddy(dt);
}

static void toggle_setting(int which) {
  if (which == 0) { DS_SAVE.music ^= 1; audio_set_music(DS_SAVE.music); }
  else if (which == 1) DS_SAVE.shake ^= 1;
  else if (which == 2) DS_SAVE.invert_y ^= 1;
  else { PL[0].first_person = !PL[0].first_person; DS_SAVE.first_person = PL[0].first_person; }
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
  player_t *P = &PL[0], *B = &PL[1];
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
  reel_move_hero(heroes[HERO_MOHEEB], R->a0, R->a1, R->ma, u, &P->pos, &P->yaw, &ja, 0);
  reel_move_hero(heroes[HERO_LUIS], R->b0, R->b1, R->mb, ds_clamp(u * 1.04f - 0.04f, 0, 1), &B->pos, &B->yaw, &jb, 1);
  /* game_frame places player 1's hero at PL[0] and the other at PL[1]: give each hero its own path whoever is picked */
  if (who == HERO_LUIS) { v3 tp = P->pos; P->pos = B->pos; B->pos = tp; float ty = P->yaw; P->yaw = B->yaw; B->yaw = ty; float tj = ja; ja = jb; jb = tj; }
  toy_jet(P->toy, ja); toy_jet(B->toy, jb);
  toy_set_weapon(P->toy, WP_BANDS); toy_set_weapon(B->toy, WP_BANDS);
  /* the army marches, nobody to shoot at */
  v3 mid = v3_scale(v3_add(P->pos, B->pos), 0.5f);
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
  P->cam_pos = u < 0.02f ? want : v3_lerp(P->cam_pos, want, 0.12f);
#ifdef SP_AIMTEST   /* a fixed side view of the aim pose */
  { float face; v3 at = mission_start(0, &face); P->pos = at; B->pos = beside(at, face, -1.4f); P->yaw = B->yaw = face;
    P->cam_pos = v3_add(at, v3_(-0.5f, 1.4f, 4.0f)); look = v3_add(at, v3_(0, 1.1f, 0));
    toy_aim(P->toy, 0.25f, 1); toy_aim(B->toy, -0.2f, 1); }
#endif
#ifdef SP_VIEW   /* test only: fixed views of the model's corrections, a few seconds each */
  { static const float V[][6] = {{690, 14, 4, 683, -30, 18}, {700, 9, 50, 683, 0, 26}, {735, -20, 95, 683, -36, 26}, {660, -38, 20, 700, -46, 60}, {350, 40, 95, 375, 0, 30}, {395, 12, 70, 382, 8, 30}};
    int k = (int)(t / 2.5f) % 6; P->cam_pos = v3_(V[k][0], V[k][1], V[k][2]); look = v3_(V[k][3], V[k][4], V[k][5]);
    toy_visible(P->toy, false); toy_visible(B->toy, false); }
#endif
  cam->fov = 52; cam->aspect = (float)DS_W / DS_H; t3_camera_update_projection_matrix(cam);
  t3_object_set_position(cam, P->cam_pos.x, P->cam_pos.y, P->cam_pos.z);
  t3_object_look_at(cam, look.x, look.y, look.z);
  audio_listener(P->cam_pos.x, P->cam_pos.y, P->cam_pos.z, 0);
#ifdef DS_TRACE
  { static int fr; if (++fr % 60 == 0) ds_log("[reel] sc%d u%.2f cam %.1f %.1f %.1f look %.1f %.1f %.1f me %.1f %.1f %.1f pal %.1f %.1f %.1f", sc, u, P->cam_pos.x, P->cam_pos.y, P->cam_pos.z, look.x, look.y, look.z, P->pos.x, P->pos.y, P->pos.z, B->pos.x, B->pos.y, B->pos.z); }
#endif
  H.title_place = R->place;
  H.title_scene = sc;
}

/* the PLAYERS row: two only with a second pad there */
static void set_players(int n) {
  bool want = n == 2;
  if (want == coop_want) return;
  if (want && !input_pad2()) { sfx(SFX_UI, 0, 0.4f); return; }
  coop_want = want;
  sfx(SFX_UI_OK, 0, 0.8f);
}

/* title rows (hud_screens.c): 0 the hero (left / right picks), then the
 * unlocked levels, then players (left / right picks), music, shake, invert
 * and the view; no wrapping */
static void update_title(const intent_t *I) {
  int unlocked = DS_SAVE.level_reached + 1;
  if (unlocked > MISSION_N) unlocked = MISSION_N;
  int rows = 1 + unlocked + 5;
  if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 0 && m < rows) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
  int set = H.menu_sel - 1 - unlocked;   /* the settings rows: 0 players, then toggle_setting's */
  if (H.menu_sel == 0 && I->nav_x) { pick_hero((hero_id)(I->nav_x > 0 ? HERO_LUIS : HERO_MOHEEB)); sfx(SFX_UI, 0, 0.6f); }
  if (set == 0 && I->nav_x) set_players(I->nav_x > 0 ? 2 : 1);
  if (I->ok && st_t > 0.4f) {
    if (H.menu_sel == 0) { H.menu_sel = 1; sfx(SFX_UI_OK, 0, 0.8f); }
    else if (H.menu_sel <= unlocked) { sfx(SFX_UI_OK, 0, 1); start_level(H.menu_sel - 1); return; }
    else if (set == 0) set_players(coop_want ? 1 : 2);
    else toggle_setting(set - 1);
  }
  H.unlocked = unlocked;
  title_reel((float)ds_now() - reel_t0);
}

/* pause rows: resume, restart, settings, leave co-op (two players only), quit;
 * settings opens the four rows after them */
static void update_pause(const intent_t *I) {
  int nm = coop ? 5 : 4, quit = nm - 1;
  if (H.menu_sel >= nm) {
    if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= nm && m < nm + 4) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
    if (I->ok) toggle_setting(H.menu_sel - nm);
    if (I->back) { H.menu_sel = 2; sfx(SFX_UI, 0, 0.6f); }
    return;
  }
  if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 0 && m < nm) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
  if (I->ok && H.menu_sel == 2) { H.menu_sel = nm; sfx(SFX_UI_OK, 0, 0.8f); return; }
  if (I->back || I->pause || (I->ok && H.menu_sel == 0)) { st = ST_PLAY; sfx(SFX_UI, 0, 0.6f); return; }
  if (I->ok && H.menu_sel == 1) { start_level(level); return; }
  if (I->ok && coop && H.menu_sel == 3) { coop_leave(); coop_want = false; st = ST_PLAY; H.menu_sel = 0; return; }
  if (I->ok && H.menu_sel == quit) { to_title(); return; }
}

/* what the HUD shows of a player; w is the width of their part of the screen */
static void hud_player(player_t *p, float w, float dt, const intent_t *I) {
  hud_player_t *h = &p->hud;
  h->name = toy_name(hero_of(p));
  h->health = p->down_t > 0 ? 0 : p->hp / HP_MAX;
  h->jet = p->jet; h->jetting = p->jetting;
  weapon_set_player(p == &PL[1]);
  h->weapon = weapon_current(); h->weapon_name = weapon_name(h->weapon); h->ammo = weapon_ammo(h->weapon);
  for (int k = 0; k < WP_COUNT; k++) h->owned[k] = weapon_unlocked(k);
  h->lipos = weapon_lipos();
  weapon_set_player(0);
  h->hit_marker = fmaxf(0, h->hit_marker - dt);
  h->hurt = fmaxf(0, h->hurt - dt);
  h->used_pad = I->used_pad;
  h->first_person = p->first_person;
  h->down = p->down_t;
  h->scoped = p->scoped; h->zoom = p->zoom;
  h->on_enemy = h->on_weak = false; h->weak_mark = false; h->range = 0;
  if (p->scoped) {
    float t, et; v3 n; int id; bool weak;
    bool hw = world_raycast(p->cam_pos, p->cam_fwd, 800, &t, &n);
    h->range = hw ? t : 0;
    if (enemy_raycast(p->cam_pos, p->cam_fwd, hw ? t : 800, &id, &et, &weak)) { h->on_enemy = true; h->on_weak = weak; h->range = et; }
    v3 at;
    if (enemy_weak_point_near(p->cam_pos, p->cam_fwd, cosf(ZOOM_FOV[p->zoom] * DS_PI / 180 * 0.5f), &at)) {
      v3 rel = v3_sub(at, p->cam_pos);
      v3 rgt = v3_norm(v3_cross(p->cam_fwd, v3_(0, 1, 0))), up = v3_cross(rgt, p->cam_fwd);
      float z = v3_dot(rel, p->cam_fwd);
      if (z > 0.1f) {
        float f = 1 / tanf(p->fov * DS_PI / 360);
        h->weak_x = w * 0.5f + v3_dot(rel, rgt) / z * f * DS_H * 0.5f;
        h->weak_y = DS_H * 0.5f - v3_dot(rel, up) / z * f * DS_H * 0.5f;
        h->weak_mark = true;
      }
    }
  }
}

#ifdef DS_TRACE
static void trace_count(t3_object *o, void *ctx) { int *c = ctx; c[0]++; if (o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH) c[1]++; }
#endif

void game_frame(float dt) {
  const intent_t *I = input_get(), *M = input_get_menu();
  player_t *P = &PL[0], *B = &PL[1];
  st_t += dt;
  fx_dt = 0;
  /* two players: a second pad drops in (play) or picks two (the title); its pad going away hands the hero back */
  if (coop && !input_pad2()) { coop_leave(); coop_want = false; }
  if (!input_pad2()) coop_want = false;
  if (input_join()) {
    if (st == ST_PLAY && !coop) coop_join();
    else if (st == ST_TITLE && !coop_want) { coop_want = true; sfx(SFX_UI_OK, 0, 0.8f); }
  }
  if (radio_prep < mission_radio_count(level) && st != ST_CUT && st != ST_TITLE) {
    const char *spk, *line = mission_radio(level, radio_prep++, &spk);
    if (line) voice_prepare(cutscene_voice_for(spk), line);
  }
#ifdef DS_TRACE
  { static int fr; if (++fr % 60 == 0) {
    ds_log("[trace] f%d st%d lvl%d pos %.1f %.1f %.1f vy %.1f hp %.0f jet %.2f g%d alive %d buddy %.1f %.1f %.1f area %s", fr, st, level, P->pos.x, P->pos.y, P->pos.z, P->vel.y, P->hp, P->jet, P->grounded, enemy_alive(), B->pos.x, B->pos.y, B->pos.z, world_area_name(world_area_at(P->pos)));
    if (coop) ds_log("[trace] p2 hp %.0f jet %.2f down %.1f g%d cam %.2f %.2f fp%d scope%d", B->hp, B->jet, B->down_t, B->grounded, B->cam_yaw, B->cam_pitch, B->first_person, B->scoped);
    const t3_render_info *ri = t3_renderer_info(DS_R);
    int cnt[2] = {0, 0}; t3_object_traverse(&scene->base, trace_count, cnt);
    ds_log("[trace] draw calls %u tris %u objects %d meshes %d", ri->calls, ri->triangles, cnt[0], cnt[1]); } }
#endif
  if (st == ST_PAUSE) update_pause(M);
  else {
    switch (st) {
      case ST_TITLE: update_title(M); break;
      case ST_PLAY:
        if (M->pause) { st = ST_PAUSE; H.menu_sel = 0; set_scoped_off(); sfx(SFX_UI, 0, 0.6f); break; }
        update_play(dt, I);
        break;
      case ST_DEAD:
        for (int i = 0; i < (coop ? 2 : 1); i++) {
          player_t *p = &PL[i];
          p->vel.x = p->vel.z = 0; p->vel.y -= GRAVITY * dt;
          world_move(&p->pos, &p->vel, HERO_R, HERO_H, dt, &p->grounded);
        }
        if (st_t > 1.2f && (M->ok || I->jump)) {
          st = ST_PLAY; st_t = 0;
          float face = P->yaw;
          reset_player(checkpoint, face);
          music_mood(MOOD_EXPLORE);
        }
        break;
      case ST_CLEAR:
        if (st_t > 1.5f && (M->ok || I->jump)) {
          if (level + 1 < MISSION_N) start_level(level + 1);
          else play_cut(CUT_ENDING, AFTER_END);
        }
        break;
      case ST_END:
        if (st_t > 3 && (M->ok || I->jump)) to_title();
        break;
      case ST_CUT:
        if (!cutscene_update(dt, M, &CV)) cut_finish();
        break;
      default: break;
    }
    if (st == ST_PLAY || st == ST_DEAD || st == ST_CLEAR) {
      world_update(dt, P->pos);
      enemy_target_t T0 = {P->pos, HERO_R, HERO_H, st == ST_PLAY && P->hp > 0 && P->down_t <= 0};
      enemy_target_t T1 = {B->pos, HERO_R, HERO_H, B->down_t <= 0 && (!coop || B->hp > 0)};
      enemy_update(dt, &T0, &T1);
      score += enemy_take_score();
    }
  }
  split = coop && (st == ST_PLAY || st == ST_PAUSE || st == ST_DEAD || st == ST_CLEAR);
  if (st != ST_PAUSE) {
    toy_place(P->toy, P->pos, P->yaw, 1);
    toy_place(B->toy, B->pos, B->yaw, 1);
    toy_visible(P->toy, !(P->first_person || P->scoped) || st != ST_PLAY);
    /* the buddy steps out of the picture rather than filling it when the camera is on top of him */
    toy_visible(B->toy, st != ST_PLAY || v3_len(v3_sub(v3_add(B->pos, v3_(0, HERO_H * 0.6f, 0)), P->cam_pos)) > 1.4f);
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
      cam->aspect = (float)DS_W / DS_H;
      t3_camera_update_projection_matrix(cam);
      t3_object_set_position(cam, CV.cam_pos.x, CV.cam_pos.y, CV.cam_pos.z);
      t3_object_look_at(cam, CV.cam_target.x, CV.cam_target.y, CV.cam_target.z);
      P->cam_pos = CV.cam_pos;
      hemi->intensity = 1.5f * (0.25f + 0.75f * (CV.lights > 0 ? CV.lights : 1));
    } else {
      hemi->intensity = 1.5f;
      if (st != ST_TITLE) {
        update_camera(P, dt, I);
        if (coop) update_camera(B, dt, input_get_player(1));
      }
    }
    for (int i = 0; i < HERO_COUNT; i++) toy_update(heroes[i], dt);
    /* split screen: the particles face each camera in turn as it is drawn, then age (game_draw) */
    if (split) fx_dt = dt;
    else fx_update(dt, cam);
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
  hud_player(P, coop ? DS_W / 2 : DS_W, dt, I);
  if (coop) hud_player(B, DS_W / 2, dt, input_get_player(1));
  const hud_player_t *h1 = &P->hud;
  H.health = h1->health; H.buddy_health = B->down_t > 0 ? 0 : B->hp / HP_MAX;
  H.jet = h1->jet; H.jetting = h1->jetting;
  H.weapon = h1->weapon; H.weapon_name = h1->weapon_name; H.ammo = h1->ammo;
  for (int w = 0; w < WP_COUNT; w++) H.owned[w] = h1->owned[w];
  H.lipos = h1->lipos; H.charge = h1->charge;
  H.score = score; H.time_s = level_time;
  H.hit_marker = h1->hit_marker; H.hit_weak = h1->hit_weak;
  H.hurt = h1->hurt; H.hurt_angle = h1->hurt_angle;
  H.prompt = h1->prompt;
  H.marker_on = h1->marker_on; H.marker_x = h1->marker_x; H.marker_y = h1->marker_y;
  H.marker_behind = h1->marker_behind; H.marker_dist = h1->marker_dist;
  H.used_pad = I->used_pad;
  H.first_person = P->first_person;
  H.banner_t = fmaxf(0, H.banner_t - dt);
  if (H.banner_t <= 0) H.banner = NULL;
  H.scoped = h1->scoped; H.zoom = h1->zoom;
  H.on_enemy = h1->on_enemy; H.on_weak = h1->on_weak; H.weak_mark = h1->weak_mark; H.range = h1->range;
  H.weak_x = h1->weak_x; H.weak_y = h1->weak_y;
  H.split = split; H.coop = coop;
  H.pl[0] = P->hud; H.pl[1] = B->hud;
  H.pad2 = input_pad2();
  H.join_hint = !coop && H.pad2;
  H.players = (st == ST_TITLE ? coop_want : coop) ? 2 : 1;
  H.boss = enemy_boss_active(); H.boss_health = enemy_boss_health(); H.boss_phase = enemy_boss_phase();
  H.boss_name = enemy_boss_name();
  H.music_on = DS_SAVE.music; H.shake_on = DS_SAVE.shake; H.invert_y = DS_SAVE.invert_y;
  H.kills = kills; H.kills_total = kills + enemy_alive(); H.clear_time_s = level_time;
  H.best_time_ms = DS_SAVE.best_ms[level]; H.best_score = DS_SAVE.best_score[level];
  /* joins are taken in play and on the title, from the next frame */
  input_set_join_open(st == ST_PLAY || st == ST_TITLE);
}

/* split screen, ahead of each half: who is in the picture for this viewer, and the particles turned to their camera */
static void view_before(int i) {
  player_t *v = &PL[i], *o = other(v);
  toy_visible(v->toy, !(v->first_person || v->scoped) || v->down_t > 0 || st != ST_PLAY);
  toy_visible(o->toy, st != ST_PLAY || v3_len(v3_sub(v3_add(o->pos, v3_(0, HERO_H * 0.6f, 0)), v->cam_pos)) > 1.4f);
  fx_view(v->cam);
}

void game_draw(void) {
  if (split) {
    render_scene_split(scene, PL[0].cam, PL[1].cam, view_before);
    if (fx_dt > 0) fx_age(fx_dt);
  } else render_scene(scene, cam);
#ifndef SP_NOHUD   /* test only: clean frames for the web page and the station image */
  hud_draw(&H);
#endif
}
