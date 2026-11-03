/* anim.h: the fighters' animation, three.js's AnimationMixer and
 * AnimationAction as the original uses them, over the baked clips of
 * assets/anim/rig_<a|b>.bin (the page's U0 table: per clip, every bone's
 * quaternion at 30 fps as int16, and the Hips position).
 *
 * three.c's own mixer has no fades, LoopOnce or clampWhenFinished, and the
 * game's timing rides on all three (a move ends when its clip pauses on its
 * last frame; crossfades decide what a hit looks like), so this file ports
 * those parts of three.js r186 line for line: _updateTime, _updateWeight,
 * _scheduleFading, PropertyMixer.accumulate / apply and Quaternion.slerpFlat. */
#ifndef CK_ANIM_H
#define CK_ANIM_H
#include <stdbool.h>
#include <stdint.h>
#include "three.h"

#define RIG_MAX_BONES 24
#define RIG_MAX_CLIPS 72

typedef struct { float peak, t, a0, a1; } ck_win;

typedef struct ck_clip {
  char name[40];
  float dur;
  int nf;
  ck_win hand, foot;
  float *times;      /* nf: min(dur, i / 30) */
  float *quats;      /* nf * nbones * 4, normalised (x, y, z, w) */
  float *hips;       /* nf * 3 */
} ck_clip;

typedef struct ck_rig {
  int nb;
  char bones[RIG_MAX_BONES][24];
  int nclips;
  ck_clip clips[RIG_MAX_CLIPS];
} ck_rig;

ck_rig *rig_load(const char *asset);
int rig_clip_index(const ck_rig *r, const char *name);   /* -1 if none */

enum { LOOP_ONCE = 2200, LOOP_REPEAT = 2201 };

typedef struct ck_action {
  int clip;              /* index into the rig */
  float time, time_scale, weight;
  bool enabled, paused, clamp_when_finished;
  int loop;              /* LOOP_ONCE / LOOP_REPEAT (repetitions Infinity) */
  int loop_count;
  /* the weight interpolant of _scheduleFading (null when !fading) */
  bool fading; double f_t0, f_t1; float f_v0, f_v1;
  float effective_weight;
  int active_index;      /* position in the mixer's active list, -1 = inactive */
} ck_action;

typedef struct ck_mixer {
  const ck_rig *rig;
  double time;
  float time_scale;
  ck_action actions[RIG_MAX_CLIPS];
  bool made[RIG_MAX_CLIPS];
  int active[RIG_MAX_CLIPS]; int nactive;
  /* the bindings: one PropertyMixer per bone quaternion, one for Hips.position */
  t3_object *bone[RIG_MAX_BONES]; t3_object *hips;
  float orig_q[RIG_MAX_BONES][4], orig_p[3];
  float acc_q[RIG_MAX_BONES][4], acc_p[3];
  float cum_q[RIG_MAX_BONES], cum_p;
  bool bound;
} ck_mixer;

void mixer_init(ck_mixer *m, const ck_rig *rig, t3_object *model);
/* mixer.clipAction(clip): the clip's action (made on first use) */
ck_action *mixer_action(ck_mixer *m, int clip);
void mixer_update(ck_mixer *m, float dt);
void mixer_stop_all(ck_mixer *m);

/* AnimationAction methods */
void action_play(ck_mixer *m, ck_action *a);
void action_stop(ck_mixer *m, ck_action *a);
void action_reset(ck_action *a);
void action_set_effective_weight(ck_action *a, float w);
void action_fade_in(ck_mixer *m, ck_action *a, float d);
void action_fade_out(ck_mixer *m, ck_action *a, float d);
void action_cross_fade_from(ck_mixer *m, ck_action *a, ck_action *from, float d);
static inline float action_duration(const ck_mixer *m, const ck_action *a) { return m->rig->clips[a->clip].dur; }
static inline const char *action_name(const ck_mixer *m, const ck_action *a) { return m->rig->clips[a->clip].name; }

#endif
