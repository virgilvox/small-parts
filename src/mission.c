/* mission.c: see mission.h. The nine levels are routes through the one lab
 * (docs/DESIGN.md), each built by a setup function out of a few kinds of
 * step: a trigger (the level loading, a zone on the floor plan, the level's
 * weapon picked up, the objective's count: printers, props, crates, the
 * charge; the boss's health) and what it does
 * (a squad marches in, a walkie beat, a checkpoint). Squads are streamed in
 * as the player gets near rather than all spawned at load, and wait their
 * turn while the army is at MAX_ALIVE.
 *
 * Every place comes from gen/lab_areas.h: a landmark box (LM_*) and an
 * offset from its centre, or an area box (LAB_AREAS). Heights come from
 * world_floor_y, searching down from a given height, so a squad lands on the
 * table top or the floor under it as asked. Build with -DMISSION_CHECK to log
 * every placement that finds no surface or no room to stand. */
#include "mission.h"
#include "cutscene.h"
#include "world.h"
#include "enemy.h"
#include "weapon.h"
#include "voice.h"
#include "audio.h"
#include "fx.h"
#include "cv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ALIVE 40
#define MAX_TRIG 32
#define MAX_ACT 64
#define MAX_PICK 16
#define NCRATE 5
#define TAKE_R 1.2f
#define PRINTER_HP 150.f

/* the levels, front of the building to back (docs/DESIGN.md) */
enum { LV_LASER, LV_STORAGE, LV_PRINTS, LV_WINDOWS, LV_COWORK, LV_WOODSHOP, LV_KITCHEN, LV_METALFAB, LV_MACHINE };

/* ── the steps ── */
enum { TR_LOAD, TR_ZONE, TR_WEAPON, TR_COUNT, TR_BELOW, TR_BOSS_HP, TR_BOSS_DEAD };
enum { AC_SQUAD, AC_BEAT, AC_CHECK };
typedef struct { int kind; float x0, z0, x1, z1; float val; bool fired; float t; } trig_t;
typedef struct { int trig, kind; int type, n; v3 at; float yaw, delay; int beat; bool done; } act_t;
/* a pickup: kind is a pickup_id, PICK_WEAPON + a weapon_id, or PICK_PROP (a spare drone prop) */
#define PICK_WEAPON 100
#define PICK_PROP 200
#define PICK_PACK 201   /* the charger's battery pack, the kitchen level's key item */
typedef struct { int kind; v3 at; bool taken; t3_object *prop; } pick_t;

static struct {
  int level;
  trig_t T[MAX_TRIG]; int nt;
  act_t A[MAX_ACT]; int na;
  pick_t P[MAX_PICK]; int np;
  int queue[16], nq;           /* walkie beats waiting their turn */
  float beat_gap;              /* seconds until the next beat may play */
  v3 start, check; float start_yaw;
  bool have_weapon; float weapon_t;
  int weapon;                  /* the weapon this level hides, -1 none */
  int count, last_count;       /* the objective's count: printers unplugged, props found, crates wrecked, charge stage */
  float charge, charged_t; bool charging;   /* the kitchen's charger */
  v3 pack;                     /* where the charger's pack lies in the basement */
  v3 crate[NCRATE]; float crate_hp[NCRATE]; int ncrate;   /* the metal fab bay's supply dump */
  v3 weapon_pos;
  bool arena;
  float t;
} M;

static t3_object *root;
static char objective_buf[64], banner_buf[48];

/* ── places ── */
static v3 lm_c(const lab_box_t *b) { return v3_((b->min[0] + b->max[0]) * 0.5f, b->max[1], (b->min[2] + b->max[2]) * 0.5f); }
/* a point dx, dz from a landmark's centre */
static v3 near(const lab_box_t *b, float dx, float dz) { v3 c = lm_c(b); return v3_(c.x + dx, 0, c.z + dz); }
static const lab_area_t *AR(int a) { return &LAB_AREAS[a]; }
/* the two aisles that run the length of the room: the front one between member
 * storage and coworking, the back one between the laser corner and the woodshop */
static float aisle_front(void) { return (AR(AREA_MEMBER_STORAGE_AND_JEWELRY)->max[2] + AR(AREA_COWORKING)->min[2]) * 0.5f; }
static float aisle_back(void) { return (AR(AREA_LASER_CORNER)->max[2] + AR(AREA_WOODSHOP)->min[2]) * 0.5f; }

/* the surface under (x, z) searching down from from_y; p.y is the height it lands on */
static v3 land(v3 p, float from_y) {
  float y = world_floor_y(v3_(p.x, from_y, p.z));
#ifdef MISSION_CHECK
  v3 c = v3_(p.x, y + 0.95f, p.z), n;
  float t; v3 nn;
  bool roof = world_raycast(v3_(p.x, y + 0.05f, p.z), v3_(0, 1, 0), 2.0f, &t, &nn);
  if (y < -1e8f || world_push_sphere(&c, 0.36f, &n) || roof)
    ds_log("[mission] L%d: no room at %.1f %.1f (from %.1f): floor %.2f roof %.2f", M.level + 1, p.x, p.z, from_y, y, roof ? t : -1.f);
#endif
  if (y < -1e8f) y = 0;
  return v3_(p.x, y, p.z);
}
static v3 floor_at(v3 p) { return land(p, 3); }
/* the top of a landmark: searched from a little above its box, since lids and planks stand proud of it */
static v3 top_of(const lab_box_t *b, float dx, float dz) { return land(near(b, dx, dz), b->max[1] + 2.5f); }

/* ── building the steps ── */
static int trig(int kind, float x0, float z0, float x1, float z1, float val) {
  trig_t *t = &M.T[M.nt];
  memset(t, 0, sizeof *t);
  t->kind = kind; t->x0 = x0; t->z0 = z0; t->x1 = x1; t->z1 = z1; t->val = val;
  return M.nt++;
}
static int on_load(void) { return trig(TR_LOAD, 0, 0, 0, 0, 0); }
static int zone(float x0, float z0, float x1, float z1) { return trig(TR_ZONE, x0, z0, x1, z1, 0); }
static int on_weapon(void) { return trig(TR_WEAPON, 0, 0, 0, 0, 0); }
static int on_count(int n) { return trig(TR_COUNT, 0, 0, 0, 0, (float)n); }
static int below(float y) { return trig(TR_BELOW, 0, 0, 0, 0, y); }   /* the player goes under y (the basement) */
static int on_boss_hp(float f) { return trig(TR_BOSS_HP, 0, 0, 0, 0, f); }
static int on_boss_dead(void) { return trig(TR_BOSS_DEAD, 0, 0, 0, 0, 0); }

static act_t *act(int t, int kind) {
  act_t *a = &M.A[M.na++];
  memset(a, 0, sizeof *a);
  a->trig = t; a->kind = kind; a->beat = -1;
  return a;
}
/* n army men around at (already landed), in ranks of three facing yaw */
static void squad(int t, int type, int n, v3 at, float yaw) {
  act_t *a = act(t, AC_SQUAD);
  a->type = type; a->n = n; a->at = at; a->yaw = yaw;
}
static void squad_late(int t, float delay, int type, int n, v3 at, float yaw) {
  squad(t, type, n, at, yaw); M.A[M.na - 1].delay = delay;
}
/* a walkie beat; one added straight after another on the same trigger plays after it */
static void beat(int t, int b, float delay) {
  act_t *prev = M.na > 0 ? &M.A[M.na - 1] : NULL;
  if (prev && prev->kind == AC_BEAT && prev->trig == t && delay < prev->delay) delay = prev->delay;
  act_t *a = act(t, AC_BEAT); a->beat = b; a->delay = delay;
}
static void checkpoint(int t, v3 at) { act_t *a = act(t, AC_CHECK); a->at = at; }
static void pickup(int kind, v3 at) {
  if (M.np >= MAX_PICK) return;
  pick_t *p = &M.P[M.np++];
  p->kind = kind; p->at = at; p->taken = false; p->prop = NULL;
}
static void weapon_at(int w, v3 at) { M.weapon = w; M.weapon_pos = at; pickup(PICK_WEAPON + w, at); }
static void prop_at(v3 at) { pickup(PICK_PROP, at); }
static void crate_at(v3 at) { if (M.ncrate < NCRATE) M.crate[M.ncrate++] = at; }

/* a squad stands in ranks of three across its facing, 1.4 apart; each man is
 * landed on whatever is under him at the squad's height */
static v3 member_pos(const act_t *a, int i) {
  float c = cosf(a->yaw), s = sinf(a->yaw);
  int row = i / 3, col = i % 3, in_row = a->n - row * 3 < 3 ? a->n - row * 3 : 3;
  float side = ((float)col - (in_row - 1) * 0.5f) * 1.4f, back = -(float)row * 1.5f;
  v3 p = v3_(a->at.x + c * side + s * back, a->at.y, a->at.z - s * side + c * back);
  if (a->type == EN_PARA) return p;
  float y = world_floor_y(v3_(p.x, a->at.y + 1.0f, p.z));
  p.y = y > -1e8f ? y : a->at.y;
  return p;
}
static void spawn_squad(const act_t *a) {
  for (int i = 0; i < a->n; i++) {
    v3 p = member_pos(a, i);
    if (a->type == EN_PARA) { p.x += ds_rr(-1.5f, 1.5f); p.z += ds_rr(-1.5f, 1.5f); p.y += ds_rr(0, 4); }
    enemy_spawn(a->type, p, a->yaw);
  }
}

#define LID (&LM_LASER_LID[0])
#define BIN (&LM_SCRAP_BIN[0])

/* where each level starts: the player's feet and facing (the buddy starts beside) */
static v3 start_of(int level, float *yaw) {
  float y = DS_PI / 2;
  v3 p;
  const lab_area_t *ms = AR(AREA_MACHINE_SHOP);
  switch (level) {
    default: p = land(v3_(lm_c(LID).x, 0, lm_c(LID).z), LID->max[1] + 2); y = -DS_PI / 2; break;   /* on the laser lid */
    case LV_STORAGE: p = floor_at(v3_(AR(AREA_MEMBER_STORAGE_AND_JEWELRY)->max[0] - 5, 0, aisle_front())); y = -DS_PI / 2; break;
    case LV_PRINTS: p = floor_at(near(&LM_SEWING_TABLE[0], -19, 19)); y = -DS_PI / 2; break;
    case LV_WINDOWS: p = floor_at(near(&LM_ROBOT_COUNTER[0], 26, -56)); y = -DS_PI / 2; break;   /* facing the front doors */
    case LV_COWORK: p = floor_at(near(&LM_COWORK_TABLE[0], -28, -26)); break;
    case LV_WOODSHOP: p = floor_at(near(&LM_ESD_MAT[0], 8, -14)); break;
    case LV_KITCHEN: p = floor_at(v3_(AR(AREA_WOODSHOP)->max[0] + 14, 0, aisle_back() + 6)); break;   /* the back hall */
    case LV_METALFAB: p = floor_at(near(&LM_WELD_TABLE[0], 0, -40)); y = 0; break;
    case LV_MACHINE: p = floor_at(v3_(ms->max[0] - 11, 0, LM_WELD_TABLE[0].min[2] - 14)); y = -DS_PI / 2; break;   /* in from the metal fab bay */
  }
#ifdef MISSION_TEST
  /* testing: start at the objective (the print rack's power strip, the kitchen table) */
  if (level == LV_PRINTS) { p = floor_at(v3_(lm_c(&LM_PRINTER_RACK[0]).x - 1.0f, 0, LM_PRINTER_RACK[0].max[2] + 4.2f)); y = DS_PI; }
  if (level == LV_KITCHEN) { v3 h = lm_c(&LM_HATCH[0]); p = floor_at(v3_(h.x, 0, LM_HATCH[0].min[2] - 7)); y = 0; }   /* by the hatch's gap */
#endif
  if (yaw) *yaw = y;
  return p;
}

/* ═════════════ the levels ═════════════ */

/* 1 LASER CORNER: down off the laser, the repeater from the scrap bin, two
 * patrols in step, out into the back aisle toward member storage */
