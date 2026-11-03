/* hero.h: Moheeb and Luis as actors. The Coffee & Kill models and their
 * baked clips (anim.c), a weapon prop in the right hand, the tiny drone's
 * motors as a jetpack on the back, and an aim pose layered over the clip.
 * game.c drives one as the player and one as the buddy; cutscene.c drives
 * both, at toy size or full size. */
#ifndef SP_HERO_H
#define SP_HERO_H
#include "ds.h"
#include "three.h"

typedef enum { HERO_MOHEEB, HERO_LUIS, HERO_COUNT } hero_id;

/* a hero standing at toy size is HERO_H units tall; full size (the prologue
 * and the ending) is HERO_H * LAB_SCALE */
#define HERO_H 1.8f
#define HERO_R 0.38f

typedef struct toy toy_t;

toy_t *toy_new(hero_id who, t3_scene *scene);
const char *toy_name(hero_id who);                /* "MOHEEB" */

/* feet at pos, facing yaw (0 = +z), size 1 = toy, LAB_SCALE = full size */
void toy_place(toy_t *t, v3 pos, float yaw, float size);
void toy_visible(toy_t *t, bool on);
/* crossfade to a clip (assets/anim rig names: Idle_Loop, Jog_Fwd_Loop,
 * Jog_Bwd_Loop, Jog_Left_Loop, Jog_Right_Loop, Sprint_Loop, Walk_Fwd_Loop,
 * Jump_Start, Jump_Loop, Dodge_Left, Dodge_Right, Hit_Chest, Hit_Head,
 * Punch_Jab, Punch_Cross, Melee_Hook, Celebration, Surprise, KipUp, and the
 * Sword_ set). Replaying the current clip only changes its speed. */
void toy_play(toy_t *t, const char *clip, float speed, bool loop);
bool toy_clip_done(const toy_t *t);               /* a LoopOnce clip reached its end */
/* the aim layer: arms up holding the weapon toward pitch (radians, + up);
 * amount 0..1 blends it in over the clip */
void toy_aim(toy_t *t, float pitch, float amount);
void toy_set_weapon(toy_t *t, int weapon);        /* weapon.h WP_*, -1 none */
void toy_jet(toy_t *t, float thrust);             /* 0 off .. 1 full: motor spin and exhaust */
void toy_jetpack_visible(toy_t *t, bool on);      /* not worn in the prologue */
void toy_update(toy_t *t, float dt);
v3 toy_muzzle(const toy_t *t);                    /* the weapon's tip, world space (after toy_update) */
v3 toy_head(const toy_t *t);

#endif
