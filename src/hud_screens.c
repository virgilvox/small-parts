/* hud_screens.c: title, level card, pause, dead, clear and credits. Each is a
 * few painted layers (redrawn when a menu row or toggle changes) moved, faded
 * and flashed with GL, so the animation is free. Everything wears the same
 * shop-label look as the play HUD: ink plates, tape strips, part numbers. */
#include "hud_int.h"
#include "ds.h"
#include "hero.h"
#include "mission.h"

static layer_t L_logo, L_rule, L_hero, L_levels, L_sets, L_prompt, L_foot;
static layer_t L_ctl, L_pctl[2], L_card, L_ptitle, L_pmenu, L_dead, L_dprompt, L_clear, L_stamp, L_cprompt, L_cred[3], L_hold;

static float blink(float rate) { return 0.55f + 0.45f * sinf((float)ds_now() * rate); }

void scr_dim(float a) { hg_rect(0, 0, DS_W, DS_H, C_INK, a); hg_scan(0.1f); }

/* the top and bottom hazard stripes shared by the full screens */
static void edge_stripes(float a, float scroll) {
  hg_stripes(0, 0, DS_W, 14, C_HIVIS, 0.95f * a, C_INK, 0.95f * a, 40, scroll);
  hg_stripes(0, DS_H - 14, DS_W, 14, C_HIVIS, 0.95f * a, C_INK, 0.95f * a, 40, -scroll);
}

/* a blinking centred prompt in its own small layer; {ACTION} puts that control's glyph inline */
static void prompt(layer_t *L, bool pad, const char *s, float cy, float a, uint32_t col, float px) {
  cv *c = layer_begin(L, 1000, 70, hash_str(s, (uint32_t)px | (uint32_t)pad << 16));
  if (c) {
    float w = g_markup(c, 0, 0, CV_LEFT, F_DISPLAY, px, 4, col, px * 1.05f, pad, s, false);
    p_plate(c, 500 - w / 2 - 20, 4, w + 40, 62, 12, C_INK, 0.7f);
    g_markup(c, 500, 49, CV_CENTER, F_DISPLAY, px, 4, col, px * 1.05f, pad, s, true);
    layer_end(L, c);
  }
  hg_tex(L, 640 - 500, cy - 35, a);
}

/* a menu row: a plate that goes orange when selected */
static void row(cv *c, float x, float y, float w, float h, bool on, bool soft) {
  p_plate(c, x, y, w, h, 10, on ? C_HIVIS : C_INK, on ? 1.f : soft ? 0.7f : 0.86f);
  if (!on) { cv_fill_hex(c, C_CREAM, 0.14f); cv_fill_rect(c, x + 10, y + h - 2, w - 20, 1.5f); }
}

/* ── title ──
 * The name is a toy box's warning label: yellow, black rule, the triangle,
 * WARNING: CHOKING HAZARD over SMALL PARTS. The menus keep to the edges so
 * the attract reel (game.c title_reel) plays in the middle of the screen. */