static void level_laser(void) {
  weapon_at(WP_BANDS, floor_at(near(BIN, 0, 8)));   /* on the floor at the bin's open end */
  pickup(PK_BATTERY, floor_at(near(LID, 0, 22)));
  pickup(PK_HEALTH, land(near(LID, -35, 50), 25));   /* on the second laser's table */
  pickup(PK_LIPO, floor_at(near(LID, 30, 45)));
  float ab = aisle_back(), x0 = AR(AREA_LASER_CORNER)->min[0];
  int L = on_load(), W = on_weapon();
  beat(L, 0, 1.5f); beat(L, 1, 0);
  beat(W, 2, 0.3f);
  squad(W, EN_RIFLE, 4, floor_at(near(LID, 18, 26)), -DS_PI / 2);
  beat(W, 3, 5); beat(W, 4, 0);
  int mid = zone(x0, LID->max[2] + 2, x0 + 52, 60);
  squad(mid, EN_RIFLE, 4, floor_at(near(LID, -48, 28)), DS_PI / 2);
  squad(mid, EN_GUNNER, 1, land(near(LID, -25, 48), 25), DS_PI);
  int front = zone(x0 + 30, LID->max[2] + 4, x0 + 98, 58);
  checkpoint(front, floor_at(near(LID, 0, 24)));
  int aisle = zone(x0, 70, AR(AREA_LASER_CORNER)->max[0], ab + 18);
  squad(aisle, EN_RIFLE, 4, floor_at(v3_(x0 + 13, 0, ab)), DS_PI / 2);
  squad(aisle, EN_GRENADIER, 1, floor_at(v3_(x0 + 8, 0, ab + 6)), DS_PI / 2);
  squad(aisle, EN_PARA, 3, v3_(x0 + 38, 44, 80), DS_PI);
  beat(aisle, 5, 0.5f);
  int last = zone(x0, 80, x0 + 32, ab + 18);
  checkpoint(last, floor_at(near(LID, -20, ab - lm_c(LID).z)));
  squad(last, EN_RIFLE, 3, floor_at(near(LID, 30, ab - lm_c(LID).z)), -DS_PI / 2);
}

#define BOX(i) (&LM_MEMBER_BOX[i])
/* 2 MEMBER STORAGE: gunners up on the shelving, a squad in the aisle, the
 * glue gun in a member's bin, paratroopers off the shelves, then the sewing
 * corner and out past the sewing table */
static void level_storage(void) {
  const lab_area_t *ms = AR(AREA_MEMBER_STORAGE_AND_JEWELRY);
  float af = aisle_front();
  weapon_at(WP_GLUE, top_of(BOX(54), 0, 0));
  /* the shelving aisle, from the area's corner (the lockers and the lost and found moved; the glue bin moved with them) */
#define MSA(dx, dz) floor_at(v3_(ms->min[0] + (dx), 0, ms->min[2] + (dz)))
  pickup(PK_BATTERY, MSA(37, 17.5f));
  pickup(PK_HEALTH, MSA(106, 37.5f));
  pickup(PK_LIPO, MSA(82, 22.5f));
  pickup(PK_GLUE, floor_at(near(&LM_SEWING_TABLE[0], 0, 30)));
  pickup(PK_HEALTH, floor_at(near(&LM_SEWING_TABLE[0], 18, 55)));
  int L = on_load(), W = on_weapon();
  beat(L, 0, 1.5f);
  squad(L, EN_RIFLE, 4, floor_at(v3_(ms->max[0] - 39, 0, af)), DS_PI / 2);
  int in = zone(ms->min[0] + 20, ms->min[2], ms->max[0] - 14, ms->max[2] + 2);
  beat(in, 1, 0.5f);
  squad(in, EN_GUNNER, 1, land(near(BOX(9), 0, 0), 58), DS_PI / 2);
  squad(in, EN_GUNNER, 1, land(near(BOX(57), 0, 0), 58), DS_PI / 2);
  squad(in, EN_GUNNER, 1, land(near(BOX(39), 0, 0), 58), 0);
  squad(in, EN_RIFLE, 5, MSA(32, 37.5f), DS_PI / 2);
  int deep = zone(ms->min[0] + 20, ms->min[2], ms->min[0] + 66, ms->max[2] + 2);
  checkpoint(deep, MSA(42, 37.5f));
  v3 g = lm_c(BOX(54));
  int bin = zone(g.x - 14, g.z - 8, g.x + 14, g.z + 26);   /* in front of the bin, on the aisle side */
  beat(bin, 2, 0); beat(bin, 3, 0);
  squad(W, EN_PARA, 2, v3_(g.x - 20, 46, g.z + 14), 0);
  squad(W, EN_PARA, 2, v3_(g.x + 10, 46, g.z + 29), 0);
  const lab_box_t *st = &LM_SEWING_TABLE[0];
  int turn = zone(AR(AREA_SEWING)->min[0], ms->max[2], ms->min[0] + 12, af + 18);
  checkpoint(turn, floor_at(v3_(ms->min[0] - 4, 0, af)));
  squad(turn, EN_RIFLE, 4, floor_at(near(st, 0, 29)), 0);
  squad(turn, EN_GRENADIER, 2, floor_at(near(st, -6, 16)), 0);
  int sew = zone(AR(AREA_SEWING)->min[0] - 4, AR(AREA_SEWING)->min[2], AR(AREA_SEWING)->max[0] + 6, 66);
  beat(sew, 4, 0.5f); beat(sew, 5, 0);
  checkpoint(sew, floor_at(near(st, 10, 40)));
  squad(sew, EN_GUNNER, 1, top_of(st, -13, 4), 0);
  squad(sew, EN_VETERAN, 2, floor_at(near(st, -14, 20)), DS_PI / 2);
  int out = zone(AR(AREA_SEWING)->min[0] - 4, 0, AR(AREA_SEWING)->min[0] + 8, 50);
  beat(out, 6, 0.5f);
}
#undef MSA

#define PRN(i) (&LM_PRINTER[i])
#define LEGO(i) (&LM_LEGO_TOWER[i])
#define CRAFT(i) (&LM_CRAFT_TABLE[i])
/* 3 PRINT FARM: along the art and LEGO wall under the veterans, past the craft
 * tables and the welcome desk to the four printers; pull their plugs at the
 * power strip on the floor (or shoot them), then out to the front windows */
static void level_printfarm(void) {
  pickup(PK_LIPO, top_of(CRAFT(2), 0, 0));
  pickup(PK_LIPO, land(near(PRN(3), 5, 44), 16));    /* on the welcome table */
  pickup(PK_GLUE, floor_at(near(LEGO(0), -12, 18)));
  pickup(PK_HEALTH, floor_at(near(CRAFT(2), -26, -20)));
  pickup(PK_BATTERY, floor_at(near(PRN(3), 32, 30)));
  int L = on_load();
  beat(L, 0, 1.5f);
  squad(L, EN_RIFLE, 4, floor_at(near(LEGO(3), 0, 22)), DS_PI / 2);
  for (int i = 0; i < 4; i++) squad(L, i & 1 ? EN_GUNNER : EN_VETERAN, 1, top_of(LEGO(i), 0, 0), 0);
  float xl = lm_c(LEGO(3)).x;
  int lego = zone(xl - 80, 0, xl + 6, aisle_front());
  beat(lego, 1, 0.3f);
  checkpoint(lego, floor_at(near(LEGO(3), -5, 22)));
  squad(lego, EN_GUNNER, 1, top_of(CRAFT(0), 0, 0), DS_PI);
  squad(lego, EN_RIFLE, 4, floor_at(near(CRAFT(2), 0, 35)), DS_PI);
  squad(lego, EN_GRENADIER, 2, floor_at(near(CRAFT(2), -15, -22)), DS_PI / 2);
  float xp = PRN(1)->max[0];
  int welcome = zone(0, 0, LEGO(0)->min[0] - 10, aisle_front());
  squad(welcome, EN_RIFLE, 5, floor_at(near(PRN(3), 27, 18)), DS_PI / 2);
  squad(welcome, EN_GRENADIER, 1, floor_at(near(PRN(3), 40, 60)), DS_PI / 2);
  int farm = zone(0, 0, xp + 38, 45);
  beat(farm, 2, 0.3f); beat(farm, 3, 0); beat(farm, 4, 0);
  checkpoint(farm, floor_at(near(PRN(3), 35, 20)));
  squad(farm, EN_VETERAN, 2, floor_at(near(PRN(3), 5, 14)), 0);
  squad(farm, EN_RIFLE, 4, floor_at(near(PRN(2), -2, 16)), 0);
  for (int k = 1; k <= 3; k++) squad(on_count(k), EN_PARA, 2, v3_(lm_c(PRN(k - 1)).x, 46, PRN(0)->max[2] + 8), 0);
  beat(on_count(2), 5, 0.5f);
  int all = on_count(4);
  beat(all, 6, 1.0f); beat(all, 7, 0);
  checkpoint(all, floor_at(near(PRN(3), 35, 20)));
  squad_late(all, 2, EN_RIFLE, 5, floor_at(v3_(AR(AREA_WELCOME)->min[0] + 4, 0, 84)), DS_PI);
}

#define LEDC (&LM_LED_CUBE[0])
#define COUNTER (&LM_ROBOT_COUNTER[0])
#define DECK (&LM_HACKER_DECK[0])
/* 4 WINDOW DISPLAYS: the storefront. Four spare drone props for the jetpacks,
 * one on the gallery tiers, one by the LED cube on the robot counter, one in
 * the hacker bay behind the rubber duck battalion, one by the counter drone */
static void level_windows(void) {
  prop_at(top_of(&LM_GALLERY_TIER[1], -20, 0));
  prop_at(land(near(LEDC, 7, 1), LEDC->min[1] + 1));
  prop_at(top_of(DECK, 15, 10));
  prop_at(land(near(&LM_COUNTER_DRONE[1], -3, -1), LM_COUNTER_DRONE[1].max[1] + 1));
  pickup(PK_HEALTH, floor_at(near(COUNTER, -14, -30)));
  pickup(PK_BATTERY, floor_at(near(COUNTER, 22, 0)));
  pickup(PK_GLUE, floor_at(near(COUNTER, 22, 36)));
  pickup(PK_LIPO, floor_at(near(&LM_GALLERY_PEDESTAL[1], 0, 9)));
  int L = on_load();
  beat(L, 0, 1.5f);
  squad(L, EN_RIFLE, 4, floor_at(near(COUNTER, -14, -42)), DS_PI / 2);
  squad(L, EN_GUNNER, 1, top_of(&LM_GALLERY_TIER[0], -30, 0), DS_PI / 2);
  squad(L, EN_GUNNER, 1, top_of(&LM_GALLERY_TIER[0], 25, 0), DS_PI / 2);
  int gallery = zone(0, 0, AR(AREA_WINDOW_DISPLAY_GALLERY_BAY)->max[0], 42);
  beat(gallery, 6, 0.3f);
  int counter = zone(COUNTER->min[0] - 6, COUNTER->min[2] - 8, COUNTER->max[0] + 16, COUNTER->max[2]);
  beat(counter, 1, 0.3f);
  checkpoint(counter, floor_at(near(COUNTER, 22, -10)));
  squad(counter, EN_GUNNER, 1, top_of(COUNTER, 5, 10), DS_PI / 2);
  squad(counter, EN_RIFLE, 4, floor_at(near(COUNTER, 22, 30)), DS_PI);
  int bay = zone(0, DECK->min[2] - 4, COUNTER->max[0] + 16, 190);
  beat(bay, 2, 0.3f); beat(bay, 3, 0);
  checkpoint(bay, floor_at(near(COUNTER, 20, 40)));
  squad(bay, EN_RIFLE, 6, top_of(DECK, 10, 4), DS_PI / 2);
  squad(bay, EN_GRENADIER, 2, top_of(DECK, -12, 8), DS_PI / 2);
  int two = on_count(2), three = on_count(3), four = on_count(4);
  beat(two, 4, 0.8f);
  squad(two, EN_PARA, 3, v3_(lm_c(&LM_GALLERY_TIER[0]).x, 46, 30), DS_PI / 2);
  squad(three, EN_VETERAN, 3, floor_at(near(COUNTER, 24, -12)), -DS_PI / 2);
  beat(four, 5, 0.8f);
  checkpoint(four, floor_at(near(COUNTER, 24, -14)));
  squad_late(four, 1.5f, EN_RIFLE, 4, floor_at(near(&LM_COWORK_TABLE[0], -12, -14)), -DS_PI / 2);
}

