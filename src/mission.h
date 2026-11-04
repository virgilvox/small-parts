/* mission.h: the nine levels laid over the one lab (docs/STORY.md, docs/DESIGN.md).
 * Each has a start, a route through real areas of the model, objectives,
 * the army's squads, pickups, checkpoints, walkie-talkie beats and an exit.
 * Places come from gen/lab_areas.h (LAB_AREAS and the LM_* landmark boxes). */
#ifndef SP_MISSION_H
#define SP_MISSION_H
#include "ds.h"
#include "three.h"

#define MISSION_N 9

void mission_init(t3_scene *scene);            /* once: pickup and objective props */
/* set up level l: clears and spawns the army (enemy_spawn), places pickups
 * and objective props, unlocks the weapons the level starts with */
void mission_load(int level);
const char *mission_name(int level);           /* "LASER CORNER" */
const char *mission_blurb(int level);          /* one line for the level card */
v3 mission_start(int level, float *yaw);       /* the player's start (the buddy starts beside it) */

typedef struct {
  const char *objective;                       /* "CANCEL THE PRINTS 2/4", NULL = none */
  bool marker; v3 marker_pos;                  /* where the HUD's objective arrow points */
  int radio_beat;                              /* a walkie beat fired this frame, -1 none */
  int pickup;                                  /* a pickup taken this frame (weapon.h PK_*), -1 none */
  int weapon_found;                            /* a weapon unlocked this frame (WP_*), -1 none */
  bool checkpoint;                             /* a new checkpoint was reached this frame */
  bool boss_arena;                             /* the player has entered the boss arena (levels 5, 6) */
  bool done;                                   /* the level is complete */
  const char *banner;                          /* a one-off banner ("PRINT CANCELLED"), NULL none */
  const char *prompt;                          /* an interact prompt while the player is in reach of something ("PULL THE PLUG"), NULL none */
  const char *prompt2;                         /* ...the second player's (two-player mode) */
  int taker;                                   /* who walked through this frame's pickup: 0 the player, 1 the second */
} mission_out_t;

/* two-player mode: the second player's position for the next mission_update
 * (off: one player). Pickups, checkpoints, story triggers, objectives and the
 * level's end then go off for either of them */
void mission_set_second(bool on, v3 pos);
void mission_update(float dt, v3 player, mission_out_t *out);
v3 mission_checkpoint(void);                   /* where to respawn */
/* the player's shots: an objective at the hit point takes the damage (the
 * printers). Returns true if something of the mission's was hit. */
bool mission_hit(v3 at, float dmg);
/* an objective along a ray (so shots stop on it), like world_raycast */
bool mission_raycast(v3 o, v3 d, float max_dist, float *t);

/* the interact button (X / E): true if something in reach used it (then no iron swing) */
bool mission_interact(v3 player);
/* the cutscene (cutscene.h cut_id) before level l, and its boss reveal (-1 none) */
int mission_intro_cut(int level);
int mission_boss_cut(int level);
/* the level with the final boss: done also waits for the boss's fall */
bool mission_is_final(int level);
/* the weapons (bit per weapon.h WP_*) a level starts with, from the levels before it */
unsigned mission_weapons_at_start(int level);

/* the walkie script: the line for a beat, NULL when there is none */
const char *mission_radio(int level, int beat, const char **speaker);
int mission_radio_count(int level);

#endif