#define C_LABEL 0xf3c623u
static void logo_paint(cv *c) {
  const float W = 640, Hh = 236;
  /* the label, die cut with clipped corners, a black keyline inside the edge */
  p_plate(c, 6, 8, W - 6, Hh - 8, 18, 0x000000, 0.45f);
  p_plate(c, 0, 0, W - 6, Hh - 8, 18, C_LABEL, 1);
  cv_stroke_hex(c, C_INK, 1); cv_line_width(c, 4);
  cv_begin(c); cv_move(c, 24, 10); cv_line(c, W - 32, 10); cv_line(c, W - 16, 26); cv_line(c, W - 16, Hh - 34);
  cv_line(c, W - 32, Hh - 18); cv_line(c, 24, Hh - 18); cv_line(c, 10, Hh - 32); cv_line(c, 10, 24); cv_close(c); cv_stroke(c);
  /* the warning triangle */
  cv_fill_hex(c, C_INK, 1);
  cv_begin(c); cv_move(c, 76, 28); cv_line(c, 124, 108); cv_line(c, 28, 108); cv_close(c); cv_fill(c);
  cv_fill_hex(c, C_LABEL, 1);
  cv_begin(c); cv_move(c, 76, 46); cv_line(c, 110, 100); cv_line(c, 42, 100); cv_close(c); cv_fill(c);
  cv_fill_hex(c, C_INK, 1);
  cv_fill_rect(c, 72, 58, 8, 26); cv_fill_rect(c, 72, 88, 8, 8);
  p_text(c, F_DISPLAY, 34, 3, CV_LEFT, "WARNING:", 142, 62, C_INK, 1);
  p_text(c, F_DISPLAY, 26, 3, CV_LEFT, "CHOKING HAZARD", 142, 100, C_INK, 1);
  /* the name, big, with the army green offset under the ink */
  cv_save(c);
  cv_font(c, F_DISPLAY, 92); cv_letter_spacing(c, 2);
  float nw = cv_measure(c, "SMALL PARTS");
  cv_font(c, F_DISPLAY, fminf(92, 92 * 572 / nw)); cv_text_align(c, CV_LEFT); cv_text_baseline(c, CV_ALPHABETIC);
  cv_fill_hex(c, C_ARMY, 1); cv_fill_text(c, "SMALL PARTS", 32, 190);
  cv_fill_hex(c, C_INK, 1); cv_fill_text(c, "SMALL PARTS", 26, 184);
  cv_restore(c);
  cv_fill_hex(c, C_INK, 1); cv_fill_rect(c, 26, 196, W - 60, 3);
  p_text(c, F_MONO, 13, 1, CV_LEFT, "NOT FOR CHILDREN UNDER 3 YRS.", 28, 214, C_INK, 1);
  p_text(c, F_MONO, 13, 1, CV_RIGHT, "2 HEROES. 500 ARMY MEN.", W - 36, 214, C_INK, 1);
  /* the 1:20 stamp */
  cv_save(c);
  cv_stroke_hex(c, 0xc8341a, 0.9f); cv_line_width(c, 3);
  cv_begin(c); cv_arc(c, W - 92, 64, 44, 0, 6.2831853f, false); cv_stroke(c);
  cv_begin(c); cv_arc(c, W - 92, 64, 37, 0, 6.2831853f, false); cv_stroke(c);
  cv_restore(c);
  p_text(c, F_DISPLAY, 26, 1, CV_CENTER, "1:20", W - 92, 72, 0xc8341a, 0.95f);
  p_text(c, F_MONO, 11, 1, CV_CENTER, "SCALE", W - 92, 44, 0xc8341a, 0.95f);
  p_text(c, F_MONO, 11, 1, CV_CENTER, "9 CM", W - 92, 96, 0xc8341a, 0.95f);
}

/* the ruler along the bottom: a steel rule in centimetres at the heroes' scale */
static void ruler(float a) {
  const float y = 640, x0 = 40, x1 = 1240;
  hg_rect(x0, y, x1 - x0, 22, 0xc9ccd1, 0.88f * a);
  hg_rect(x0, y, x1 - x0, 2, 0xffffff, 0.5f * a);
  for (int i = 0; x0 + i * 12 <= x1; i++) {
    float h = i % 10 == 0 ? 14 : i % 5 == 0 ? 10 : 6;
    hg_rect(x0 + i * 12, y + 22 - h, 1.5f, h, C_INK, 0.85f * a);
  }
}

static void title_bg(const hud_t *h) {
  float t = (float)ds_now();
  /* darken only behind the menus at the two edges: the reel plays clear in the middle */
  hg_grad(260, 360, 520, 720, 0, C_INK, 0.62f, 0.0f);
  hg_grad(1280 - 220, 360, 440, 720, 0, C_INK, 0.0f, 0.5f);
  hg_halftone(0, 560, DS_W, 160, C_ARMY, 0.22f, 13, false);
  edge_stripes(1, t * 0.08f);
  (void)h;
}

static const char *HERO_LINE[2] = {"BUILDER. TALKS FAST. ALWAYS MID-PROJECT.", "STEADY. DRY. READS THE MANUAL, THEN IGNORES IT."};
static const char *HERO_NAME[2] = {"MOHEEB", "LUIS"};