#define CT(i) (&LM_COWORK_TABLE[i])
#define ESD (&LM_ESD_MAT[0])
/* 5 COWORKING: under the desks, across the chairs, the couch fortress, the TV
 * wall and the bike bench, to the coilgun on the electronics bench */
static void level_coworking(void) {
  weapon_at(WP_COIL, top_of(ESD, 8, 0));
  pickup(PK_HEALTH, floor_at(near(CT(1), 0, 18)));
  pickup(PK_BATTERY, floor_at(near(CT(0), 0, -2)));
  pickup(PK_GLUE, top_of(&LM_COFFEE_TABLE[0], 3, 3));
  pickup(PK_LIPO, floor_at(near(CT(2), 20, -20)));
  pickup(PK_HEALTH, floor_at(near(ESD, -20, -15)));
  int L = on_load(), W = on_weapon();
  beat(L, 0, 1.5f);
  squad(L, EN_RIFLE, 4, floor_at(near(CT(0), 6, 0)), -DS_PI / 2);
  float x0 = lm_c(CT(0)).x;
  int t1 = zone(x0 - 10, 100, 400, 190);
  beat(t1, 1, 1.0f);
  squad(t1, EN_RIFLE, 4, floor_at(near(CT(1), 0, 0)), -DS_PI / 2);
  squad(t1, EN_GRENADIER, 1, top_of(CT(1), -6, -6), -DS_PI / 2);
  squad(t1, EN_GRENADIER, 1, top_of(CT(1), 6, 6), -DS_PI / 2);
  int t2 = zone(lm_c(CT(1)).x - 10, 100, 400, 190);
  checkpoint(t2, floor_at(near(CT(1), 0, -26)));
  squad(t2, EN_VETERAN, 2, floor_at(near(CT(2), -10, 16)), -DS_PI / 2);
  squad(t2, EN_RIFLE, 3, floor_at(near(CT(2), 0, -26)), -DS_PI / 2);
  int lounge = zone(CT(2)->min[0], 100, 400, 190);
  beat(lounge, 2, 0.5f); beat(lounge, 3, 0);
  for (int i = 0; i < 3; i++) squad(lounge, EN_GUNNER, 1, top_of(&LM_SOFA_SEAT[i], 0, 0), -DS_PI / 2);
  squad(lounge, EN_RIFLE, 4, floor_at(near(&LM_COFFEE_TABLE[0], 10, 0)), -DS_PI / 2);
  const lab_area_t *lo = AR(AREA_LOUNGE);
  int tv = zone(lo->min[0] + 4, 100, 400, 190);
  beat(tv, 4, 0.5f);
  checkpoint(tv, floor_at(v3_(lo->min[0] + 2, 0, aisle_front() + 8)));
  squad(tv, EN_PARA, 3, v3_(lo->min[0] + 12, 46, lo->max[2] + 6), -DS_PI / 2);
  const lab_area_t *bk = AR(AREA_BIKE_STATION);
  int bike = zone(bk->min[0] - 10, 0, 420, 190);
  beat(bike, 5, 0.5f);
  checkpoint(bike, floor_at(v3_(bk->min[0], 0, aisle_front() + 8)));
  squad(bike, EN_RIFLE, 4, floor_at(v3_(bk->max[0] - 10, 0, aisle_front() + 4)), -DS_PI / 2);
  squad(bike, EN_VETERAN, 2, floor_at(v3_(bk->max[0] + 10, 0, bk->min[2] + 2)), -DS_PI / 2);
  int el = zone(bk->max[0] - 10, 0, 430, 190);
  squad(el, EN_RIFLE, 3, floor_at(near(ESD, 0, -18)), DS_PI);
  squad(el, EN_GUNNER, 1, top_of(ESD, -12, 0), -DS_PI / 2);
  beat(W, 6, 0.3f);
}

#define SOLDER (&LM_SOLDER_STATION[0])
#define WOOD (&LM_WOOD_BENCH[0])
#define BACKB (&LM_BACK_BENCH[0])
/* 6 WOODSHOP: the parts drawers and the soldering station, down the aisle and
 * into the woodshop, where the Sergeant holds the eight foot bench */
static void level_woodshop(void) {
  weapon_at(WP_IRON, top_of(SOLDER, 0, 0));
  pickup(PK_CELLS, floor_at(near(ESD, 30, -10)));
  pickup(PK_HEALTH, floor_at(v3_(AR(AREA_ELECTRONICS_AND_WOODSHOP_DIVIDE)->max[0] - 10, 0, aisle_back())));
  pickup(PK_BATTERY, floor_at(near(WOOD, -40, -30)));
  pickup(PK_LIPO, floor_at(near(WOOD, -10, -35)));
  pickup(PK_CELLS, floor_at(near(BACKB, -20, 0)));
  pickup(PK_GLUE, floor_at(near(WOOD, 15, -30)));
  int L = on_load(), W = on_weapon();
  squad(L, EN_SERGEANT, 1, top_of(WOOD, 0, -8), -DS_PI / 2);
  beat(L, 0, 1.5f); beat(L, 1, 0);
  squad(L, EN_RIFLE, 4, floor_at(near(ESD, 25, -32)), -DS_PI / 2);
  squad(L, EN_GUNNER, 1, top_of(&LM_MCU_CABINET[0], 0, 0), DS_PI / 2);
  v3 so = lm_c(SOLDER);
  int iron = zone(so.x - 18, so.z - 22, so.x + 14, so.z + 6);
  beat(iron, 2, 0); beat(iron, 3, 0);
  checkpoint(W, floor_at(near(ESD, 20, -12)));
  squad(W, EN_VETERAN, 2, floor_at(near(SOLDER, 23, -66)), DS_PI);
  squad(W, EN_RIFLE, 3, floor_at(near(SOLDER, 30, -75)), DS_PI);
  const lab_area_t *dv = AR(AREA_ELECTRONICS_AND_WOODSHOP_DIVIDE);
  int aisle = zone(dv->min[0] - 20, 60, dv->max[0] + 3, dv->min[2] + 4);
  beat(aisle, 4, 0.5f);
  checkpoint(aisle, floor_at(v3_(dv->min[0], 0, aisle_back() + 4)));
  squad(aisle, EN_RIFLE, 5, floor_at(v3_(dv->max[0] - 4, 0, aisle_back())), -DS_PI / 2);
  squad(aisle, EN_GRENADIER, 2, floor_at(v3_(dv->max[0] + 10, 0, aisle_back() - 4)), -DS_PI / 2);
  const lab_area_t *ws = AR(AREA_WOODSHOP);
  int arena = zone(ws->min[0] + 1, ws->min[2] - 2, ws->max[0], ws->max[2]);
  checkpoint(arena, floor_at(v3_(ws->min[0] + 6, 0, aisle_back() + 10)));
  squad(arena, EN_RIFLE, 4, land(v3_(ws->min[0] + 36, 0, lm_c(WOOD).z), 22), -DS_PI / 2);
  squad(arena, EN_GUNNER, 1, top_of(BACKB, 0, -15), -DS_PI / 2);
  squad(arena, EN_GUNNER, 1, top_of(BACKB, 0, 18), -DS_PI / 2);
  int dead = on_boss_dead();
  beat(dead, 5, 2.0f); beat(dead, 6, 0);
}

#define KTABLE (&LM_KITCHEN_TABLE[0])
#define HATCH (&LM_HATCH[0])
#define BASEMENT (&LM_BASEMENT[0])
/* the gap the shoved hatch plate leaves: the hole's near end, by the closet wall's corner */
static v3 hatch_gap(void) { return v3_(lm_c(HATCH).x, 0, HATCH->min[2] + 3.5f); }
/* the basement's dirt under p (searched from just under the slab) */
static v3 dirt(v3 p) { return land(p, BASEMENT->max[1] - 1); }
/* 7 KITCHEN AND DARKROOM: the back hall and the biolab, where the hatch by the
 * supply closet doors is open and the charger's battery pack has gone down it;
 * drop into the basement for it, jet back out up the dirt pile, then the
 * kitchen, where the charger on the table needs sixty seconds and the army
 * wants the room */
static void level_kitchen(void) {
  const lab_area_t *kt = AR(AREA_KITCHEN_AND_DARKROOM), *bl = AR(AREA_BIOLAB);
  float hall = aisle_back() + 6;   /* the hall's middle, clear of the biolab's wall */
  v3 h = lm_c(HATCH);
  M.pack = dirt(v3_(BASEMENT->min[0] + 30, 0, BASEMENT->min[2] + 88));   /* on the second mound, away from the hatch */
  pickup(PICK_PACK, M.pack);
  pickup(PK_HEALTH, floor_at(v3_(AR(AREA_SUPPLY_CLOSET)->min[0] + 6, 0, hall)));
  pickup(PK_BATTERY, dirt(v3_(h.x + 8, 0, h.z + 22)));   /* in the basement: a full battery for the way out */
  pickup(PK_CELLS, top_of(&LM_DARK_COUNTER[0], 10, 0));
  pickup(PK_LIPO, floor_at(near(KTABLE, -14, 18)));
  pickup(PK_HEALTH, floor_at(near(KTABLE, 14, -16)));
  pickup(PK_GLUE, floor_at(near(&LM_DARK_SINK[0], 12, 0)));
  int L = on_load(), P = on_weapon();
  beat(L, 0, 1.5f);
  squad(L, EN_RIFLE, 3, floor_at(v3_(AR(AREA_SUPPLY_CLOSET)->min[0], 0, hall)), -DS_PI / 2);
  int closet = zone(AR(AREA_SUPPLY_CLOSET)->min[0] - 20, 60, bl->min[0], 120);
  beat(closet, 1, 0.3f);
  int bio = zone(bl->min[0] - 6, 60, kt->min[0], 120);
  checkpoint(bio, floor_at(v3_(bl->min[0] - 8, 0, hall)));
  squad(bio, EN_RIFLE, 4, floor_at(v3_(bl->max[0] - 6, 0, hall)), -DS_PI / 2);
  int hz = zone(bl->min[0], bl->min[2], bl->max[0], h.z + 40);
  beat(hz, 2, 0.3f); beat(hz, 3, 0);
  checkpoint(hz, floor_at(v3_(h.x + 14, 0, h.z + 30)));
  squad(hz, EN_RIFLE, 3, floor_at(v3_(h.x + 22, 0, h.z + 14)), -DS_PI / 2);
  /* the basement: the army's hideout */
  int down = below(BASEMENT->max[1] - 2);
  beat(down, 4, 0.5f);
  checkpoint(down, dirt(v3_(h.x, 0, h.z + 4)));
  squad(down, EN_RIFLE, 4, dirt(v3_(h.x + 36, 0, h.z + 46)), DS_PI);
  squad(down, EN_GUNNER, 1, dirt(v3_(M.pack.x + 10, 0, M.pack.z + 8)), DS_PI / 2);
  squad(down, EN_GUNNER, 1, dirt(v3_(M.pack.x - 6, 0, M.pack.z - 14)), DS_PI / 2);
  squad(down, EN_VETERAN, 3, dirt(v3_(h.x + 50, 0, h.z + 80)), DS_PI);
  beat(P, 5, 0.3f);
  squad(P, EN_RIFLE, 3, dirt(v3_(h.x - 30, 0, h.z + 50)), DS_PI / 2);
  int kitchen = zone(kt->min[0] - 2, kt->min[2], kt->max[0], kt->max[2]);
  beat(kitchen, 6, 0.3f);
  checkpoint(kitchen, floor_at(near(KTABLE, -20, 6)));
  squad(kitchen, EN_GUNNER, 1, top_of(&LM_SUPPLY_SHELF[3], 0, 0), 0);
  /* the charge: started at the table, waves at each third */
  int c1 = on_count(1), c2 = on_count(2), c3 = on_count(3), c4 = on_count(4);
  beat(c1, 7, 0.5f);
  squad(c1, EN_RIFLE, 4, floor_at(near(KTABLE, -27, 20)), DS_PI);
  squad(c1, EN_GRENADIER, 2, floor_at(near(&LM_DARK_COUNTER[0], 0, 12)), 0);
  squad(c2, EN_PARA, 3, v3_(lm_c(KTABLE).x, 48, lm_c(KTABLE).z), 0);
  squad(c2, EN_VETERAN, 3, floor_at(near(&LM_DARK_SINK[0], 8, 30)), DS_PI / 2);
  beat(c3, 8, 0.5f);
  squad(c3, EN_RIFLE, 4, floor_at(near(KTABLE, 18, 26)), DS_PI);
  squad(c3, EN_GUNNER, 2, top_of(&LM_DARK_COUNTER[0], -12, 0), 0);
  beat(c4, 9, 0.3f);
}

