/* weapon.h: what a toy-sized hero can make from a hackerspace, and the
 * player's shots in flight (enemy shots are enemy.c's).
 *
 *   WP_BANDS   rubber band repeater, laser-cut plywood: semi auto, endless
 *   WP_GLUE    hot glue gun: lobbed globs that stick, slow and burn plastic, leave puddles; glue sticks
 *   WP_COIL    coilgun off a capacitor bank: hold to charge, release; pierces; scoped; charge cells
 *   WP_IRON    soldering iron: melee, melts plastic, endless
 *   LiPo packs are thrown with their own button (they puff, then pop); not a slot. */
#ifndef SP_WEAPON_H
#define SP_WEAPON_H
#include "ds.h"
#include "three.h"

typedef enum { WP_BANDS, WP_GLUE, WP_COIL, WP_IRON, WP_COUNT } weapon_id;
typedef enum { PK_HEALTH, PK_GLUE, PK_CELLS, PK_LIPO, PK_BATTERY, PK_COUNT } pickup_id;

typedef struct {
  v3 eye, dir;          /* the aim ray: camera through the reticle */
  v3 muzzle;            /* where shots leave (the hero's weapon tip) */
  v3 body;              /* the hero's chest, for melee and throws */
  float yaw;            /* the hero's facing */
  bool fire, fire_held, fire_released;
  bool aim;             /* LT / right mouse held */
  bool throw_lipo, melee, next, prev;
  int select;           /* -1, or a weapon to switch to (number keys) */
} weapon_in_t;

typedef struct {
  float recoil;         /* camera kick this frame, 0..1 */
  bool hit;             /* something was hit this frame: the HUD's hit marker */
  bool hit_weak;        /* ...a weak point */
  bool killed;          /* ...and it went down */
  bool scope;           /* the coilgun's scope is up (game.c narrows the fov) */
  float charge;         /* coilgun charge 0..1 */
  bool swing;           /* a melee swing started (game.c plays the hero's swing clip) */
  bool threw;           /* a LiPo left the hand */
} weapon_out_t;

void weapon_init(t3_scene *scene);
void weapon_reset(int level);           /* ammo for the level's start; which weapons it has found */
void weapon_clear(void);                /* remove shots, puddles and LiPos in flight */
void weapon_unlock(int w);              /* found it (missions call this) */
bool weapon_unlocked(int w);
void weapon_update(float dt, const weapon_in_t *in, weapon_out_t *out);
int weapon_current(void);
int weapon_ammo(int w);                 /* -1 = endless */
int weapon_lipos(void);
const char *weapon_name(int w);         /* "RUBBER BAND REPEATER" */
void weapon_give(int pickup);           /* pickup_id (PK_HEALTH and PK_BATTERY are game.c's) */
/* glue puddles slow anything walking through them: 1 = full speed */
float weapon_glue_slow(v3 at);

#endif