void scr_title(const hud_t *h, float dt) {
  (void)dt;
  title_bg(h);
  float t = h->t, t2 = (float)ds_now();
  cv *c = layer_begin(&L_logo, 640, 236, 2);
  if (c) { logo_paint(c); layer_end(&L_logo, c); }
  /* the label drops in and settles with a little swing, then hangs a degree off level */
  float drop = -320 * expf(-5.5f * t) * cosf(8.f * t);
  float m[6]; mat_place(m, 40 + 320, 30 + 118 + drop, -0.018f + 0.03f * expf(-3 * t) * sinf(9 * t), 320, 118, 1, 1);
  hg_tex_m(&L_logo, m, 1, 0xffffff);
  (void)t2;

  /* where the reel is: a camera tag top right, retyped when the scene changes */
  const char *place = h->title_place ? h->title_place : "";
  c = layer_begin(&L_rule, 420, 64, hash_str(place, 7));
  if (c) {
    p_tape(c, 0, 0, 22, F_MONO, 14, 2, "NOW SHOWING", C_CREAM, C_INK);
    p_plate(c, 0, 28, 420, 34, 8, C_INK, 0.85f);
    cv_fill_hex(c, C_ALERT, 1); cv_begin(c); cv_arc(c, 18, 45, 6, 0, 6.2831853f, false); cv_fill(c);
    p_text(c, F_DISPLAY, 18, 2, CV_LEFT, place, 34, 52, C_CREAM, 1);
    layer_end(&L_rule, c);
  }
  float sa = ds_clamp((t - 0.5f) / 0.4f, 0, 1);
  hg_tex(&L_rule, 1240 - 420, 40, sa * (0.85f + 0.15f * blink(3)));
  ruler(sa);

  int n = h->unlocked < 1 ? 1 : h->unlocked > MISSION_N ? MISSION_N : h->unlocked;
  int sel = h->menu_sel;
  float ma = ds_clamp((t - 0.8f) / 0.4f, 0, 1);
  int hero = h->hero < 0 || h->hero > 1 ? 0 : h->hero;

  /* the hero pick: a row of two chips with arrows when it is the selected row */
  uint32_t key = (uint32_t)hero | (uint32_t)(sel == 0) << 4;
  c = layer_begin(&L_hero, 420, 112, key);
  if (c) {
    bool on = sel == 0;
    p_tape(c, 0, 0, 20, F_MONO, 14, 2, "WHO ARE YOU", C_CREAM, C_INK);
    for (int i = 0; i < 2; i++) {
      float x = 26 + i * 192, y = 28;
      bool me = hero == i;
      p_plate(c, x, y, 178, 48, 10, me ? C_HIVIS : C_INK, me ? 1.f : 0.86f);
      p_text(c, F_DISPLAY, 24, 3, CV_CENTER, HERO_NAME[i], x + 89, y + 33, me ? C_INK : 0x8d93a3, 1);
      if (on && me) { cv_stroke_hex(c, C_CREAM, 1); cv_line_width(c, 3); cv_begin(c); cv_rect(c, x - 1.5f, y - 1.5f, 181, 51); cv_stroke(c); }
    }
    uint32_t ac = on ? C_HIVIS : 0x586070;   /* the left and right arrows */
    cv_fill_hex(c, ac, 1);
    cv_begin(c); cv_move(c, 18, 40); cv_line(c, 18, 64); cv_line(c, 4, 52); cv_close(c); cv_fill(c);
    cv_begin(c); cv_move(c, 402, 40); cv_line(c, 402, 64); cv_line(c, 416, 52); cv_close(c); cv_fill(c);
    p_text(c, F_MONO, 15, 0, CV_LEFT, HERO_LINE[hero], 26, 100, 0xd7d0bf, 1);
    layer_end(&L_hero, c);
  }
  float hx = 48 - (1 - hud_ease_out(ds_clamp((t - 0.6f) / 0.5f, 0, 1))) * 80;
  hg_tex(&L_hero, hx, 290, ma);

  /* the missions: one tape strip row per unlocked level */
  key = (uint32_t)sel | (uint32_t)n << 8 | (uint32_t)h->used_pad << 12;
  const float pitch = 44;
  c = layer_begin(&L_levels, 420, (int)(n * pitch + 34), key);
  if (c) {
    p_tape(c, 0, 0, 20, F_MONO, 14, 2, "MISSIONS", C_TAN, C_INK);
    for (int i = 0; i < n; i++) {
      bool on = sel == 1 + i;
      float y = 28 + i * pitch;
      row(c, 26, y, 370, 38, on, false);
      char b[8]; snprintf(b, sizeof b, "%02d", i + 1);
      p_text(c, F_MONO, 20, 0, CV_LEFT, b, 40, y + 26, on ? C_INK : C_HIVIS, 1);
      p_text(c, F_DISPLAY, 18, 2, CV_LEFT, mission_name(i), 80, y + 26, on ? C_INK : C_CREAM, 1);
      if (on) g_markup(c, 384, y + 26, CV_RIGHT, F_DISPLAY, 16, 0, C_INK, 24, h->used_pad, "{PICK}", true);
    }
    layer_end(&L_levels, c);
  }
  hg_tex(&L_levels, 48 - (1 - hud_ease_out(ds_clamp((t - 0.7f) / 0.5f, 0, 1))) * 80, 410, ma);

  /* settings on the right: music, shake, invert Y, view */
  key = (uint32_t)(sel - 1 - n + 8) | (uint32_t)h->music_on << 8 | (uint32_t)h->shake_on << 9 | (uint32_t)h->invert_y << 10 | (uint32_t)h->first_person << 11;
  c = layer_begin(&L_sets, 360, 244, key);
  if (c) {
    p_tape(c, 0, 0, 20, F_MONO, 14, 2, "SETTINGS", C_TEAL, C_INK);
    static const char *lab[4] = {"MUSIC", "SHAKE", "INVERT Y", "VIEW"};
    bool val[4] = {h->music_on, h->shake_on, h->invert_y, h->first_person};
    for (int k = 0; k < 4; k++) {
      bool on = sel == 1 + n + k;
      float y = 28 + k * 54;
      row(c, 0, y, 360, 46, on, false);
      p_text(c, F_DISPLAY, 18, 2, CV_LEFT, lab[k], 18, y + 31, on ? C_INK : C_CREAM, 1);
      const char *v = k == 3 ? (val[k] ? "FIRST PERSON" : "THIRD PERSON") : (val[k] ? "ON" : "OFF");
      uint32_t vc = on ? C_INK : (k == 3 || val[k]) ? C_GOO : 0x8d93a3;
      p_text(c, F_DISPLAY, 18, 2, CV_RIGHT, v, 342, y + 31, vc, 1);
    }
    layer_end(&L_sets, c);
  }
  hg_tex(&L_sets, 880 + (1 - hud_ease_out(ds_clamp((t - 0.7f) / 0.5f, 0, 1))) * 80, 334, ma);

  /* the one line of help for the selected row */
  const char *help = sel == 0 ? "{LEFTRIGHT} PICK   {PICK} CONFIRM"
                   : sel <= n ? "{UPDOWN} MOVE   {PICK} START"
                   : "{UPDOWN} MOVE   {PICK} CHANGE";
  c = layer_begin(&L_prompt, 700, 36, hash_str(help, 3 + (uint32_t)h->used_pad));
  if (c) {
    p_plate(c, 0, 0, 700, 36, 9, C_INK, 0.8f);
    g_markup(c, 350, 25, CV_CENTER, F_MONO, 18, 1, C_CREAM, 24, h->used_pad, help, true);
    layer_end(&L_prompt, c);
  }
  hg_tex(&L_prompt, 290, 596, ma * (0.75f + 0.25f * blink(5)));

  c = layer_begin(&L_foot, 1280, 30, 1);
  if (c) {
    p_text(c, F_MONO, 16, 1, CV_LEFT, "A HEATSYNC LABS GAME", 48, 22, C_CREAM, 0.85f);
    p_text(c, F_MONO, 16, 1, CV_RIGHT, "PLEASE RETURN THE SCALE TO 1:1 WHEN YOU ARE DONE", 1232, 22, C_CREAM, 0.85f);
    layer_end(&L_foot, c);
  }
  hg_tex(&L_foot, 0, 668, ma);
  hg_scan(0.07f);
}