#define WELD (&LM_WELD_TABLE[0])
#define ANVIL (&LM_ANVIL[0])
/* 8 METAL FAB BAY: the army's supply dump, five crate stacks among the welders
 * and the anvil; wreck them, then the connecting door into the machine shop */
static void level_metalfab(void) {
  const lab_area_t *mf = AR(AREA_METAL_FAB_BAY);
  crate_at(floor_at(near(WELD, -6, -16)));
  crate_at(floor_at(near(WELD, 30, -14)));
  crate_at(floor_at(near(ANVIL, 14, -2)));
  crate_at(floor_at(near(ANVIL, 12, 14)));
  crate_at(floor_at(near(WELD, -20, 14)));
  pickup(PK_LIPO, floor_at(near(WELD, 0, -26)));
  pickup(PK_LIPO, floor_at(near(ANVIL, -10, -12)));
  pickup(PK_HEALTH, floor_at(near(WELD, 18, -20)));
  pickup(PK_BATTERY, top_of(WELD, 0, 0));
  pickup(PK_CELLS, floor_at(near(ANVIL, 18, -16)));
  int L = on_load();
  beat(L, 0, 1.5f);
  squad(L, EN_RIFLE, 4, floor_at(near(WELD, 0, -18)), DS_PI);
  int bay = zone(mf->min[0], mf->min[2] + 4, mf->max[0], mf->max[2]);
  beat(bay, 1, 0.3f); beat(bay, 2, 0);
  checkpoint(bay, floor_at(near(WELD, 6, -24)));
  squad(bay, EN_GUNNER, 1, top_of(WELD, 4, -4), DS_PI);
  squad(bay, EN_RIFLE, 4, floor_at(near(WELD, 32, -10)), DS_PI);
  squad(bay, EN_GRENADIER, 2, floor_at(near(ANVIL, 6, 8)), -DS_PI / 2);
  int anvil = zone(ANVIL->min[0] - 10, ANVIL->min[2] - 10, ANVIL->max[0] + 10, ANVIL->max[2] + 10);
  beat(anvil, 6, 0.3f);
  int two = on_count(2), four = on_count(4), five = on_count(5);
  beat(two, 3, 0.8f);
  squad(two, EN_PARA, 3, v3_(lm_c(WELD).x + 10, 50, lm_c(WELD).z), DS_PI);
  squad(on_count(3), EN_VETERAN, 4, floor_at(v3_(mf->min[0] + 6, 0, lm_c(WELD).z - 18)), DS_PI / 2);
  beat(four, 4, 0.8f);
  beat(five, 5, 1.0f);
  checkpoint(five, floor_at(near(WELD, -14, -20)));
  squad_late(five, 1.5f, EN_RIFLE, 4, floor_at(v3_(mf->min[0] - 12, 0, lm_c(WELD).z - 22)), DS_PI / 2);
}

#define LATHE (&LM_LATHE_BED[0])
#define PANEL (&LM_PANEL[0])
/* 9 MACHINE SHOP: in through the metal fab bay's door; the General in front
 * of the lathe, then the breaker panel */
static void level_machineshop(void) {
  const lab_area_t *ms = AR(AREA_MACHINE_SHOP);
  pickup(PK_HEALTH, floor_at(near(LATHE, -30, -50)));
  pickup(PK_BATTERY, floor_at(near(LATHE, 20, -60)));
  pickup(PK_CELLS, floor_at(near(LATHE, -14, -66)));
  pickup(PK_LIPO, floor_at(near(LATHE, 42, -30)));
  pickup(PK_HEALTH, floor_at(near(LATHE, 50, -55)));
  pickup(PK_BATTERY, floor_at(near(LATHE, -12, -10)));
  v3 gp = floor_at(near(LATHE, -13.5f, -21));   /* the clear floor in front of the lathe */
  int L = on_load();
  squad(L, EN_GENERAL, 1, gp, atan2f(M.start.x - gp.x, M.start.z - gp.z));
  beat(L, 0, 1.5f); beat(L, 1, 0);
  squad(L, EN_RIFLE, 4, floor_at(near(LATHE, -20, -45)), -DS_PI / 2);
  squad(L, EN_GUNNER, 1, land(v3_(ms->min[0] + 7, 0, lm_c(LATHE).z - 34), 22), DS_PI / 2);
  squad(L, EN_GUNNER, 1, land(v3_(ms->min[0] + 7, 0, lm_c(LATHE).z - 14), 22), DS_PI / 2);
  squad(L, EN_RIFLE, 3, floor_at(near(LATHE, 45, -30)), -DS_PI / 2);
  int arena = zone(ms->min[0], ms->min[2], ms->max[0] - 22, ms->max[2]);
  checkpoint(arena, M.start);
  squad(arena, EN_VETERAN, 2, floor_at(near(LATHE, 30, -40)), -DS_PI / 2);
  int h1 = on_boss_hp(0.66f), h2 = on_boss_hp(0.33f);
  beat(h1, 2, 0);
  squad(h1, EN_RIFLE, 4, floor_at(near(LATHE, 30, -50)), -DS_PI / 2);
  squad(h1, EN_PARA, 2, v3_(lm_c(LATHE).x - 20, 50, lm_c(LATHE).z - 40), 0);
  beat(h2, 3, 0);
  squad(h2, EN_VETERAN, 2, floor_at(near(LATHE, -30, -55)), 0);
  squad(h2, EN_GRENADIER, 2, floor_at(near(LATHE, 15, -58)), -DS_PI / 2);
  int dead = on_boss_dead();
  beat(dead, 4, 2.5f); beat(dead, 5, 0);
  checkpoint(dead, floor_at(near(LATHE, 0, -14)));
}

/* ═════════════ the walkie script ═════════════ */
typedef struct { const char *who, *text; } line_t;
static const char *const MO = "MOHEEB", *const LU = "LUIS";
#define MAX_BEATS 10
static const line_t RADIO[MISSION_N][MAX_BEATS] = {
  { {MO, "We're on the laser lid. Mind the honeycomb."},
    {LU, "Hold jump in the air. Those drone motors on your back still lift."},
    {MO, "A rubber band repeater from the scrap bin. Laser cut, slightly charred. Perfect."},
    {LU, "They're in formation."},
    {MO, "They came out of the printer in formation."},
    {LU, "Member storage is down the aisle. Stay low and keep moving."} },
  { {LU, "Member storage. Fifty eight banker boxes, every one labelled."},
    {MO, "Gunners on the top shelf. They've run a supply line through our stuff."},
    {LU, "We are not opening anyone's bin."},
    {MO, "We are opening this one. It says GLUE."},
    {LU, "Sewing corner. The thread spools make good cover."},
    {MO, "Pins in that cushion. Don't land on the cushion."},
    {LU, "Art and LEGO wall next. They've put lookouts up there."} },
  { {LU, "Print farm is at the front. All four printers are still running."},
    {MO, "Tan ones on the LEGO towers. Those are the veterans."},
    {MO, "The screens say ARMY MEN, five hundred. Sixty one percent."},
    {LU, "It's still printing them."},
    {MO, "Then we stop the print. Power strip at the foot of the rack. Get close and pull the plugs."},
    {LU, "Two unplugged. Nobody tell the kids' class."},
    {MO, "Job queue's empty."},
    {LU, "The army isn't. Out to the front windows."} },
  { {MO, "Four spare props in the window displays. Gallery, counter, and the hacker bay."},
    {LU, "The robot counter is staffed. Mind the LED cube, it's somebody's thesis."},
    {LU, "Rubber ducks. Twenty three of them. In formation."},
    {MO, "They've drafted the ducks."},
    {LU, "Two props. These are better than the ones we had."},
    {MO, "Four props. Past the counter and back to coworking."},
    {MO, "Gallery bay. Nobody touch the art. Except the prop on it."} },
  { {MO, "Coworking. We go under the desks. It's shorter."},
    {LU, "Cables everywhere. It's a jungle under here."},
    {LU, "They have the high ground."},
    {MO, "It's a couch."},
    {MO, "TV wall. Somebody left a movie paused for nine hours."},
    {LU, "Bike bench, then electronics. The coilgun is on the far bench."},
    {MO, "Capacitor bank, copper coil. I built this for a demo and it still works."} },
  { {LU, "Parts drawers. Resistors. Ten thousand of them. Sorted."},
    {MO, "Someone labelled every drawer. I love this place."},
    {MO, "The soldering iron is still warm."},
    {LU, "Hot end first."},
    {LU, "Woodshop's ahead. Sawdust, and somebody with a megaphone."},
    {MO, "Sergeant's down. The machine shop door is blocked. They stacked the scrap bin against it."},
    {LU, "Then we go round. Back hall, kitchen, metal fab, in the side door."} },
  { {LU, "Back hall. Supply closet, biolab, then the kitchen."},
    {MO, "Supply closet. Enough paper towels for a thousand years."},
    {LU, "The hatch by the closet doors is open. Somebody shoved the plate aside."},
    {MO, "And the charger's battery pack is gone. Little boot prints, straight down the hatch."},
    {LU, "The basement. Dirt, drain pipes and a lot of army. No stairs."},
    {MO, "Got the pack. Up the dirt pile, then jet out through the hatch."},
    {MO, "Kitchen. The charger is on the table. Plug the pack in, then hold the room."},
    {LU, "Charging. Sixty seconds. Stay by the table, they heard the beep."},
    {MO, "Somebody labelled this yogurt in twenty nineteen."},
    {LU, "Full charge. That's enough battery for the breaker panel."} },
  { {MO, "Metal fab bay. That cardboard is not ours."},
    {LU, "Supply dump. Five crates of spare army."},
    {MO, "Shoot them, or throw a LiPo. Same result, louder."},
    {LU, "That one was all left boots."},
    {MO, "One more crate. Then the connecting door to the machine shop."},
    {LU, "Dump's gone. The door is on the far wall, past the welders."},
    {MO, "That's an anvil. Do not get under the anvil."} },
  { {LU, "Machine shop. The breaker panel is on the far wall, above the lathe."},
    {MO, "Flip the main, the laser resets, we walk home tall."},
    {LU, "Go for the pull string box on his back. Or the knees. The knees are hollow."},
    {MO, "He's wobbling. Stay on the knees."},
    {LU, "He's down. Panel's up there. The charge should get us there."},
    {MO, "Race you to the main."} },
};

const char *mission_radio(int level, int b, const char **speaker) {
  if (speaker) *speaker = NULL;
  if ((unsigned)level >= MISSION_N || (unsigned)b >= MAX_BEATS || !RADIO[level][b].text) return NULL;
  if (speaker) *speaker = RADIO[level][b].who;
  return RADIO[level][b].text;
}
int mission_radio_count(int level) {
  if ((unsigned)level >= MISSION_N) return 0;
  int n = 0;
  while (n < MAX_BEATS && RADIO[level][n].text) n++;
  return n;
}

static const char *const NAMES[MISSION_N] = {"LASER CORNER", "MEMBER STORAGE", "PRINT FARM", "WINDOW DISPLAYS", "COWORKING",
                                             "WOODSHOP", "KITCHEN AND DARKROOM", "METAL FAB BAY", "MACHINE SHOP"};
static const char *const BLURBS[MISSION_N] = {
  "The SCALE button worked. Get off the laser and find something to shoot with.",
  "Fifty eight labelled bins and an army supply line running through them.",
  "Four printers, one job in the queue: ARMY_MEN_x500. Pull the plugs.",
  "The storefront. Robots, CRTs, a duck battalion, and four spare drone props.",
  "Under the desks, over the chairs. The army holds the couch.",
  "Parts drawers, a warm soldering iron, and a Sergeant with a megaphone.",
  "Sixty seconds on the charger. The army wants the kitchen, and the yogurt.",
  "The army's supply dump, five crates among the welders. Wreck it.",
  "The breaker panel is on the far wall. Something large is standing in front of it.",
};
const char *mission_name(int l) { return (unsigned)l < MISSION_N ? NAMES[l] : ""; }
const char *mission_blurb(int l) { return (unsigned)l < MISSION_N ? BLURBS[l] : ""; }

