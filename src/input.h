/* input.h: one player's intent each frame, from any pad and the keyboard.
 *
 * Pad (standard mapping): left stick moves, right stick aims, A jumps (held
 * in the air: the jetpack), RT fires, LT aims (the coilgun's scope; d-pad up
 * and down or LB / RB zoom while scoped), B dodges, X swings the soldering
 * iron, Y throws a LiPo, LB / RB or the d-pad left and right change weapon,
 * SELECT (back / view) swaps first and third person, START pauses.
 *
 * Keyboard: WASD moves, arrows aim, Space jumps and jets, J, F or Left Ctrl
 * fires, K aims, Shift dodges, E swings the iron, Q or G throws a LiPo, 1 to 4
 * pick a weapon, Tab is the next one, R and V zoom while scoped, C swaps the
 * view, Enter, Esc or P pauses. The web page turns a locked mouse into the
 * right stick, left click into RT, right click into LT and the wheel into
 * LB / RB, so the cart only ever sees pads and keys. */
#ifndef DS_INPUT_H
#define DS_INPUT_H
#include <stdbool.h>
#include <stdint.h>
#include "wasmcart.h"

#define DS_KEY_LSHIFT 0xE1
#define DS_KEY_RSHIFT 0xE5

typedef struct {
  float mx, my;            /* move, -1..1 (y +1 = forward) */
  float ax, ay;            /* aim rate, -1..1 (y +1 = up) */
  bool jump, jump_held;    /* jump: pressed this frame */
  bool fire, fire_held, fire_released;
  bool aim;                /* held */
  int zoom;                /* -1 / +1 when a zoom step was asked this frame (only while aiming) */
  bool dodge, melee, lipo;
  bool next, prev;         /* weapon cycle (not while aiming) */
  int select;              /* -1, or a weapon slot picked with 1 to 4 */
  bool view;               /* swap first / third person */
  bool pause;
  /* menus */
  int nav_x, nav_y;        /* edges, y +1 = down */
  bool ok, back;
  bool used_pad;           /* the last thing touched was a pad (for on-screen prompts) */
} intent_t;

void input_frame(const wc_pad_t *pads, const uint8_t *keys);
const intent_t *input_get(void);
bool input_key_down(int code);
bool input_start_held(void);   /* START is down on any pad (cutscene skip hold) */

#endif