/* ── level card ── */
void scr_intro(const hud_t *h) {
  float t = h->t;
  uint32_t key = hash_str(h->level_name, hash_str(h->level_blurb, (uint32_t)h->level + 90));
  cv *c = layer_begin(&L_card, 900, 270, key);
  if (c) {
    p_plate(c, 0, 0, 900, 270, 24, C_INK, 0.92f);
    p_hazard(c, 0, 0, 900, 12, 18, C_HIVIS, C_INK);
    p_hazard(c, 0, 258, 900, 12, 18, C_HIVIS, C_INK);
    char b[24]; snprintf(b, sizeof b, "LEVEL %02d OF %02d", h->level + 1, MISSION_N);
    p_tape(c, 44, 36, 28, F_MONO, 18, 3, b, C_CREAM, C_INK);
    char pn[24]; snprintf(pn, sizeof pn, "PN SP-%04d", (h->level + 1) * 108);
    p_text(c, F_MONO, 16, 2, CV_RIGHT, pn, 856, 56, 0x8d93a3, 1);
    const char *nm = h->level_name ? h->level_name : "UNLISTED";
    cv_font(c, F_DISPLAY, 70); cv_letter_spacing(c, 2);
    float w = cv_measure(c, nm), px = w > 810 ? 70 * 810 / w : 70;
    p_text(c, F_DISPLAY, px, 2, CV_LEFT, nm, 46, 150, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, px, 2, CV_LEFT, nm, 44, 146, C_CREAM, 1);
    cv_fill_hex(c, C_ARMY, 1); cv_fill_rect(c, 46, 168, 90, 6);
    cv_fill_hex(c, C_TAN, 1); cv_fill_rect(c, 140, 168, 40, 6);
    if (h->level_blurb) p_wrap(c, h->level_blurb, 46, 208, 800, 28, 0xd7d0bf);
    layer_end(&L_card, c);
  }
  float in = hud_ease_out(t / 0.5f), out = hud_ease_out((t - 2.8f) / 0.5f);
  float x = -940 + in * (940 + 190) - out * 1300;
  float a = 1 - ds_clamp((t - 3.0f) / 0.4f, 0, 1);
  hg_vgrad(380, 720, C_INK, 0.0f, 0.45f * (in - out > 0 ? 1.f : 0.f));
  hg_tex(&L_card, x, 100, a);
  /* the controls for the device in hand, under the card */
  c = layer_begin(&L_ctl, 900, 230, (uint32_t)h->used_pad + 5);
  if (c) { ctl_panel(c, 0, 0, 900, 30, h->used_pad, h->used_pad ? "CONTROLS  GAMEPAD" : "CONTROLS  KEYBOARD AND MOUSE"); layer_end(&L_ctl, c); }
  hg_tex(&L_ctl, x, 390, a);
  edge_stripes(a, t * 0.1f);
}