/* ═════════════ props ═════════════ */
static t3_material *mat(uint32_t c, float rough, float emis) {
  t3_material *m = t3_mesh_standard_material_new(c);
  m->roughness = rough; m->metalness = 0;
  if (emis > 0) { m->emissive = t3_color_hex(c); m->emissive_intensity = emis; }
  return m;
}
static t3_mesh *add(t3_object *par, t3_geometry *g, uint32_t c, float rough, float emis, float x, float y, float z) {
  t3_mesh *me = t3_mesh_new(g, mat(c, rough, emis));
  t3_release(g);
  T3_OBJ(me)->frustum_culled = false;
  t3_object_set_position(me, x, y, z);
  t3_object_add(par, me); t3_release(me);
  return me;
}
static t3_mesh *bx(t3_object *p, float w, float h, float d, float x, float y, float z, uint32_t c) { return add(p, t3_box_geometry_new(w, h, d, 1, 1, 1), c, 0.7f, 0, x, y, z); }
static t3_mesh *cy(t3_object *p, float r, float h, float x, float y, float z, uint32_t c, int seg) { return add(p, t3_cylinder_geometry_new(r, r, h, seg, 1, false), c, 0.6f, 0, x, y, z); }
static t3_mesh *cyz(t3_object *p, float r, float len, float x, float y, float z, uint32_t c) {   /* along z */
  t3_mesh *m = cy(p, r, len, x, y, z, c, 8);
  t3_object_set_rotation(m, DS_PI / 2, 0, 0);
  return m;
}

/* the pickups, a little larger than life so they read at 480x270; the origin is the bottom */
static t3_object *build_pickup(int kind) {
  t3_object *o = t3_object_new();
  switch (kind) {
    case PK_HEALTH: {   /* a roll of electrical tape on its edge, a cardboard core */
      add(o, t3_torus_geometry_new(0.34f, 0.17f, 6, 12, 2 * DS_PI), 0x22252b, 0.5f, 0, 0, 0.52f, 0);
      t3_mesh *c = add(o, t3_cylinder_geometry_new(0.2f, 0.2f, 0.3f, 10, 1, true), 0xc8a878, 0.9f, 0, 0, 0.52f, 0);
      t3_object_set_rotation(c, DS_PI / 2, 0, 0);
      break; }
    case PK_BATTERY: {  /* a cell standing up: teal wrap, a copper cap, the nub */
      cy(o, 0.22f, 0.9f, 0, 0.45f, 0, C_TEAL, 10);
      cy(o, 0.225f, 0.18f, 0, 0.84f, 0, 0xb8642c, 10);
      cy(o, 0.08f, 0.08f, 0, 0.97f, 0, 0xd0d4da, 8);
      bx(o, 0.04f, 0.3f, 0.02f, 0, 0.45f, 0.225f, 0xf2ead8);
      break; }
    case PK_GLUE: {     /* a bundle of three glue sticks and a band */
      for (int i = 0; i < 3; i++) add(o, t3_cylinder_geometry_new(0.13f, 0.13f, 0.95f, 8, 1, false), C_GOO, 0.3f, 0.15f, (i - 1) * 0.24f, 0.5f, i == 1 ? 0.15f : 0);
      bx(o, 0.74f, 0.1f, 0.42f, 0, 0.5f, 0.06f, C_HIVIS);
      break; }
    case PK_CELLS: {    /* three electrolytic capacitors on their legs */
      for (int i = 0; i < 3; i++) {
        float x = (i - 1) * 0.36f;
        cy(o, 0.16f, 0.5f, x, 0.45f, 0, 0x2a3a8a, 10);
        cy(o, 0.162f, 0.04f, x, 0.71f, 0, 0xc0c6cc, 10);
        bx(o, 0.03f, 0.2f, 0.03f, x - 0.05f, 0.1f, 0, 0xc0c6cc);
        bx(o, 0.03f, 0.2f, 0.03f, x + 0.05f, 0.1f, 0, 0xc0c6cc);
      }
      break; }
    case PK_LIPO: {     /* a LiPo pack: silver wrap, a yellow label, red and black leads */
      bx(o, 0.9f, 0.26f, 0.6f, 0, 0.3f, 0, 0xb8bec6);
      bx(o, 0.7f, 0.27f, 0.4f, 0, 0.3f, 0, 0xe8c43a);
      bx(o, 0.05f, 0.05f, 0.5f, -0.08f, 0.3f, 0.5f, 0xd8302a);
      bx(o, 0.05f, 0.05f, 0.5f, 0.08f, 0.3f, 0.5f, 0x16181c);
      break; }
    case PICK_WEAPON + WP_BANDS:   /* the repeater as found: plywood frame, charred edge, a band on the nose */
      bx(o, 0.1f, 0.28f, 1.1f, 0, 0.45f, 0, 0xc89b62);
      bx(o, 0.105f, 0.04f, 1.1f, 0, 0.6f, 0, 0x3a2a1a);
      bx(o, 0.09f, 0.36f, 0.15f, 0, 0.2f, -0.38f, 0xb4884f);
      bx(o, 0.14f, 0.05f, 0.05f, 0, 0.62f, 0.55f, 0xd8503a);
      break;
    case PICK_WEAPON + WP_GLUE:
      bx(o, 0.18f, 0.25f, 0.65f, 0, 0.5f, 0, 0x5b6068);
      bx(o, 0.15f, 0.4f, 0.16f, 0, 0.2f, -0.18f, 0x3c4046);
      cyz(o, 0.055f, 0.4f, 0, 0.53f, -0.5f, C_GOO);
      cyz(o, 0.032f, 0.22f, 0, 0.47f, 0.42f, 0xc8a050);
      break;
    case PICK_WEAPON + WP_COIL:
      cyz(o, 0.055f, 1.4f, 0, 0.5f, 0, 0x8a8f96);
      for (int i = 0; i < 3; i++) cyz(o, 0.1f, 0.18f, 0, 0.5f, -0.3f + i * 0.35f, 0xb8642c);
      for (int i = 0; i < 3; i++) cy(o, 0.07f, 0.22f, 0, 0.3f, -0.35f + i * 0.18f, 0x2a3a8a, 8);
      bx(o, 0.05f, 0.05f, 0.05f, 0.08f, 0.63f, -0.4f, C_TEAL);
      break;
    case PICK_WEAPON + WP_IRON:
      cyz(o, 0.07f, 0.45f, 0, 0.3f, -0.3f, 0x2a5aa8);
      cyz(o, 0.03f, 0.55f, 0, 0.3f, 0.2f, 0xaab0b8);
      t3_object_set_rotation(add(o, t3_cylinder_geometry_new(0.022f, 0.022f, 0.15f, 6, 1, false), 0xff6a2a, 0.5f, 2.0f, 0, 0.3f, 0.55f), DS_PI / 2, 0, 0);
      break;
  }
  o->visible = false;
  t3_object_add(root, o); t3_release(o);
  return o;
}
/* a pool of props per kind, built once */
#define POOL_PER 4
static t3_object *pool[PK_COUNT + WP_COUNT][POOL_PER];
static int pool_slot(int kind) { return kind >= PICK_WEAPON ? PK_COUNT + kind - PICK_WEAPON : kind; }
static t3_object *prop_take(int kind) {
  int s = pool_slot(kind), lim = kind >= PICK_WEAPON ? 1 : POOL_PER;
  for (int i = 0; i < lim; i++) {
    bool used = false;
    for (int k = 0; k < M.np; k++) if (M.P[k].prop == pool[s][i]) used = true;
    if (!used) return pool[s][i];
  }
  return NULL;
}

/* ── markers: an orange arrow bobbing over each thing the objective wants ── */
#define NCONE 6
static t3_object *cone[NCONE];
static int ncone_used;
static void build_cones(void) {
  for (int i = 0; i < NCONE; i++) {
    cone[i] = t3_object_new();
    t3_mesh *c = add(cone[i], t3_cone_geometry_new(0.55f, 1.2f, 8, 1, false), C_HIVIS, 0.5f, 1.6f, 0, 0, 0);
    t3_object_set_rotation(c, DS_PI, 0, 0);   /* point down */
    cone[i]->visible = false;
    t3_object_add(root, cone[i]); t3_release(cone[i]);
  }
}
static void cones_begin(void) { ncone_used = 0; }
static void cone_at(v3 p) {
  if (ncone_used >= NCONE) return;
  t3_object *c = cone[ncone_used++];
  c->visible = true;
  t3_object_set_position(c, p.x, p.y + 0.4f + sinf(M.t * 4 + ncone_used) * 0.3f, p.z);
  t3_object_set_rotation(c, 0, M.t * 2, 0);
}
static void cones_end(void) { for (int i = ncone_used; i < NCONE; i++) cone[i]->visible = false; }

/* ── the printers: a status screen and a coloured light on each, a power
 * strip on the floor at the rack's foot with one plug per printer ── */
