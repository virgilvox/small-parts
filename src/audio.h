/* audio.h: the synth. Effects are fire-and-forget; music follows a mood. */
#ifndef SP_AUDIO_H
#define SP_AUDIO_H
#include <stdint.h>

typedef enum {
  SFX_BAND,         /* rubber band repeater: a twang and a snap */
  SFX_GLUE,         /* hot glue gun: a wet pop */
  SFX_COIL_CHARGE,  /* a rising whine (call every frame while charging, gain = charge) */
  SFX_COIL,         /* coilgun release: a crack and a ring */
  SFX_IRON,         /* soldering iron swing: a hiss */
  SFX_MELT,         /* plastic meets the iron */
  SFX_LIPO_THROW, SFX_LIPO_PUFF, SFX_BOOM,
  SFX_PELLET,       /* an army rifle: a tiny click-pop */
  SFX_MG,           /* the gunner's rattle (one per shot) */
  SFX_PLINK,        /* a hit on plastic */
  SFX_TOPPLE,       /* an army man falls over: a hollow clack */
  SFX_HURT,
  SFX_JUMP, SFX_LAND, SFX_DODGE,
  SFX_JET,          /* the jetpack's motors (call every frame while thrusting, gain = thrust) */
  SFX_JET_EMPTY,
  SFX_PICKUP, SFX_WEAPON, SFX_PRINT_STOP,
  SFX_WHISTLE,      /* the army's whistle: a squad spotted you */
  SFX_UI, SFX_UI_OK,
  SFX_BOSS_HIT, SFX_ALARM,
  SFX_COUNT
} sfx_t;

typedef enum { MOOD_TITLE, MOOD_EXPLORE, MOOD_FIGHT, MOOD_BOSS, MOOD_CLEAR, MOOD_CINE, MOOD_SILENT } mood_t;

void audio_init(uint32_t rate);
void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double ms);
/* pan -1..1, gain 0..1 */
void sfx(sfx_t s, float pan, float gain);
/* a sound at a place in the lab, panned and attenuated from the listener (game.c sets it each frame) */
void sfx_at(sfx_t s, float x, float y, float z, float gain);
void audio_listener(float x, float y, float z, float yaw);
void music_mood(mood_t m);
void audio_set_music(int on);

#endif
