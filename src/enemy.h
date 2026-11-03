/* enemy.h: the army. Five hundred plastic army men (assets/models/soldier.glb,
 * Quaternius's CC0 soldier in moulded green or tan plastic), their squads,
 * shots and grenades, the Sergeant and the General.
 *
 * Every attack is something a plastic army man could do at nine centimetres:
 * pellet rifles, tiny grenades, a machine gun nest behind a thread spool,
 * paratroopers drifting down from a shelf on plastic parachutes. They fall
 * over when beaten (no gore) and melt a little under the soldering iron. */
#ifndef SP_ENEMY_H
#define SP_ENEMY_H
#include "ds.h"
#include "three.h"

typedef enum {
  EN_RIFLE,      /* green rifleman: patrols in step, shoots pellets */
  EN_GRENADIER,  /* green: lobs tiny grenades over cover */
  EN_GUNNER,     /* tan: a machine gun nest, stays put, sweeps */
  EN_PARA,       /* green: drops from height on a parachute, then fights as a rifleman */
  EN_VETERAN,    /* tan rifleman: tougher, flanks */
  EN_SERGEANT,   /* tan, twice the size, a megaphone: mid boss of level 5 */
  EN_GENERAL,    /* the twelve-inch display figure: the final boss */
  EN_TYPE_COUNT
} enemy_type;

void enemy_init(t3_scene *scene);
void enemy_clear(void);
void enemy_spawn(int type, v3 pos, float yaw);

/* who the army can see and shoot at: the player and the buddy */
typedef struct { v3 pos; float r, h; bool alive; } enemy_target_t;
void enemy_update(float dt, const enemy_target_t *player, const enemy_target_t *buddy);

/* the first enemy along a ray within max_dist: id, distance, weak point hit */
bool enemy_raycast(v3 origin, v3 dir, float max_dist, int *id, float *t, bool *weak);
/* as enemy_raycast, ignoring the ids in skip (the coilgun's pierce) */
bool enemy_raycast_skip(v3 o, v3 d, float max_dist, const int *skip, int nskip, int *id, float *t, bool *weak);
/* the player's aim ray this frame: veterans in it sidestep */
void enemy_notice_aim(v3 eye, v3 dir);
/* damage; kind is the weapon.h WP_* that did it (WP_IRON melts, WP_GLUE sticks).
 * Returns true if it went down. */
bool enemy_damage(int id, float amount, v3 at, bool weak, int kind);
void enemy_blast(v3 at, float radius, float amount);
/* enemies inside a sphere (melee, glue splash): fills ids, returns the count */
int enemy_in_sphere(v3 c, float r, int *ids, int max);
v3 enemy_pos(int id);
/* slow an enemy (glue): factor 0..1 for seconds */
void enemy_slow(int id, float factor, float seconds);

/* the damage the army did to a target this frame, and where it came from */
float enemy_hurt(int target /*0 player, 1 buddy*/, v3 *from);

int enemy_alive(void);
int enemy_alive_near(v3 c, float r);
int enemy_take_score(void);
/* the nearest living enemy the buddy should shoot at, -1 if none in range */
int enemy_nearest(v3 from, float range, bool need_sight);

/* bosses: the Sergeant (level 5) and the General (level 6) */
bool enemy_boss_active(void);
float enemy_boss_health(void);           /* 0..1 */
int enemy_boss_phase(void);              /* 1..3 */
bool enemy_boss_dead(void);
const char *enemy_boss_name(void);       /* "THE SERGEANT" / "THE GENERAL" */
void enemy_boss_wake(void);              /* dormant until its reveal cutscene ends */
/* a weak point near the aim cone, for the coilgun scope's bracket */
bool enemy_weak_point_near(v3 origin, v3 dir, float cos_cone, v3 *at);
/* the boss's spoken lines as they happen ("PULL MY STRING."), NULL when none this frame */
const char *enemy_take_shout(const char **speaker);

#endif