static const uint32_t PR_COL[LM_PRINTER_N] = {0xe8433a, 0xffd23a, 0x2ec4b6, 0xff7a1a};
static struct {
  float hp, flash, pull_t; bool cancelled, pulling;
  t3_mesh *screen; t3_mesh *led; t3_object *plug; t3_object *cable;
  v3 socket, end;            /* the plug's place in the strip, the cable's end on the printer */
} PR[LM_PRINTER_N];
static t3_object *strip;
static t3_material *scr_on, *scr_off, *led_off;
static t3_material *led_on[LM_PRINTER_N];
static void paint_screen(cv *c, int w, int h, bool on) {
  cv_fill_hex(c, on ? 0x0a1830 : 0x07080a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, on ? 0x3aa8ff : 0x30343a, 1); cv_fill_rect(c, 2, 2, (float)w - 4, 2);
  cv_font(c, F_MONO, 15); cv_letter_spacing(c, 0); cv_text_align(c, CV_LEFT); cv_text_baseline(c, CV_ALPHABETIC);
  cv_fill_hex(c, on ? 0xf2ead8 : 0x6a2020, 1);
  cv_fill_text(c, on ? "ARMY_MEN_x500" : "PRINT CANCELLED", 6, 24);
  if (on) {
    cv_font(c, F_MONO, 22);
    cv_fill_hex(c, 0xff7a1a, 1); cv_fill_text(c, "61%", 6, 50);
    cv_fill_hex(c, 0x1c2a44, 1); cv_fill_rect(c, 56, 38, (float)w - 64, 10);
    cv_fill_hex(c, 0xff7a1a, 1); cv_fill_rect(c, 56, 38, ((float)w - 64) * 0.61f, 10);
  }
}
static void paint_on(cv *c, int w, int h) { paint_screen(c, w, h, true); }
static void paint_off(cv *c, int w, int h) { paint_screen(c, w, h, false); }
static t3_material *screen_mat(void (*fn)(cv *, int, int)) {
  const int w = 128, h = 64;
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
  return m;
}
static void set_mat(t3_mesh *m, t3_material *x) {
  if (m->materials[0] == x) return;
  t3_retain(x); t3_release(m->materials[0]); m->materials[0] = x;
}
/* a cable: a thin box stretched from a to b */
static void cable_set(t3_object *c, v3 a, v3 b) {
  v3 d = v3_sub(b, a);
  float l = v3_len(d);
  t3_object_set_position(c, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f);
  t3_object_set_scale(c, 1, 1, l > 0.01f ? l : 0.01f);
  t3_object_look_at(c, b.x, b.y, b.z);
}
static void build_printers(void) {
  scr_on = screen_mat(paint_on); scr_off = screen_mat(paint_off);
  led_off = mat(0x301010, 0.8f, 0);
  const lab_box_t *rk = &LM_PRINTER_RACK[0];
  v3 sp = v3_((rk->min[0] + rk->max[0]) * 0.5f, 0, rk->max[2] + 2.2f);
  sp.y = world_floor_y(v3_(sp.x, 2, sp.z)); if (sp.y < -1e8f) sp.y = 0;
  strip = t3_object_new();
  t3_object_set_position(strip, sp.x, sp.y, sp.z);
  bx(strip, 6.6f, 0.5f, 1.1f, 0, 0.25f, 0, 0xf2ead8);
  bx(strip, 0.5f, 0.3f, 0.3f, -3.0f, 0.62f, 0, 0xe8433a);   /* the switch, lit */
  t3_object_add(root, strip); t3_release(strip);
  static const int ORDER[LM_PRINTER_N] = {1, 2, 0, 3};    /* left to right along the strip: [2] [0] [1] [3] */
  for (int i = 0; i < LM_PRINTER_N; i++) {
    const lab_box_t *b = PRN(i);
    led_on[i] = mat(PR_COL[i], 0.5f, 2.2f);
    t3_mesh *s = t3_mesh_new(t3_plane_geometry_new(4.2f, 2.1f, 1, 1), scr_on);
    T3_OBJ(s)->frustum_culled = false;
    t3_object_set_position(s, b->max[0] - 2.1f - 0.8f, b->min[1] + 2.2f, b->max[2] + 0.08f);
    t3_object_add(root, s); t3_release(s);
    PR[i].screen = s;
    t3_geometry *g = t3_box_geometry_new(1.4f, 1.4f, 0.12f, 1, 1, 1);
    t3_mesh *l = t3_mesh_new(g, led_on[i]); t3_release(g);
    T3_OBJ(l)->frustum_culled = false;
    t3_object_set_position(l, b->min[0] + 1.3f, b->min[1] + 2.2f, b->max[2] + 0.1f);
    t3_object_add(root, l); t3_release(l);
    PR[i].led = l;
    /* its plug in the strip, coloured like its light, and the cable up to it */
    PR[i].socket = v3_(sp.x + ((float)ORDER[i] - 1.5f) * 1.45f, sp.y + 0.5f, sp.z);
    PR[i].plug = t3_object_new();
    bx(PR[i].plug, 0.8f, 0.7f, 0.8f, 0, 0.35f, 0, PR_COL[i]);
    bx(PR[i].plug, 0.12f, 0.35f, 0.08f, -0.18f, -0.1f, 0, 0xc0c6cc);
    bx(PR[i].plug, 0.12f, 0.35f, 0.08f, 0.18f, -0.1f, 0, 0xc0c6cc);
    t3_object_add(root, PR[i].plug); t3_release(PR[i].plug);
    PR[i].cable = t3_object_new();
    bx(PR[i].cable, 0.16f, 0.16f, 1, 0, 0, 0, 0x16181c);
    t3_object_add(root, PR[i].cable); t3_release(PR[i].cable);
    PR[i].end = v3_(i < 2 ? b->min[0] + 0.6f : b->max[0] - 0.6f, b->min[1] + 0.6f, b->max[2] + 0.2f);
  }
}
static void printer_set(int i, bool cancelled) {
  PR[i].cancelled = cancelled; PR[i].hp = cancelled ? 0 : PRINTER_HP; PR[i].flash = 0; PR[i].pulling = false;
  set_mat(PR[i].screen, cancelled ? scr_off : scr_on);
  set_mat(PR[i].led, cancelled ? led_off : led_on[i]);
  /* unplugged, the plug lies on the floor in front of the strip */
  v3 p = cancelled ? v3_add(PR[i].socket, v3_(0, -0.45f, 2.4f)) : PR[i].socket;
  t3_object_set_position(PR[i].plug, p.x, p.y, p.z);
  t3_object_set_rotation(PR[i].plug, cancelled ? DS_PI / 2 : 0, 0, 0);
  cable_set(PR[i].cable, PR[i].end, v3_add(p, v3_(0, 0.5f, 0)));
}
static void printer_cancel(int i) {
  if (PR[i].cancelled) return;
  printer_set(i, true);
  M.count++;
  const lab_box_t *b = PRN(i);
  sfx_at(SFX_PRINT_STOP, (b->min[0] + b->max[0]) * 0.5f, b->min[1] + 2, b->max[2], 1);
}
/* pulled by hand: the plug comes out of the strip in an arc and lands on the floor */
static void printer_pull(int i) {
  if (PR[i].cancelled) return;
  printer_cancel(i);
  PR[i].pulling = true; PR[i].pull_t = 0;
  sfx(SFX_PLINK, 0, 1);
}
static void printers_tick(float dt) {
  for (int i = 0; i < LM_PRINTER_N; i++) {
    if (PR[i].pulling) {
      PR[i].pull_t += dt;
      float u = ds_clamp(PR[i].pull_t / 0.55f, 0, 1);
      v3 p = v3_add(PR[i].socket, v3_(0, sinf(u * DS_PI) * 1.6f - 0.45f * u, 2.4f * u));
      t3_object_set_position(PR[i].plug, p.x, p.y, p.z);
      t3_object_set_rotation(PR[i].plug, u * DS_PI * 2.5f, 0, 0);
      cable_set(PR[i].cable, PR[i].end, v3_add(p, v3_(0, 0.5f, 0)));
      if (u >= 1) { PR[i].pulling = false; t3_object_set_rotation(PR[i].plug, DS_PI / 2, 0, 0); }
    }
    if (PR[i].flash > 0) {
      PR[i].flash -= dt;
      bool off = PR[i].flash > 0 && fmodf(PR[i].flash, 0.1f) < 0.05f;
      set_mat(PR[i].screen, PR[i].cancelled || off ? scr_off : scr_on);
    }
    if (!PR[i].cancelled) led_on[i]->emissive_intensity = 1.6f + 1.4f * sinf(M.t * 6 + i);   /* a slow pulse: still printing */
  }
}
static v3 printer_front(int i) { const lab_box_t *b = PRN(i); return v3_((b->min[0] + b->max[0]) * 0.5f, b->min[1], b->max[2] + 1.0f); }
/* the printer the player can unplug from here: at its plug in the strip, or at its own front on its shelf */
static int printer_in_reach(v3 p) {
  int best = -1; float bd = 1e9f;
  for (int i = 0; i < LM_PRINTER_N; i++) {
    if (PR[i].cancelled) continue;
    v3 s = PR[i].socket, f = printer_front(i);
    float ds = (p.x - s.x) * (p.x - s.x) + (p.z - s.z) * (p.z - s.z);
    float df = (p.x - f.x) * (p.x - f.x) + (p.z - f.z) * (p.z - f.z);
    if (ds < 3.0f * 3.0f && fabsf(p.y - s.y) < 2.5f && ds < bd) { bd = ds; best = i; }
    if (df < 3.5f * 3.5f && p.y > f.y - 1.0f && p.y < f.y + 3.0f && df < bd) { bd = df; best = i; }
  }
  return best;
}

/* ── the window displays: four spare drone props ── */
#define NPROP 4
static t3_object *spare[NPROP];
static t3_object *packprop;
static void build_pack(void) {   /* the charger's battery pack: a fat yellow LiPo with a black label and leads */
  packprop = t3_object_new();
  bx(packprop, 1.8f, 0.7f, 1.1f, 0, 0.35f, 0, 0xe8c43a);
  bx(packprop, 1.2f, 0.72f, 0.6f, 0, 0.36f, 0, 0x16181c);
  bx(packprop, 0.08f, 0.08f, 0.7f, -0.2f, 0.4f, 0.85f, 0xd8302a);
  bx(packprop, 0.08f, 0.08f, 0.7f, 0.2f, 0.4f, 0.85f, 0x16181c);
  packprop->visible = false;
  t3_object_add(root, packprop); t3_release(packprop);
}
static void build_spares(void) {
  for (int i = 0; i < NPROP; i++) {
    t3_object *o = t3_object_new();
    cy(o, 0.16f, 0.24f, 0, 0.5f, 0, 0x30343c, 10);
    bx(o, 2.2f, 0.06f, 0.32f, 0, 0.62f, 0, i & 1 ? C_HIVIS : C_TEAL);
    o->visible = false;
    t3_object_add(root, o); t3_release(o);
    spare[i] = o;
  }
}

/* ── the kitchen: the battery charger on the table ── */
#define CHARGE_S 60.f
#define CHARGE_R 18.f
static t3_object *charger, *charge_led[4];
static void build_charger(void) {
  charger = t3_object_new();
  bx(charger, 1.8f, 0.6f, 1.2f, 0, 0.3f, 0, 0x2b2f36);
  bx(charger, 1.2f, 0.3f, 0.7f, 0, 0.75f, 0, 0xe8c43a);   /* the pack on it */
  for (int i = 0; i < 4; i++) {
    t3_geometry *g = t3_box_geometry_new(0.22f, 0.12f, 0.12f, 1, 1, 1);
    t3_mesh *l = t3_mesh_new(g, mat(0x1a3a1a, 0.6f, 0)); t3_release(g);
    T3_OBJ(l)->frustum_culled = false;
    t3_object_set_position(l, -0.6f + i * 0.4f, 0.45f, 0.62f);
    t3_object_add(charger, l); t3_release(l);
    charge_led[i] = T3_OBJ(l);
  }
  charger->visible = false;
  t3_object_add(root, charger); t3_release(charger);
}
static void charger_lights(void) {
  for (int i = 0; i < 4; i++) {
    t3_material *m = ((t3_mesh *)charge_led[i])->materials[0];
    bool lit = M.charge >= (float)(i + 1) * CHARGE_S / 4 || (M.charging && M.charge >= (float)i * CHARGE_S / 4 && fmodf(M.t, 0.8f) < 0.4f);
    m->color = t3_color_hex(lit ? 0x5aff6a : 0x1a3a1a);
    m->emissive = t3_color_hex(0x5aff6a); m->emissive_intensity = lit ? 1.5f : 0;
  }
}
static v3 charger_pos(void) { return top_of(KTABLE, 0, 0); }
static bool within(v3 p, float r) { v3 c = lm_c(KTABLE); return (p.x - c.x) * (p.x - c.x) + (p.z - c.z) * (p.z - c.z) < r * r; }
static bool by_charger(v3 p) { return within(p, CHARGE_R); }   /* close enough to keep it charging */
static bool at_charger(v3 p) { return within(p, 14); }          /* close enough to plug it in */

/* ── the metal fab bay: the army's supply dump ── */
#define CRATE_HP 90.f
static t3_object *crate_obj[NCRATE];
static void build_crates(void) {
  for (int i = 0; i < NCRATE; i++) {
    t3_object *o = t3_object_new();
    bx(o, 3.0f, 2.4f, 2.6f, 0, 1.2f, 0, 0xb08a58);
    bx(o, 3.05f, 0.3f, 0.5f, 0, 1.6f, 0, 0xd8c8a0);          /* tape */
    t3_mesh *top = bx(o, 2.5f, 2.0f, 2.2f, 0.15f, 3.4f, -0.05f, 0xa07c4c);
    t3_object_set_rotation(top, 0, 0.3f, 0);
    bx(o, 0.08f, 1.6f, 0.08f, 0.6f, 5.2f, 0, 0x2b2f36);      /* a flag on top */
    bx(o, 0.9f, 0.55f, 0.04f, 1.05f, 5.7f, 0, C_ARMY);
    o->visible = false;
    t3_object_add(root, o); t3_release(o);
    crate_obj[i] = o;
  }
}
static void crate_box(int i, lab_box_t *b) {
  v3 p = M.crate[i];
  b->min[0] = p.x - 1.6f; b->min[1] = p.y; b->min[2] = p.z - 1.4f;
  b->max[0] = p.x + 1.6f; b->max[1] = p.y + 4.5f; b->max[2] = p.z + 1.4f;
}
static void crate_wreck(int i) {
  if (M.crate_hp[i] <= -1000) return;
  M.crate_hp[i] = -1000;
  crate_obj[i]->visible = false;
  v3 p = v3_add(M.crate[i], v3_(0, 1.6f, 0));
  fx_explosion(p, 2.5f);
  fx_poof(p, 0xb08a58, 3.0f);
  sfx_at(SFX_BOOM, p.x, p.y, p.z, 0.7f);
  M.count++;
}

