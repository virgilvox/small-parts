/* input.c: see input.h. One player: every connected pad and the keyboard
 * drive them; the strongest stick wins and buttons are or'd. Two players:
 * the second pad drives player 2 only, everything else player 1. */
#include "input.h"
#include "ds.h"
#include <string.h>

static uint8_t kcur[32], kprev[32];
static wc_pad_t pcur[4], pprev[4];
static intent_t I, I2, IM;
static int nav_ox[2], nav_oy[2];
static bool last_pad;
static bool fire_was[2];
static bool coop, join_open, join_edge;
static bool seen[4], flagged[4];   /* a pad that has pressed something, and one that has reported connected */
static int pad2 = -1;

#define DOWN(k, c) (((k)[(c) >> 3] >> ((c) & 7)) & 1)

bool input_key_down(int c) { return c > 0 && c < 256 && DOWN(kcur, c); }
static bool kp(int c) { return DOWN(kcur, c) && !DOWN(kprev, c); }
static bool any_k(const int *c, int n) { for (int i = 0; i < n; i++) if (DOWN(kcur, c[i])) return true; return false; }
static bool any_kp(const int *c, int n) { for (int i = 0; i < n; i++) if (kp(c[i])) return true; return false; }

static float stick(int16_t v) {
  float f = v / 32767.f, dz = 0.2f;
  if (fabsf(f) < dz) return 0;
  return ds_sign(f) * fminf(1, (fabsf(f) - dz) / (1 - dz));
}

/* a pad is there when it says it is connected, or (a host that flags only
 * port 0, as the station's core may) once it has pressed something; one
 * that reported connected and stops is gone */
static bool pad_live(int p) { return pcur[p].connected || (seen[p] && !flagged[p]); }

/* one intent from the pads in mask (bit per pad), plus the keyboard when keys */
static void read_intent(intent_t *o, unsigned mask, bool keys, int slot) {
  memset(o, 0, sizeof *o);
  static const int K_FIRE[] = {WC_KEY_J, WC_KEY_F, WC_KEY_LCTRL}, K_AIM[] = {WC_KEY_K},
                   K_DODGE[] = {DS_KEY_LSHIFT, DS_KEY_RSHIFT}, K_JUMP[] = {WC_KEY_SPACE},
                   K_OK[] = {WC_KEY_ENTER, WC_KEY_SPACE, WC_KEY_J}, K_BACK[] = {WC_KEY_ESCAPE, WC_KEY_BACKSPACE};
  o->select = -1;
  bool zin = false, zout = false, cyc_n = false, cyc_p = false;
  int dx = 0, dy = 0;
  if (keys) {
    o->mx = (DOWN(kcur, WC_KEY_D) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_A) ? 1.f : 0.f);
    o->my = (DOWN(kcur, WC_KEY_W) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_S) ? 1.f : 0.f);
    o->ax = (DOWN(kcur, WC_KEY_RIGHT) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_LEFT) ? 1.f : 0.f);
    o->ay = (DOWN(kcur, WC_KEY_UP) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_DOWN) ? 1.f : 0.f);
    o->jump = any_kp(K_JUMP, 1); o->jump_held = any_k(K_JUMP, 1);
    o->fire = any_kp(K_FIRE, 3); o->fire_held = any_k(K_FIRE, 3);
    o->aim = any_k(K_AIM, 1);
    zin = kp(WC_KEY_R) || kp(WC_KEY_EQUAL); zout = kp(WC_KEY_V) || kp(WC_KEY_MINUS);
    cyc_n = kp(WC_KEY_TAB);
    o->dodge = any_kp(K_DODGE, 2); o->melee = kp(WC_KEY_E); o->lipo = kp(WC_KEY_Q) || kp(WC_KEY_G);
    for (int k = 0; k < 4; k++) if (kp(WC_KEY_1 + k)) o->select = k;
    o->view = kp(WC_KEY_C);
    o->pause = kp(WC_KEY_ESCAPE) || kp(WC_KEY_ENTER) || kp(WC_KEY_P);
    o->ok = any_kp(K_OK, 3); o->back = any_kp(K_BACK, 2);
    dx = (DOWN(kcur, WC_KEY_D) || DOWN(kcur, WC_KEY_RIGHT)) - (DOWN(kcur, WC_KEY_A) || DOWN(kcur, WC_KEY_LEFT));
    dy = (DOWN(kcur, WC_KEY_S) || DOWN(kcur, WC_KEY_DOWN)) - (DOWN(kcur, WC_KEY_W) || DOWN(kcur, WC_KEY_UP));
  }

  for (int p = 0; p < 4; p++) {
    const wc_pad_t *n = &pcur[p], *o_ = &pprev[p];
    if (!(mask >> p & 1) || !pad_live(p)) continue;
    uint32_t b = n->buttons, pb = o_->buttons, pr = b & ~pb;
    float lx = stick(n->left_x), ly = -stick(n->left_y), rx = stick(n->right_x), ry = -stick(n->right_y);
    bool lt = n->left_trigger > WC_TRIGGER_PRESSED, rt = n->right_trigger > WC_TRIGGER_PRESSED;
    bool plt = o_->left_trigger > WC_TRIGGER_PRESSED, prt = o_->right_trigger > WC_TRIGGER_PRESSED;
    if (slot == 0 && (pr || (rt && !prt) || (lt && !plt) || fabsf(lx) + fabsf(ly) + fabsf(rx) + fabsf(ry) > 0.3f)) last_pad = true;
    if (fabsf(lx) > fabsf(o->mx)) o->mx = lx;
    if (fabsf(ly) > fabsf(o->my)) o->my = ly;
    if (fabsf(rx) > fabsf(o->ax)) o->ax = rx;
    if (fabsf(ry) > fabsf(o->ay)) o->ay = ry;
    o->jump |= (pr & WC_BTN_A) != 0; o->jump_held |= (b & WC_BTN_A) != 0;
    o->fire |= rt && !prt; o->fire_held |= rt;
    o->aim |= lt;
    zin |= (pr & WC_BTN_UP) != 0; zout |= (pr & WC_BTN_DOWN) != 0;
    if (lt) { zin |= (pr & WC_BTN_R) != 0; zout |= (pr & WC_BTN_L) != 0; }
    else { cyc_n |= (pr & WC_BTN_R) != 0; cyc_p |= (pr & WC_BTN_L) != 0; }
    cyc_n |= (pr & WC_BTN_RIGHT) != 0; cyc_p |= (pr & WC_BTN_LEFT) != 0;
    o->dodge |= (pr & WC_BTN_B) != 0; o->melee |= (pr & WC_BTN_X) != 0; o->lipo |= (pr & WC_BTN_Y) != 0;
    o->view |= (pr & (WC_BTN_SELECT | WC_BTN_R3)) != 0;
    o->pause |= (pr & WC_BTN_START) != 0;
    o->ok |= (pr & (WC_BTN_A | WC_BTN_START)) != 0; o->back |= (pr & WC_BTN_B) != 0;
    int px = (b & WC_BTN_RIGHT || n->left_x > 16000) - (b & WC_BTN_LEFT || n->left_x < -16000);
    int py = (b & WC_BTN_DOWN || n->left_y > 16000) - (b & WC_BTN_UP || n->left_y < -16000);
    if (px) dx = px;
    if (py) dy = py;
  }
  if (o->aim) { o->zoom = zin ? 1 : zout ? -1 : 0; }
  else { o->next = cyc_n; o->prev = cyc_p; }
  o->fire_released = fire_was[slot] && !o->fire_held;
  fire_was[slot] = o->fire_held;
  o->nav_x = dx != nav_ox[slot] ? dx : 0; o->nav_y = dy != nav_oy[slot] ? dy : 0;
  nav_ox[slot] = dx; nav_oy[slot] = dy;
  o->used_pad = slot == 0 ? last_pad : true;
}