/* ── pause ── */
void scr_pause(const hud_t *h) {
  scr_dim(0.7f);
  edge_stripes(1, 0);
  cv *c = layer_begin(&L_ptitle, 560, 120, 1);
  if (c) {
    p_text(c, F_DISPLAY, 84, 2, CV_LEFT, "ON BREAK", 2, 88, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, 84, 2, CV_LEFT, "ON", 0, 84, C_HIVIS, 1);
    float w = 0; cv_font(c, F_DISPLAY, 84); cv_letter_spacing(c, 2); w = cv_measure(c, "ON ");
    p_text(c, F_DISPLAY, 84, 2, CV_LEFT, "BREAK", w, 84, C_CREAM, 1);
    p_cut(c, -4, 84 - 84 * 0.34f, 580, 3);
    cv_fill_hex(c, C_ARMY, 1); cv_fill_rect(c, 2, 104, 120, 6);
    layer_end(&L_ptitle, c);
  }
  hg_tex(&L_ptitle, 60, 36, 1);
  int sel = h->menu_sel;
  uint32_t key = (uint32_t)sel | (uint32_t)h->music_on << 8 | (uint32_t)h->shake_on << 9 | (uint32_t)h->invert_y << 10 | (uint32_t)h->used_pad << 11 | (uint32_t)h->first_person << 12;
  c = layer_begin(&L_pmenu, 520, 560, key);
  if (c) {
    static const char *rows[4] = {"RESUME", "RESTART LEVEL", "SETTINGS", "QUIT TO TITLE"};
    for (int i = 0; i < 4; i++) {
      bool on = sel == i || (i == 2 && sel >= 4);
      float y = i * 58;
      row(c, 0, y, 520, 50, on, false);
      p_text(c, F_DISPLAY, 24, 3, CV_LEFT, rows[i], 28, y + 35, on ? C_INK : C_CREAM, 1);
      if (sel == i) p_text(c, F_DISPLAY, 24, 0, CV_RIGHT, ">", 492, y + 35, C_INK, 1);
    }
    static const char *lab[4] = {"MUSIC", "SCREEN SHAKE", "INVERT Y", "VIEW"};
    bool val[4] = {h->music_on, h->shake_on, h->invert_y, h->first_person};
    for (int k = 0; k < 4; k++) {
      bool on = sel == 4 + k;
      float y = 244 + k * 48;
      p_plate(c, 40, y, 480, 40, 9, on ? C_TEAL : C_INK, on ? 1.f : 0.78f);
      p_text(c, F_DISPLAY, 18, 2, CV_LEFT, lab[k], 60, y + 27, on ? C_INK : C_CREAM, 1);
      const char *v = k == 3 ? (val[k] ? "FIRST PERSON" : "THIRD PERSON") : (val[k] ? "ON" : "OFF");
      p_text(c, F_DISPLAY, 18, 2, CV_RIGHT, v, 500, y + 27, on ? C_INK : (k == 3 || val[k]) ? C_GOO : 0x8d93a3, 1);
    }
    g_markup(c, 0, 478, CV_LEFT, F_MONO, 16, 0, 0xa0a6b6, 22, h->used_pad, "{UPDOWN} MOVE   {PICK} PICK   {BACK} BACK   {PAUSE} RESUME", true);
    layer_end(&L_pmenu, c);
  }
  hg_tex(&L_pmenu, 60, 170, 1);
  /* both layouts, side by side with the menu, so a pad player can see the keys and the other way round */
  for (int d = 0; d < 2; d++) {
    c = layer_begin(&L_pctl[d], 640, 250, (uint32_t)d + 9);
    if (c) { ctl_panel(c, 0, 0, 640, 32, d == 0, d == 0 ? "GAMEPAD" : "KEYBOARD AND MOUSE"); layer_end(&L_pctl[d], c); }
    hg_tex(&L_pctl[d], 600, 36 + d * 262 + (h->used_pad == (d == 0) ? 0 : 0), h->used_pad == (d == 0) ? 1.f : 0.82f);
  }
}

