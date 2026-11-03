/* cutscene.h: in-engine cutscenes, staged in the lab itself (docs/STORY.md).
 * cutscene.c owns the shots, any props it stages (army men posed for a
 * scene, the laser's flash), the captions and every line of dialogue;
 * game.c runs it and shows what it asks for. */
#ifndef SP_CUTSCENE_H
#define SP_CUTSCENE_H
#include "ds.h"
#include "three.h"
#include "input.h"
#include "hero.h"
#include "voice.h"

typedef enum {
  CUT_PROLOGUE,     /* before level 1: the SCALE button */
  CUT_STORAGE,      /* before level 2 */
  CUT_PRINTFARM,    /* before level 3 */
  CUT_COWORKING,    /* before level 4 */
  CUT_WOODSHOP,     /* before level 5 */
  CUT_SERGEANT,     /* level 5: the Sergeant's reveal */
  CUT_MACHINESHOP,  /* before level 6 */
  CUT_GENERAL,      /* level 6: the General's reveal */
  CUT_ENDING,
  CUT_WINDOWS,      /* before the window displays */
  CUT_KITCHEN,      /* before the kitchen and darkroom */
  CUT_METALFAB,     /* before the metal fab bay */
  CUT_COUNT
} cut_id;

/* one hero in a cutscene frame */
typedef struct {
  bool visible; v3 pos; float yaw; float size;    /* size 1 = toy, LAB_SCALE = full size */
  const char *clip; float clip_speed; bool clip_loop;
  float aim;                                      /* 0..1 the aim pose */
  bool jetpack; float jet;                        /* worn, thrust */
  bool armed;                                     /* the rubber band repeater in hand */
} cine_hero_t;

/* what a cutscene frame wants shown */
typedef struct {
  v3 cam_pos, cam_target; float fov;
  cine_hero_t hero[HERO_COUNT];
  float letterbox;                       /* 0..1 */
  const char *speaker, *text; int chars; /* the caption, typed out to chars */
  float skip;                            /* skip hold progress 0..1 */
  float fade;                            /* 0 clear .. 1 black */
  float flash;                           /* 0..1 a white flash (the laser) */
  float lights;                          /* 0..1 the lab's lights (the breaker) */
} cine_view_t;

void cutscene_init(t3_scene *scene);     /* once, after world_init */
/* player: who the player picked (the other is the buddy) */
void cutscene_start(cut_id id, hero_id player);
bool cutscene_active(void);
/* the player's and buddy's feet when an in-level scene starts, so it stages around them */
void cutscene_hint(v3 player, float yaw, v3 buddy);
/* step; false once it has finished or been skipped (hold START / Enter / A) */
bool cutscene_update(float dt, const intent_t *I, cine_view_t *out);

/* the voice for a caption speaker name ("MOHEEB", "THE GENERAL", ...) */
vo_speaker cutscene_voice_for(const char *speaker);
/* a dry line for the death screen and the loading card */
const char *story_quip(int seed);

#endif