void mission_init(t3_scene *scene) {
  root = t3_object_new();
  t3_object_add(&scene->base, root); t3_release(root);
  for (int k = 0; k < PK_COUNT; k++) for (int i = 0; i < POOL_PER; i++) pool[k][i] = build_pickup(k);
  for (int w = 0; w < WP_COUNT; w++) pool[PK_COUNT + w][0] = build_pickup(PICK_WEAPON + w);
  build_printers();
  build_cones();
  build_spares();
  build_pack();
  build_charger();
  build_crates();
#ifdef MISSION_CHECK
  for (int l = 0; l < MISSION_N; l++) {
    mission_load(l);
    int men = 0;
    for (int i = 0; i < M.na; i++) if (M.A[i].kind == AC_SQUAD) men += M.A[i].n;
    ds_log("[mission] L%d %s: start %.1f %.1f %.1f, %d steps, %d army men, %d pickups, %d beats", l + 1, NAMES[l], M.start.x, M.start.y, M.start.z, M.na, men, M.np, mission_radio_count(l));
    for (int k = 0; k < M.np; k++) ds_log("[mission]   pickup %d at %.1f %.2f %.1f", M.P[k].kind, M.P[k].at.x, M.P[k].at.y, M.P[k].at.z);
    for (int k = 0; k < M.ncrate; k++) ds_log("[mission]   crate at %.1f %.2f %.1f", M.crate[k].x, M.crate[k].y, M.crate[k].z);
    for (int i = 0; i < M.na; i++) if (M.A[i].kind == AC_SQUAD) {
      act_t *a = &M.A[i];
      ds_log("[mission]   squad type %d x%d at %.1f %.2f %.1f", a->type, a->n, a->at.x, a->at.y, a->at.z);
      if (a->type == EN_PARA || a->type >= EN_SERGEANT) continue;
      for (int k = 0; k < a->n; k++) { v3 p = member_pos(a, k); if (fabsf(p.y - a->at.y) > 0.6f) ds_log("[mission]     man %d lands at %.2f", k, p.y); land(p, p.y + 0.5f); }
    }
  }
  mission_load(0);
#endif
}

void mission_load(int level) {
  for (int k = 0; k < M.np; k++) if (M.P[k].prop) M.P[k].prop->visible = false;
  memset(&M, 0, sizeof M);
  M.level = (unsigned)level < MISSION_N ? level : 0;
  M.weapon = -1;
  M.start = start_of(M.level, &M.start_yaw);
  switch (M.level) {
    case LV_LASER: level_laser(); break;
    case LV_STORAGE: level_storage(); break;
    case LV_PRINTS: level_printfarm(); break;
    case LV_WINDOWS: level_windows(); break;
    case LV_COWORK: level_coworking(); break;
    case LV_WOODSHOP: level_woodshop(); break;
    case LV_KITCHEN: level_kitchen(); break;
    case LV_METALFAB: level_metalfab(); break;
    case LV_MACHINE: level_machineshop(); break;
  }
  M.check = M.start;
  int sp = 0;
  for (int k = 0; k < M.np; k++) {
    pick_t *p = &M.P[k];
    p->prop = p->kind == PICK_PROP ? (sp < NPROP ? spare[sp++] : NULL) : p->kind == PICK_PACK ? packprop : prop_take(p->kind);
    if (p->prop) { t3_object_set_position(p->prop, p->at.x, p->at.y, p->at.z); p->prop->visible = true; }
  }
  for (int i = sp; i < NPROP; i++) spare[i]->visible = false;
  if (M.level != LV_KITCHEN) packprop->visible = false;
  /* the print job runs until the print farm cancels it */
  for (int i = 0; i < LM_PRINTER_N; i++) printer_set(i, M.level > LV_PRINTS);
  /* the charger sits on the kitchen table from the kitchen on */
  charger->visible = M.level >= LV_KITCHEN;
  if (charger->visible) { v3 c = charger_pos(); t3_object_set_position(charger, c.x, c.y, c.z); }
  if (M.level > LV_KITCHEN) M.charge = CHARGE_S;
  charger_lights();
  for (int i = 0; i < NCRATE; i++) {
    crate_obj[i]->visible = i < M.ncrate;
    if (i < M.ncrate) { M.crate_hp[i] = CRATE_HP; t3_object_set_position(crate_obj[i], M.crate[i].x, M.crate[i].y, M.crate[i].z); t3_object_set_rotation(crate_obj[i], 0, (float)i * 0.7f, 0); }
  }
  for (int i = 0; i < NCONE; i++) cone[i]->visible = false;
}

v3 mission_start(int level, float *yaw) {
  if (level == M.level && M.nt > 0) { if (yaw) *yaw = M.start_yaw; return M.start; }
  return start_of(level, yaw);
}

v3 mission_checkpoint(void) { return M.check; }

/* ── the update ── */
static bool in_zone(const trig_t *t, v3 p) { return p.x >= t->x0 && p.x <= t->x1 && p.z >= t->z0 && p.z <= t->z1; }

/* the nearest of a set of places, for the HUD's arrow */
static v3 nearest_of(const v3 *ps, const bool *on, int n, v3 from) {
  v3 best = from; float bd = 1e18f;
  for (int i = 0; i < n; i++) if (on[i]) {
    float d = v3_dot(v3_sub(ps[i], from), v3_sub(ps[i], from));
    if (d < bd) { bd = d; best = ps[i]; }
  }
  return best;
}

static void objective(mission_out_t *o, v3 player) {
  o->objective = NULL; o->marker = false;
  v3 mk = v3_(0, 0, 0);
  bool lift = true;
  switch (M.level) {
    case LV_LASER:
      if (!M.have_weapon) { o->objective = "GRAB THE REPEATER"; mk = M.weapon_pos; }
      else { o->objective = "HEAD FOR MEMBER STORAGE"; mk = v3_(AR(AREA_LASER_CORNER)->min[0], 0, aisle_back()); }
      break;
    case LV_STORAGE:
      if (!M.have_weapon) { o->objective = "FIND THE HOT GLUE GUN"; mk = M.weapon_pos; }
      else { o->objective = "PAST THE SEWING TABLE"; mk = v3_(AR(AREA_SEWING)->min[0], 0, 30); }
      break;
    case LV_PRINTS:
      if (M.count < LM_PRINTER_N) {
        snprintf(objective_buf, sizeof objective_buf, "PULL THE PLUGS %d/%d AT THE RACK", M.count, LM_PRINTER_N);
        o->objective = objective_buf;
        v3 s[LM_PRINTER_N]; bool on[LM_PRINTER_N];
        for (int i = 0; i < LM_PRINTER_N; i++) { s[i] = PR[i].socket; on[i] = !PR[i].cancelled; }
        mk = nearest_of(s, on, LM_PRINTER_N, player);
      } else { o->objective = "HEAD FOR THE FRONT WINDOWS"; mk = v3_(AR(AREA_WELCOME)->min[0] - 12, 0, 60); }
      break;
    case LV_WINDOWS:
      if (M.count < NPROP) {
        snprintf(objective_buf, sizeof objective_buf, "FIND THE SPARE PROPS %d/%d", M.count, NPROP);
        o->objective = objective_buf;
        v3 s[MAX_PICK]; bool on[MAX_PICK];
        for (int k = 0; k < M.np; k++) { s[k] = M.P[k].at; on[k] = M.P[k].kind == PICK_PROP && !M.P[k].taken; }
        mk = nearest_of(s, on, M.np, player);
      } else { o->objective = "BACK TO COWORKING"; mk = v3_(lm_c(CT(0)).x - 12, 0, AR(AREA_COWORKING)->min[2] - 4); }
      break;
    case LV_COWORK:
      if (!M.have_weapon) { o->objective = "GET TO THE COILGUN"; mk = M.weapon_pos; }
      else { o->objective = "COILGUN FOUND"; return; }
      break;
    case LV_WOODSHOP:
      if (!M.have_weapon) { o->objective = "GRAB THE SOLDERING IRON"; mk = M.weapon_pos; }
      else if (!M.arena) { o->objective = "INTO THE WOODSHOP"; mk = v3_(AR(AREA_WOODSHOP)->min[0], 0, aisle_back() + 10); }
      else if (!enemy_boss_dead()) { o->objective = "TAKE DOWN THE SERGEANT"; return; }
      else { o->objective = "INTO THE BACK HALL"; mk = v3_(AR(AREA_WOODSHOP)->max[0] + 8, 0, aisle_back() + 6); }
      break;
    case LV_KITCHEN:
      mk = charger_pos();
      if (M.charge >= CHARGE_S) { o->objective = "BATTERIES CHARGED"; return; }
      if (!M.have_weapon) {
        bool under = player.y < BASEMENT->max[1] - 1;
        o->objective = under ? "GET THE CHARGER PACK" : "DOWN THE HATCH FOR THE PACK";
        mk = under ? M.pack : hatch_gap();
        break;
      }
      if (player.y < BASEMENT->max[1] - 1) { o->objective = "CLIMB OUT THE HATCH"; mk = hatch_gap(); lift = false; break; }
      if (!M.charging) o->objective = "PLUG IN THE CHARGER";
      else {
        snprintf(objective_buf, sizeof objective_buf, by_charger(player) ? "HOLD THE KITCHEN %d%%" : "GET BACK TO THE CHARGER %d%%", (int)(M.charge * 100 / CHARGE_S));
        o->objective = objective_buf;
      }
      lift = false;
      break;
    case LV_METALFAB:
      if (M.count < M.ncrate) {
        snprintf(objective_buf, sizeof objective_buf, "WRECK THE SUPPLY DUMP %d/%d", M.count, M.ncrate);
        o->objective = objective_buf;
        v3 s[NCRATE]; bool on[NCRATE];
        for (int i = 0; i < M.ncrate; i++) { s[i] = v3_add(M.crate[i], v3_(0, 4.5f, 0)); on[i] = M.crate_hp[i] > -1000; }
        mk = nearest_of(s, on, M.ncrate, player);
      } else { o->objective = "THROUGH TO THE MACHINE SHOP"; mk = v3_(AR(AREA_METAL_FAB_BAY)->min[0], 0, lm_c(WELD).z - 20); }
      break;
    case LV_MACHINE:
      if (!enemy_boss_dead()) { o->objective = M.arena ? "STOP THE GENERAL" : "FIND THE BREAKER PANEL"; if (M.arena) return; }
      else o->objective = "FLIP THE MAIN BREAKER";
      mk = v3_((PANEL->min[0] + PANEL->max[0]) * 0.5f, (PANEL->min[1] + PANEL->max[1]) * 0.5f, PANEL->min[2]);
      lift = false;
      break;
  }
  o->marker = true; o->marker_pos = lift ? v3_add(mk, v3_(0, 1.0f, 0)) : mk;
}

/* the things in the world the objective wants, each marked with an arrow */
static void markers(void) {
  cones_begin();
  if (M.level == LV_PRINTS) for (int i = 0; i < LM_PRINTER_N; i++) if (!PR[i].cancelled) {
    cone_at(v3_add(PR[i].socket, v3_(0, 1.6f, 0)));
    fx_glow(v3_add(PR[i].socket, v3_(0, 0.4f, 0)), 1.4f + 0.5f * sinf(M.t * 6 + i), PR_COL[i]);
    const lab_box_t *b = PRN(i);
    fx_glow(v3_(b->min[0] + 1.3f, b->min[1] + 2.2f, b->max[2] + 0.4f), 2.2f + 0.8f * sinf(M.t * 6 + i), PR_COL[i]);
  }
  if (M.level == LV_WINDOWS) for (int k = 0; k < M.np; k++) if (M.P[k].kind == PICK_PROP && !M.P[k].taken) cone_at(v3_add(M.P[k].at, v3_(0, 2.4f, 0)));
  if (M.level == LV_KITCHEN && !M.have_weapon) cone_at(v3_add(M.pack, v3_(0, 2.4f, 0)));
  if (M.level == LV_KITCHEN && M.have_weapon && M.charge < CHARGE_S) {
    v3 c = charger_pos();
    cone_at(v3_add(c, v3_(0, 2.6f, 0)));
    fx_glow(v3_add(c, v3_(0, 0.8f, 0)), 1.6f + 0.6f * sinf(M.t * 5), 0x5aff6a);
  }
  if (M.level == LV_METALFAB) for (int i = 0; i < M.ncrate; i++) if (M.crate_hp[i] > -1000) cone_at(v3_add(M.crate[i], v3_(0, 7.0f, 0)));
  cones_end();
}