/* ── dead ── */
void scr_dead(const hud_t *h) {
  float k = ds_clamp(h->t / 0.7f, 0, 1);
  scr_dim(0.62f * k);
  hg_vignette(C_ALERT, 0.5f * k, 0.3f);
  const char *line = h->quip && h->quip[0] ? h->quip : "Small problem.";
  cv *c = layer_begin(&L_dead, 1000, 300, hash_str(line, 7));
  if (c) {
    p_text(c, F_DISPLAY, 128, 4, CV_CENTER, "SCRAPPED", 500, 140, 0x000000, 0.6f);
    p_text(c, F_DISPLAY, 128, 4, CV_CENTER, "SCRAPPED", 494, 134, C_CREAM, 1);
    p_cut(c, 0, 134 - 128 * 0.34f, 1000, 4);
    p_hazard(c, 200, 164, 600, 8, 16, C_ALERT, C_INK);
    cv_font(c, F_TYPE, 26);
    float tw = cv_measure(c, line), px = tw > 920 ? 26 * 920 / tw : 26;
    p_text(c, F_TYPE, px, 0, CV_CENTER, line, 500, 222, 0xe9dfc9, 1);
    layer_end(&L_dead, c);
  }
  float m[6]; float s = 1 + 0.3f * expf(-8 * h->t);
  mat_place(m, 640, 320, 0, 500, 150, s, s);
  hg_tex_m(&L_dead, m, k, 0xffffff);
  if (h->t > 1.0f) prompt(&L_dprompt, h->used_pad, "PRESS {PICK} TO RETRY", 520, blink(7), C_HIVIS, 40);
}

