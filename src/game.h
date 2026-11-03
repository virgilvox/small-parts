/* game.h: the save, the game's entry points and the render hooks main.c provides. */
#ifndef DS_GAME_H
#define DS_GAME_H
#include "ds.h"
#include "three.h"

/* the save region (RetroArch .srm, the page's storage) */
typedef struct {
  uint32_t magic;            /* DS_SAVE_MAGIC once written */
  uint8_t level_reached;     /* furthest level unlocked, 0-based */
  uint8_t music, shake, invert_y;
  uint32_t best_ms[12];      /* best clear time per level, 0 = none */
  uint32_t best_score[12];
  uint8_t hero;              /* hero.h HERO_*: who the player picked last */
  uint8_t first_person;
  uint8_t pad[14];
} ds_save_t;
#define DS_SAVE_MAGIC 0x32545053u   /* 'SPT2' (12 levels of bests) */
extern ds_save_t DS_SAVE;

extern t3_renderer *DS_R;
void render_scene(t3_scene *scene, t3_camera *cam);   /* into the frame target */
void render_target(void);                             /* bind the frame target (for raw GL overlays) */
void render_clear(uint32_t rgb);

void game_init(void);
void game_frame(float dt);
void game_draw(void);

#endif