static bool level_done(v3 p) {
  switch (M.level) {
    case LV_LASER: return p.x < AR(AREA_LASER_CORNER)->min[0] + 2 && p.z > AR(AREA_LASER_CORNER)->max[2] - 2 && p.z < AR(AREA_COWORKING)->min[2];
    case LV_STORAGE: return M.have_weapon && p.x < AR(AREA_SEWING)->min[0] && p.z < 50;
    case LV_PRINTS: return M.count >= LM_PRINTER_N && p.x < AR(AREA_WELCOME)->min[0] - 8 && p.z > 34 && p.z < 92;
    case LV_WINDOWS: return M.count >= NPROP && p.x > COUNTER->max[0] + 6 && p.z > COUNTER->min[2] && p.z < AR(AREA_COWORKING)->min[2] + 12;
    case LV_COWORK: return M.have_weapon && M.t - M.weapon_t > 2.5f;
    case LV_WOODSHOP: return enemy_boss_dead() && p.x > AR(AREA_WOODSHOP)->max[0] + 3 && p.z < AR(AREA_MACHINE_SHOP)->min[2] + 2;
    case LV_KITCHEN: return M.charge >= CHARGE_S && M.t - M.charged_t > 2.5f;
    case LV_METALFAB: return M.count >= M.ncrate && p.x < AR(AREA_METAL_FAB_BAY)->min[0] + 1;
    case LV_MACHINE: {
      const lab_box_t *b = PANEL;
      return enemy_boss_dead() && p.x > b->min[0] - 3 && p.x < b->max[0] + 3 && p.y > b->min[1] - 3 && p.y < b->max[1] + 3 && p.z > b->min[2] - 5;
    }
  }
  return false;
}

void mission_update(float dt, v3 player, mission_out_t *out) {
  M.t += dt;
  out->radio_beat = out->pickup = out->weapon_found = -1;
  out->banner = NULL; out->checkpoint = false; out->done = false; out->boss_arena = false; out->prompt = NULL;
  /* the arena: the woodshop (the Sergeant), the machine shop entered from the metal fab bay (the General) */
  if (M.level == LV_WOODSHOP && player.x > AR(AREA_WOODSHOP)->min[0] + 1 && player.z > AR(AREA_WOODSHOP)->min[2] - 2) M.arena = true;
  if (M.level == LV_MACHINE && player.x < AR(AREA_MACHINE_SHOP)->max[0] - 22) M.arena = true;
  out->boss_arena = M.arena;
  /* the kitchen's charge runs while someone is at the table */
  if (M.level == LV_KITCHEN && M.charging && M.charge < CHARGE_S) {
    if (by_charger(player)) M.charge = fminf(CHARGE_S, M.charge + dt);
    int stage = M.charge >= CHARGE_S ? 4 : 1 + (int)(M.charge / (CHARGE_S / 3));
    if (stage > M.count) M.count = stage;
    if (M.charge >= CHARGE_S) { M.charged_t = M.t; out->banner = "BATTERIES CHARGED"; sfx(SFX_UI_OK, 0, 1); }
    charger_lights();
  }
  /* triggers */
  for (int i = 0; i < M.nt; i++) {
    trig_t *t = &M.T[i];
    if (t->fired) continue;
    bool go = false;
    switch (t->kind) {
      case TR_LOAD: go = true; break;
      case TR_ZONE: go = in_zone(t, player); break;
      case TR_WEAPON: go = M.have_weapon; break;
      case TR_COUNT: go = M.count >= (int)t->val; break;
      case TR_BELOW: go = player.y < t->val; break;
      case TR_BOSS_HP: go = enemy_boss_active() && enemy_boss_health() <= t->val; break;
      case TR_BOSS_DEAD: go = enemy_boss_dead(); break;
    }
    if (go) { t->fired = true; t->t = M.t; }
  }
  /* what they set off: squads wait for room in the army, beats for the walkie */
  for (int i = 0; i < M.na; i++) {
    act_t *a = &M.A[i];
    trig_t *t = &M.T[a->trig];
    if (a->done || !t->fired || M.t - t->t < a->delay) continue;
    switch (a->kind) {
      case AC_SQUAD:
        if (enemy_alive() + a->n > MAX_ALIVE) continue;
        spawn_squad(a);
        break;
      case AC_BEAT:
        if (M.nq < 16) M.queue[M.nq++] = a->beat;
        break;
      case AC_CHECK:
        M.check = a->at; out->checkpoint = true;
        break;
    }
    a->done = true;
  }
  M.beat_gap -= dt;
  if (M.nq > 0 && M.beat_gap <= 0 && !voice_busy()) {
    int b = M.queue[0];
    memmove(M.queue, M.queue + 1, (size_t)(M.nq - 1) * sizeof M.queue[0]);
    M.nq--;
    const char *spk, *line = mission_radio(M.level, b, &spk);
    if (line) { out->radio_beat = b; M.beat_gap = 1.2f + (float)strlen(line) * 0.055f; }
  }
  /* pickups: bob and spin, taken by walking through */
  v3 chest = v3_add(player, v3_(0, 0.9f, 0));
  for (int k = 0; k < M.np; k++) {
    pick_t *p = &M.P[k];
    if (p->taken || !p->prop) continue;
    float ph = M.t * 2.2f + k * 1.3f;
    float y = p->at.y + 0.25f + sinf(ph) * 0.12f;
    t3_object_set_position(p->prop, p->at.x, y, p->at.z);
    t3_object_set_rotation(p->prop, 0, M.t * (p->kind == PICK_PROP ? 4.0f : 1.6f) + k, 0);
    v3 d = v3_sub(v3_(p->at.x, y + 0.4f, p->at.z), chest);
    p->prop->visible = v3_dot(d, d) < 160 * 160;
    if (d.x * d.x + d.z * d.z < TAKE_R * TAKE_R && fabsf(d.y) < 1.6f) {
      p->taken = true; p->prop->visible = false;
      if (p->kind == PICK_PACK) {
        M.have_weapon = true; M.weapon_t = M.t;   /* the level's key item: on_weapon fires */
        out->banner = "CHARGER PACK";
        sfx(SFX_WEAPON, 0, 1);
      } else if (p->kind == PICK_PROP) {
        M.count++;
        snprintf(banner_buf, sizeof banner_buf, "SPARE PROP %d/%d", M.count, NPROP);
        out->banner = banner_buf;
        sfx(SFX_PICKUP, 0, 1);
      } else if (p->kind >= PICK_WEAPON) {
        out->weapon_found = p->kind - PICK_WEAPON;
        if (p->kind - PICK_WEAPON == M.weapon) { M.have_weapon = true; M.weapon_t = M.t; }
      } else out->pickup = p->kind;
      break;   /* one a frame: the outputs hold one each */
    }
  }
  /* the printers and the dump: banners as they go */
  if (M.level == LV_PRINTS) {
    printers_tick(dt);
    if (M.count != M.last_count && M.count > 0) out->banner = M.count >= LM_PRINTER_N ? "JOB QUEUE EMPTY" : "PRINT CANCELLED";
    if (printer_in_reach(player) >= 0) out->prompt = "PULL THE PLUG";
  }
  if (M.level == LV_METALFAB && M.count != M.last_count && M.count > 0) {
    snprintf(banner_buf, sizeof banner_buf, M.count >= M.ncrate ? "SUPPLY DUMP WRECKED" : "CRATE %d/%d", M.count, M.ncrate);
    out->banner = banner_buf;
  }
  if (M.level == LV_KITCHEN && M.have_weapon && !M.charging && at_charger(player)) out->prompt = "PLUG IN THE CHARGER";
  M.last_count = M.count;
  markers();
  objective(out, player);
  out->done = level_done(player);
}

/* ── the interact button ── */
bool mission_interact(v3 player) {
  if (M.level == LV_PRINTS) {
    int i = printer_in_reach(player);
    if (i >= 0) { printer_pull(i); return true; }
  }
  if (M.level == LV_KITCHEN && M.have_weapon && !M.charging && at_charger(player)) {
    M.charging = true;
    if (M.count < 1) M.count = 1;
    sfx(SFX_UI_OK, 0, 1);
    return true;
  }
  return false;
}

/* ── the printers and the crates as targets ── */
static float box_dist(const lab_box_t *b, v3 p) {
  float dx = fmaxf(fmaxf(b->min[0] - p.x, 0), p.x - b->max[0]);
  float dy = fmaxf(fmaxf(b->min[1] - p.y, 0), p.y - b->max[1]);
  float dz = fmaxf(fmaxf(b->min[2] - p.z, 0), p.z - b->max[2]);
  return sqrtf(dx * dx + dy * dy + dz * dz);
}
static bool ray_box(const lab_box_t *b, v3 o, v3 d, float max_t, float *t) {
  float t0 = 0, t1 = max_t;
  const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
  for (int a = 0; a < 3; a++) {
    if (fabsf(dd[a]) < 1e-8f) { if (oo[a] < b->min[a] || oo[a] > b->max[a]) return false; continue; }
    float inv = 1 / dd[a], ta = (b->min[a] - oo[a]) * inv, tb = (b->max[a] - oo[a]) * inv;
    if (ta > tb) { float s = ta; ta = tb; tb = s; }
    if (ta > t0) t0 = ta;
    if (tb < t1) t1 = tb;
    if (t0 > t1) return false;
  }
  *t = t0;
  return true;
}

bool mission_hit(v3 at, float dmg) {
  bool hit = false;
  if (M.level == LV_PRINTS) for (int i = 0; i < LM_PRINTER_N; i++) {
    float d = box_dist(PRN(i), at);
    if (d <= 0.35f) {
      hit = true;
      if (PR[i].cancelled) continue;
      PR[i].hp -= dmg; PR[i].flash = 0.3f;
      if (PR[i].hp <= 0) printer_cancel(i);
    } else if (d < 4.5f && dmg >= 55 && !PR[i].cancelled) {
      printer_cancel(i);   /* a blast beside it (a LiPo pops at 60): the print is over */
    }
  }
  if (M.level == LV_METALFAB) for (int i = 0; i < M.ncrate; i++) {
    if (M.crate_hp[i] <= -1000) continue;
    lab_box_t b; crate_box(i, &b);
    float d = box_dist(&b, at);
    if (d <= 0.35f) { hit = true; M.crate_hp[i] -= dmg; if (M.crate_hp[i] <= 0) crate_wreck(i); }
    else if (d < 4.5f && dmg >= 55) crate_wreck(i);
  }
  return hit;
}

bool mission_raycast(v3 o, v3 d, float max_dist, float *t) {
  float best = max_dist, tt; bool any = false;
  if (M.level == LV_PRINTS) for (int i = 0; i < LM_PRINTER_N; i++)
    if (ray_box(PRN(i), o, d, best, &tt) && tt < best) { best = tt; any = true; }
  if (M.level == LV_METALFAB) for (int i = 0; i < M.ncrate; i++) {
    if (M.crate_hp[i] <= -1000) continue;
    lab_box_t b; crate_box(i, &b);
    if (ray_box(&b, o, d, best, &tt) && tt < best) { best = tt; any = true; }
  }
  if (any && t) *t = best;
  return any;
}

/* ── level wiring: the cutscenes, the boss levels, the weapons found so far ── */
int mission_intro_cut(int level) {
  static const int before[MISSION_N] = {CUT_PROLOGUE, CUT_STORAGE, CUT_PRINTFARM, CUT_WINDOWS, CUT_COWORKING,
                                        CUT_WOODSHOP, CUT_KITCHEN, CUT_METALFAB, CUT_MACHINESHOP};
  return level >= 0 && level < MISSION_N ? before[level] : -1;
}
int mission_boss_cut(int level) { return level == LV_WOODSHOP ? CUT_SERGEANT : level == LV_MACHINE ? CUT_GENERAL : -1; }
bool mission_is_final(int level) { return level == LV_MACHINE; }
unsigned mission_weapons_at_start(int level) {
  unsigned w = 1u << WP_BANDS;                   /* the repeater: the laser corner's scrap bin */
  if (level > LV_STORAGE) w |= 1u << WP_GLUE;    /* a member's bin */
  if (level > LV_COWORK) w |= 1u << WP_COIL;     /* the electronics bench */
  if (level > LV_WOODSHOP) w |= 1u << WP_IRON;   /* the soldering station */
  return w;
}