/* ── clear ── */
void scr_clear(const hud_t *h) {
  scr_dim(0.78f);
  hg_halftone(0, 360, DS_W, 360, C_ARMY, 0.22f, 13, false);
  edge_stripes(1, 0);
  char kb[24], tb[24], bt[24], sb[24], bb[24], lv[64];
  snprintf(kb, sizeof kb, "%d / %d", h->kills, h->kills_total);
  hud_fmt_time(tb, sizeof tb, h->clear_time_s);
  if (h->best_time_ms > 0) hud_fmt_time(bt, sizeof bt, h->best_time_ms / 1000.f); else snprintf(bt, sizeof bt, "NONE");
  hud_fmt_int(sb, sizeof sb, h->score); hud_fmt_int(bb, sizeof bb, h->best_score);
  snprintf(lv, sizeof lv, "LEVEL %02d  %s", h->level + 1, h->level_name ? h->level_name : "");
  uint32_t key = hash_str(kb, hash_str(tb, hash_str(bt, hash_str(sb, hash_str(bb, hash_str(lv, 5))))));
  cv *c = layer_begin(&L_clear, 900, 560, key);
  if (c) {
    p_tape(c, 4, 6, 30, F_DISPLAY, 20, 4, lv, C_HIVIS, C_INK);
    p_text(c, F_DISPLAY, 104, 2, CV_LEFT, "LEVEL CLEAR", 2, 150, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, 104, 2, CV_LEFT, "LEVEL CLEAR", 0, 144, C_CREAM, 1);
    p_cut(c, -4, 144 - 104 * 0.34f, 920, 4);
    p_text(c, F_MONO, 19, 0, CV_LEFT, "Bench left cleaner than we found it. Mostly.", 4, 182, 0xd7d0bf, 1);
    const char *lab[4] = {"ARMY MEN DOWN", "TIME", "SCORE", "BEST SCORE"};
    const char *val[4] = {kb, tb, sb, bb};
    for (int i = 0; i < 4; i++) {
      float y = 206 + i * 84;
      p_plate(c, 0, y, 760, 72, 14, C_INK, 0.86f);
      cv_fill_hex(c, i == 3 ? C_TAN : C_HIVIS, 1); cv_fill_rect(c, 0, y + 14, 8, 58);
      p_text(c, F_DISPLAY, 22, 5, CV_LEFT, lab[i], 32, y + 46, C_CREAM, 1);
      p_text(c, F_DISPLAY, 44, 1, CV_RIGHT, val[i], 736, y + 52, i == 3 ? C_TAN : C_CREAM, 1);
      if (i == 1) {
        char b[40]; snprintf(b, sizeof b, "BEST %s", bt);
        p_text(c, F_MONO, 16, 0, CV_LEFT, b, 130, y + 46, 0x8d93a3, 1);
      }
    }
    layer_end(&L_clear, c);
  }
  float rv[5] = {0.1f, 0.5f, 0.8f, 1.1f, 1.4f};
  float a0 = ds_clamp((h->t - rv[0]) / 0.3f, 0, 1);
  hg_sub(&L_clear, 0, 0, 900, 196, 190 - (1 - a0) * 60, 50, a0);
  for (int i = 0; i < 4; i++) {
    float a = ds_clamp((h->t - rv[i + 1]) / 0.3f, 0, 1);
    hg_sub(&L_clear, 0, 206 + i * 84, 760, 72, 190 + (1 - hud_ease_out(a)) * 220, 256 + i * 84, a);
  }
  if (h->new_best && h->t > 2.0f) {
    c = layer_begin(&L_stamp, 420, 120, 1);
    if (c) {
      cv_stroke_hex(c, C_HIVIS, 1); cv_line_width(c, 8); cv_begin(c); cv_round_rect(c, 14, 14, 392, 92, 10); cv_stroke(c);
      p_text(c, F_DISPLAY, 62, 4, CV_CENTER, "NEW BEST", 210, 82, C_HIVIS, 1);
      p_cut(c, 0, 60, 420, 3);
      layer_end(&L_stamp, c);
    }
    float k = h->t - 2.0f, s = 1 + 1.4f * expf(-12 * k);
    float m[6]; s *= 0.8f; mat_place(m, 1090, 330, -0.14f, 210, 60, s, s);
    hg_tex_m(&L_stamp, m, ds_clamp(k / 0.08f, 0, 1), 0xffffff);
  }
  if (h->t > 2.2f) prompt(&L_cprompt, h->used_pad, "PRESS {PICK} TO CONTINUE", 668, blink(7), C_HIVIS, 34);
}