void input_frame(const wc_pad_t *pads, const uint8_t *keys) {
  memcpy(kprev, kcur, 32); memcpy(kcur, keys, 32);
  memcpy(pprev, pcur, sizeof pcur); memcpy(pcur, pads, sizeof pcur);
  bool any_key = false;
  for (int i = 0; i < 32; i++) if (kcur[i] & ~kprev[i]) any_key = true;
  if (any_key) last_pad = false;
  for (int p = 0; p < 4; p++) {
    if (pcur[p].connected) { flagged[p] = true; seen[p] = true; }
    else if (flagged[p]) flagged[p] = seen[p] = false;   /* unplugged */
    if (pcur[p].buttons & ~pprev[p].buttons) seen[p] = true;
  }
  /* player 1's pad is the first one there, player 2's the next */
  int pad1 = -1; pad2 = -1;
  for (int p = 0; p < 4; p++) if (pad_live(p)) { if (pad1 < 0) pad1 = p; else if (pad2 < 0) pad2 = p; }
  uint32_t pr2 = pad2 >= 0 ? pcur[pad2].buttons & ~pprev[pad2].buttons : 0;
  join_edge = !coop && join_open && (pr2 & (WC_BTN_START | WC_BTN_A)) != 0;
  /* two players, or the second pad's join press: that pad is not player 1's */
  unsigned mask1 = 15u;
  if (pad2 >= 0 && (coop || join_edge)) mask1 &= ~(1u << pad2);
  read_intent(&I, mask1, true, 0);
  if (coop && pad2 >= 0) read_intent(&I2, 1u << pad2, false, 1);
  else { memset(&I2, 0, sizeof I2); I2.select = -1; fire_was[1] = false; }
  /* menus: player 1, and in two-player mode the second pad's picks too */
  IM = I;
  if (coop) {
    if (!IM.nav_x) IM.nav_x = I2.nav_x;
    if (!IM.nav_y) IM.nav_y = I2.nav_y;
    IM.ok |= I2.ok; IM.back |= I2.back; IM.pause |= I2.pause;
  }
}

const intent_t *input_get(void) { return &I; }
const intent_t *input_get_player(int p) { return p == 1 ? &I2 : &I; }
const intent_t *input_get_menu(void) { return &IM; }
void input_set_coop(bool on) { coop = on; }
void input_set_join_open(bool on) { join_open = on; }
bool input_join(void) { return join_edge; }
bool input_pad2(void) { return pad2 >= 0; }
bool input_start_held(void) {
  for (int p = 0; p < 4; p++) if (pcur[p].connected && (pcur[p].buttons & WC_BTN_START)) return true;
  return false;
}
