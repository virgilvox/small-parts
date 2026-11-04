/* hud.h: everything 2D. The play HUD, the coilgun scope, and every screen
 * and menu, painted with cv.h and composited over the frame at 1280x720
 * (the scene under it is the low resolution PS1 pass). game.c fills a hud_t
 * each frame; hud_draw paints it. */
#ifndef SP_HUD_H
#define SP_HUD_H
#include "ds.h"

typedef enum {
  HUD_TITLE,        /* logo, the hero pick (MOHEEB / LUIS), level select once unlocked, settings */
  HUD_INTRO,        /* the level card over the first seconds of a level */
  HUD_PLAY,
  HUD_PAUSE,
  HUD_DEAD,         /* "back to the checkpoint" */
  HUD_CLEAR,        /* level stats */
  HUD_END,          /* after the ending: credits */
  HUD_CINE          /* a cutscene: nothing but hud_cine.c's captions */
} hud_mode;

/* one player's half of the screen in two-player mode (hud_t.split): what the
 * one-player HUD shows of the player, positions in the half's own pixels
 * (640 x 720, its centre at 320, 360) */
typedef struct {
  const char *name;            /* the hero */
  float health, jet;           /* 0..1 */
  bool jetting;
  int weapon; const char *weapon_name; int ammo; bool owned[4]; int lipos;
  float charge;
  float hit_marker; bool hit_weak;
  float hurt, hurt_angle;
  bool used_pad, first_person;
  const char *prompt;
  bool marker_on; float marker_x, marker_y; bool marker_behind; float marker_dist;
  bool scoped; int zoom; float range; bool on_enemy, on_weak;
  bool weak_mark; float weak_x, weak_y;
  float down;                  /* > 0: on the floor, seconds until they get up */
} hud_player_t;

typedef struct {
  hud_mode mode;
  float t;                     /* seconds in this mode */
  int level;                   /* 0-based */
  const char *level_name, *level_blurb;
  int hero;                    /* hero.h HERO_*: who the player is */
  const char *hero_name, *buddy_name;
  /* play */
  float health;                /* 0..1 */
  float buddy_health;          /* 0..1; 0 = the buddy is down (getting back up) */
  float jet;                   /* jetpack battery 0..1 */
  bool jetting;                /* thrust on this frame */
  int weapon;                  /* weapon.h WP_* */
  const char *weapon_name;
  int ammo;                    /* the current weapon's ammo, -1 endless */
  bool owned[4];               /* weapons found so far (WP_COUNT) */
  int lipos;
  float charge;                /* coilgun charge 0..1 */
  int score;
  float time_s;
  float hit_marker;            /* >0: show the hit marker (seconds left) */
  bool hit_weak;
  float hurt;                  /* >0: red edge flash */
  float hurt_angle;            /* where the hurt came from, radians, 0 = ahead, + = right */
  bool used_pad;               /* draw pad prompts, else key caps */
  bool first_person;
  const char *banner;          /* centre message ("CHECKPOINT"), NULL = none */
  float banner_t;
  const char *objective;       /* the mission's line, NULL = none */
  const char *prompt;          /* an interact prompt ("PULL THE PLUG"): show it big with the X / E button */
  bool marker_on; float marker_x, marker_y; bool marker_behind; float marker_dist;  /* objective arrow, screen pixels */
  /* scope (coilgun) */
  bool scoped;
  int zoom;                    /* 1..3 */
  float range;                 /* distance to what the reticle is on, units (the HUD shows cm: units * 5) */
  bool on_enemy, on_weak;
  bool weak_mark; float weak_x, weak_y;
  /* boss */
  bool boss; float boss_health; int boss_phase; const char *boss_name;
  /* menus: the selected row; title rows are hero, the unlocked levels, then players, music, shake, invert, view;
   * pause rows are resume, restart, settings, leave co-op (two players only), quit, then the four settings */
  int menu_sel;
  int unlocked;
  bool music_on, shake_on, invert_y;
  /* clear */
  int kills, kills_total; float clear_time_s; int best_time_ms; int best_score; bool new_best;
  /* cutscenes (hud_cine.c): letterbox 0..1, the caption being typed, skip hold 0..1, white flash, fade */
  float cine;
  const char *cap_speaker, *cap_text; int cap_chars;
  float skip, flash, fade;
  /* banter during play: a caption at the bottom, seconds left */
  const char *radio_speaker, *radio_text; float radio_t;
  const char *quip;            /* the death screen's line */
  const char *title_place; int title_scene;   /* the title reel's scene: where in the lab it is */
  /* two players */
  bool split;                  /* split screen: pl[0] the left half (player 1), pl[1] the right; the fields above are shared */
  hud_player_t pl[2];
  bool coop;                   /* player 2 is in (the pause menu's LEAVE CO-OP row) */
  bool join_hint;              /* one player and a second pad: "P2 PRESS START TO JOIN" */
  int players;                 /* the title's PLAYERS row: 1 or 2 */
  bool pad2;                   /* a second pad is there (the row can be 2) */
} hud_t;

void hud_init(void);
void hud_draw(const hud_t *h);
void hud_cine_draw(const hud_t *h);

#endif