/* ── credits ── */
void scr_end(const hud_t *h) {
  scr_dim(0.84f);
  hg_halftone(0, 0, DS_W, 300, C_ARMY, 0.2f, 13, true);
  /* '!' title, '#' tape heading, '@' subtitle, '~' mono small line, '^' print farm line */
  static const char *L[] = {
    "!SMALL PARTS", "@A HEATSYNC LABS GAME", "",
    "#STARRING", "MOHEEB AND LUIS", "AS THEMSELVES, NINE CENTIMETRES TALL", "",
    "#CHARACTERS", "Moheeb and Luis from COFFEE & KILL.", "",
    "#ARMY MEN", "Quaternius, CC0.", "Five hundred of them. Now very still.", "",
    "#THE LAB", "The HSL-108 model of HeatSync Labs,", "108 West Main, Mesa.", "",
    "#VOICES", "Moheeb and Luis, cloned from their own voices", "Kyutai Pocket TTS (CC BY 4.0)", "CMU Flite for the army", "",
    "#FONTS", "Archivo, IBM Plex Mono, JetBrains Mono,", "under the Open Font License", "",
    "#CODE", "MIT. Music and sound synthesised in code,", "no samples.", "",
    "@THANK YOU FOR PLAYING", "PLEASE RETURN THE SCALE TO 1:1 WHEN YOU ARE DONE.", "", "^ARMY_MEN_x500   62 PERCENT"};
  const int LAYER_H = 700;
  float total = 0;
  for (int pg = 0; pg < 3; pg++) {
    cv *c = layer_begin(&L_cred[pg], 960, LAYER_H, (uint32_t)pg + 1);
    float y = 40, base = 0;
    /* walk the whole list; paint the part that falls in this layer's slice of the roll */
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
      const char *s = L[i];
      float adv, ink_y;
      if (!*s) { adv = 16; ink_y = 0; }
      else if (*s == '!') { adv = 100; ink_y = 78; }
      else if (*s == '#') { adv = 44; ink_y = 22; }
      else if (*s == '@') { adv = 44; ink_y = 28; }
      else { adv = 34; ink_y = 22; }
      if (c && *s) {
        float ly = y - pg * LAYER_H;
        if (ly > -110 && ly < LAYER_H + 10) {
          if (*s == '!') p_text(c, F_DISPLAY, 92, 6, CV_CENTER, s + 1, 480, ly + ink_y, C_HIVIS, 1);
          else if (*s == '#') {
            cv_font(c, F_DISPLAY, 18); cv_letter_spacing(c, 4);
            p_tape(c, 480 - (cv_measure(c, s + 1) + 26 * 0.8f) / 2, ly, 26, F_DISPLAY, 18, 4, s + 1, C_CREAM, C_INK);
          }
          else if (*s == '@') p_text(c, F_DISPLAY, 28, 5, CV_CENTER, s + 1, 480, ly + ink_y, C_CREAM, 1);
          else if (*s == '^') p_text(c, F_MONO, 24, 2, CV_CENTER, s + 1, 480, ly + ink_y, C_TAN, 1);
          else p_text(c, F_TYPE, 24, 0, CV_CENTER, s, 480, ly + ink_y, 0xe9dfc9, 1);
        }
      }
      y += adv; base = y;
    }
    total = base;
    if (c) layer_end(&L_cred[pg], c);
  }
  float y = 720 - h->t * 58;
  float lim = 720 - total - 110;
  bool done = y <= lim;
  if (done) y = lim;   /* the roll stops with the last line on screen, the prompt below it */
  for (int pg = 0; pg < 3; pg++) if (y + pg * LAYER_H < DS_H && y + (pg + 1) * LAYER_H > 0) hg_tex(&L_cred[pg], 160, y + pg * LAYER_H, 1);
  edge_stripes(1, 0);
  if (done || h->t > 40) prompt(&L_hold, h->used_pad, "PRESS {PICK}", 676, blink(7), C_CREAM, 26);
}
