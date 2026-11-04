/* hud.c: the play HUD and the coilgun's scope. Static plates and text are
 * painted once into layers (hud_gl.c) and redrawn only when their numbers
 * change; bars, battery cells, reticles, rings and the scope mask are GL
 * shapes, so a play frame costs a couple of dozen quads and almost no CPU
 * paint. The look is a shop label: ink plates, strips of label-maker tape,
 * part numbers in monospace, HeatSync orange. The other screens live in
 * hud_screens.c, the cutscene layer and the radio in hud_cine.c. */
#include "hud_int.h"
#include "game.h"
#include "weapon.h"
#include <stdlib.h>

static float now_t, dt_;

/* ── layers ── */
static layer_t L_h1, L_h2, L_h3, L_prompt, L_vit, L_down, L_wpn, L_obj, L_stat, L_boss, L_banner, L_hints, L_arc;
static layer_t L_ret[3], L_brk, L_weak, L_zl, L_zr, L_mark, L_marrow, L_mdist;

/* smoothed values, so bars glide and damage leaves a trail */
static float s_health, s_trail, s_buddy, s_boss, s_boss_trail, s_jet;
static double banner_t0; static uint32_t banner_key;
static double scope_t0; static bool was_scoped;

/* the four weapon slots, short labels for the chips */
static const char *SLOT[WP_COUNT] = {"BAND", "GLUE", "COIL", "IRON"};
static const char *AMMO_LABEL[WP_COUNT] = {"", "GLUE STICKS", "CHARGE CELLS", ""};

/* ── reusable bits ── */
/* a bar: dark well, a pale trail behind the value, a highlight along the top */
static void bar(float x, float y, float w, float h, float v, float trail, uint32_t col, uint32_t trail_col) {
  hg_rect(x - 2, y - 2, w + 4, h + 4, C_CREAM, 0.9f);
  hg_rect(x, y, w, h, 0x07080c, 1);
  hg_rect(x, y, w * ds_clamp(trail, 0, 1), h, trail_col, 0.85f);
  hg_rect(x, y, w * ds_clamp(v, 0, 1), h, col, 1);
  hg_rect(x, y, w * ds_clamp(v, 0, 1), h * 0.32f, 0xffffff, 0.18f);
}
/* ruler ticks over a bar, painted once per size */
static layer_t L_segs[3];
static void segs(int slot, float x, float y, float w, float h, float cell) {
  cv *c = layer_begin(&L_segs[slot], (int)w, (int)h, (uint32_t)(w * 10 + h));
  if (c) {
    cv_fill_hex(c, C_INK, 1);
    for (float sx = cell; sx < w - 1; sx += cell) cv_fill_rect(c, sx - 1.5f, 0, 3, h);
    layer_end(&L_segs[slot], c);
  }
  hg_tex(&L_segs[slot], x, y, 1);
}

/* a battery: cream shell, a nub, five cells that drain from the right; the last cell fades as it goes */
static void battery(float x, float y, float w, float h, float v, uint32_t col, uint32_t shell, float glow) {
  const int N = 5; const float t = 3, pad = 4;
  if (glow > 0) hg_rect(x - 5, y - 5, w + 10 + 8, h + 10, col, 0.28f * glow);
  hg_rect(x, y, w, h, shell, 1);
  hg_rect(x + w, y + h * 0.28f, 7, h * 0.44f, shell, 1);
  hg_rect(x + t, y + t, w - 2 * t, h - 2 * t, 0x07080c, 1);
  float cw = (w - 2 * t - pad * (N + 1)) / N;
  for (int i = 0; i < N; i++) {
    float lvl = ds_clamp(v * N - i, 0, 1);
    float cx = x + t + pad + i * (cw + pad);
    hg_rect(cx, y + t + pad, cw, h - 2 * t - 2 * pad, 0x1d212c, 1);
    if (lvl > 0) hg_rect(cx, y + t + pad, cw, h - 2 * t - 2 * pad, col, 0.35f + 0.65f * lvl);
  }
}

/* ── play ── */
static void play_vitals(const hud_t *h) {
  const float x = 40, y = 568, w = 408, hh = 112;
  uint32_t key = hash_str(h->hero_name, hash_str(h->buddy_name, (uint32_t)h->used_pad + 1));
  cv *c = layer_begin(&L_vit, (int)w, (int)hh, key);
  if (c) {
    p_plate(c, 0, 0, w, hh, 16, C_INK, 0.86f);
    p_hazard(c, 0, 16, 9, hh - 16, 14, C_HIVIS, C_INK);
    p_tape(c, 24, 8, 22, F_DISPLAY, 14, 3, h->hero_name ? h->hero_name : "MOHEEB", C_CREAM, C_INK);
    p_tape(c, 24, 72, 18, F_DISPLAY, 11, 2, h->buddy_name ? h->buddy_name : "LUIS", C_TAN, C_INK);
    p_tape(c, 296, 8, 22, F_DISPLAY, 14, 3, "JET", C_TEAL, C_INK);
    g_markup(c, 296, 103, CV_LEFT, F_MONO, 14, 0, 0x9aa1b2, 19, h->used_pad, "HOLD {JUMP}", true);
    layer_end(&L_vit, c);
  }
  hg_tex(&L_vit, x, y, 1);
  float t = (float)(now_t * 9);
  bool low = s_health < 0.28f;
  uint32_t hc = low ? (sinf(t) > 0 ? C_ALERT : 0xff8a7a) : C_HIVIS;
  bar(x + 24, y + 52, 258, 14, s_health, s_trail, hc, C_CREAM);
  segs(0, x + 24, y + 52, 258, 14, 25.8f);
  /* the buddy: a green bar, or DOWN while they are on the floor */
  bool down = h->buddy_health <= 0.001f;
  if (down) {
    cv *d = layer_begin(&L_down, 120, 20, 1);
    if (d) {
      p_plate(d, 0, 0, 120, 20, 5, C_ALERT, 1);
      p_text(d, F_DISPLAY, 13, 3, CV_CENTER, "DOWN", 60, 15, C_CREAM, 1);
      layer_end(&L_down, d);
    }
    hg_tex(&L_down, x + 130, y + 71, sinf(t * 0.7f) > -0.2f ? 1.f : 0.45f);
  } else {
    bar(x + 130, y + 76, 152, 10, s_buddy, s_buddy, C_TAN, C_TAN);
  }
  /* jetpack battery: teal, flashing red when low, a glow while thrusting */
  bool jlow = s_jet < 0.25f;
  bool flash = jlow && sinf(t * 1.3f) > 0;
  uint32_t jc = jlow ? C_ALERT : C_TEAL;
  battery(x + 296, y + 38, 88, 46, s_jet, jc, flash ? C_ALERT : C_CREAM, h->jetting ? 0.6f + 0.4f * sinf(t * 2) : 0);
}

static void play_weapon(const hud_t *h) {
  const float x = 888, y = 568, w = 352, hh = 112;
  int own = 0; for (int i = 0; i < 4; i++) own |= h->owned[i] << i;
  uint32_t key = (uint32_t)h->weapon | (uint32_t)(h->ammo + 1) << 4 | (uint32_t)(h->lipos & 15) << 16 | (uint32_t)own << 20 | (uint32_t)h->used_pad << 24;
  key = hash_str(h->weapon_name, key);
  cv *c = layer_begin(&L_wpn, (int)w, (int)hh, key);
  if (c) {
    p_plate(c, 0, 0, w, hh, 16, C_INK, 0.86f);
    p_hazard(c, w - 9, 0, 9, hh - 16, 14, C_HIVIS, C_INK);
    p_tape(c, 16, 8, 22, F_DISPLAY, 15, 2, h->weapon_name ? h->weapon_name : "WEAPON", C_HIVIS, C_INK);
    for (int i = 0; i < WP_COUNT; i++) {
      float sx = 16 + i * 80, sy = 44, sw = 74, sh = 30;
      bool sel = h->weapon == i, own_i = h->owned[i];
      cv_fill_hex(c, sel ? C_CREAM : own_i ? 0x2a2f3c : 0x161922, 1); cv_fill_rect(c, sx, sy, sw, sh);
      uint32_t ink = sel ? C_INK : own_i ? C_CREAM : 0x4a5062;
      char k[2] = {(char)('1' + i), 0};
      p_text(c, F_MONO, 14, 0, CV_LEFT, k, sx + 6, sy + 20, sel ? C_HIVIS : own_i ? C_HIVIS : 0x4a5062, 1);
      p_text(c, F_DISPLAY, 13, 1, CV_LEFT, own_i ? SLOT[i] : "?", sx + 20, sy + 20, ink, 1);
      if (own_i && !sel) { cv_fill_hex(c, C_HIVIS, 1); cv_fill_rect(c, sx, sy + sh - 3, sw, 3); }
    }
    /* ammo, or endless */
    if (h->ammo < 0) {
      p_text(c, F_DISPLAY, 22, 2, CV_LEFT, "ENDLESS", 16, 104, C_CREAM, 1);
    } else {
      char b[16]; snprintf(b, sizeof b, "%02d", h->ammo);
      p_text(c, F_DISPLAY, 34, 1, CV_LEFT, b, 16, 106, h->ammo ? C_CREAM : C_ALERT, 1);
      p_text(c, F_MONO, 13, 0, CV_LEFT, AMMO_LABEL[h->weapon >= 0 && h->weapon < WP_COUNT ? h->weapon : 0], 70, 102, 0x9aa1b2, 1);
    }
    /* LiPo packs: little orange pouches */
    g_markup(c, w - 20, 88, CV_RIGHT, F_MONO, 13, 0, 0x9aa1b2, 18, h->used_pad, "{LIPO}", true);
    int n = h->lipos < 5 ? h->lipos : 5;
    for (int k = 0; k < 5; k++) {
      float px_ = w - 24 - 5 * 20 + k * 20, py = 94;
      bool on = k < n;
      if (on) {
        cv_fill_hex(c, C_HIVIS, 1); cv_fill_rect(c, px_, py + 3, 15, 13);
        cv_fill_hex(c, C_INK, 1); cv_fill_rect(c, px_ + 3, py, 9, 4); cv_fill_rect(c, px_ + 6, py + 7, 3, 5);
      } else { cv_stroke_hex(c, 0x586070, 1); cv_line_width(c, 1.5f); cv_begin(c); cv_rect(c, px_ + 0.75f, py + 3.75f, 13.5f, 11.5f); cv_stroke(c); }
    }
    if (h->lipos > 5) { char b[8]; snprintf(b, sizeof b, "+%d", h->lipos - 5); p_text(c, F_MONO, 13, 0, CV_RIGHT, b, w - 124 - 4, 108, C_HIVIS, 1); }
    layer_end(&L_wpn, c);
  }
  hg_tex(&L_wpn, x, y, 1);
}

static void play_objective(const hud_t *h) {
  if (!h->objective || !h->objective[0]) return;
  uint32_t k = hash_str(h->objective, 31);
  static float w_obj = 200;
  cv *c = layer_begin(&L_obj, 560, 66, k);
  if (c) {
    cv_font(c, F_DISPLAY, 22); cv_letter_spacing(c, 2);
    w_obj = fminf(cv_measure(c, h->objective) + 56, 560);
    p_plate(c, 0, 0, w_obj, 66, 14, C_INK, 0.86f);
    p_hazard(c, 0, 14, 9, 52, 14, C_HIVIS, C_INK);
    p_tape(c, 24, 8, 18, F_MONO, 13, 2, "OBJECTIVE", C_CREAM, C_INK);
    p_text(c, F_DISPLAY, 22, 2, CV_LEFT, h->objective, 24, 56, C_CREAM, 1);
    layer_end(&L_obj, c);
  }
  hg_tex(&L_obj, h->split ? 640 - w_obj / 2 : 40, h->split ? 24 : 30, 1);
}

static void play_stats(const hud_t *h) {
  char tb[24]; hud_fmt_time(tb, sizeof tb, h->time_s);
  char sb[24]; hud_fmt_int(sb, sizeof sb, h->score);
  uint32_t key = hash_str(tb, hash_str(sb, (uint32_t)h->level + 3));
  cv *c = layer_begin(&L_stat, 280, 66, key);
  if (c) {
    p_plate(c, 0, 0, 280, 66, 14, C_INK, 0.86f);
    char lv[24]; snprintf(lv, sizeof lv, "LEVEL %02d", h->level + 1);
    p_tape(c, 16, 8, 18, F_MONO, 13, 2, lv, C_TAN, C_INK);
    p_text(c, F_MONO, 26, 0, CV_RIGHT, tb, 264, 31, C_CREAM, 1);
    p_text(c, F_MONO, 15, 1, CV_LEFT, "SCORE", 16, 56, 0x8d93a3, 1);
    p_text(c, F_DISPLAY, 20, 1, CV_RIGHT, sb, 264, 58, C_HIVIS, 1);
    layer_end(&L_stat, c);
  }
  hg_tex(&L_stat, 1280 - 40 - 280, 30, 1);
}

static void play_boss(const hud_t *h) {
  const float x = 360, y = 104, w = 560;
  int ph = h->boss_phase < 1 ? 1 : h->boss_phase;
  cv *c = layer_begin(&L_boss, (int)w, 82, hash_str(h->boss_name, (uint32_t)ph * 31));
  if (c) {
    p_plate(c, 0, 0, w, 82, 16, C_INK, 0.9f);
    p_hazard(c, 0, 0, w, 7, 14, C_ALERT, C_INK);
    p_text(c, F_DISPLAY, 24, 3, CV_LEFT, h->boss_name && h->boss_name[0] ? h->boss_name : "THE ARMY", 20, 38, C_CREAM, 1);
    char b[16]; snprintf(b, sizeof b, "PHASE %d", ph);
    p_text(c, F_DISPLAY, 16, 3, CV_RIGHT, b, w - 20, 36, C_ALERT, 1);
    cv_fill_hex(c, 0x07080c, 1); cv_fill_rect(c, 20, 48, w - 40, 22);
    layer_end(&L_boss, c);
  }
  hg_tex(&L_boss, x, y, 1);
  bar(x + 20, y + 48, w - 40, 22, s_boss, s_boss_trail, C_ALERT, C_CREAM);
  for (int i = 1; i < 3; i++) hg_rect(x + 20 + (w - 40) * i / 3 - 1.5f, y + 48, 3, 22, C_INK, 1);
}

static void play_banner(const hud_t *h) {
  if (!h->banner || h->banner_t <= 0) return;
  uint32_t k = hash_str(h->banner, 5);
  if (k != banner_key) { banner_key = k; banner_t0 = now_t; }
  cv *c = layer_begin(&L_banner, 760, 116, k);
  if (c) {
    cv_font(c, F_DISPLAY, 52); cv_letter_spacing(c, 5);
    float tw = cv_measure(c, h->banner), bw = fminf(tw + 80, 760);
    p_plate(c, 380 - bw / 2, 14, bw, 88, 16, C_INK, 0.88f);
    p_hazard(c, 380 - bw / 2, 14, bw, 7, 14, C_HIVIS, C_INK);
    p_hazard(c, 380 - bw / 2, 95, bw, 7, 14, C_HIVIS, C_INK);
    float px = tw > 700 ? 52 * 700 / tw : 52;
    p_text(c, F_DISPLAY, px, 5, CV_CENTER, h->banner, 380, 80, C_CREAM, 1);
    layer_end(&L_banner, c);
  }
  float age = (float)(now_t - banner_t0), pop = 1 + 0.35f * expf(-9 * age);
  float a = ds_clamp(h->banner_t / 0.35f, 0, 1);
  float m[6]; mat_place(m, 640, 250, 0, 380, 58, pop, pop);
  hg_tex_m(&L_banner, m, a, 0xffffff);
}

/* ── hints ── */
/* the bottom strip: the few controls that matter right now, with glyphs */
static void play_strip(const hud_t *h) {
  float a = h->t < 40 ? 0.95f : 0.5f;
  bool coil = h->weapon == WP_COIL;
  int own = 0; for (int i = 0; i < 4; i++) own += h->owned[i];
  uint32_t key = (uint32_t)h->used_pad | (uint32_t)h->weapon << 1 | (uint32_t)(h->lipos > 0) << 4 | (uint32_t)(own > 1) << 5;
  cv *c = layer_begin(&L_hints, 1000, 34, key);
  if (c) {
    char s[200];
    snprintf(s, sizeof s, "{%s} %s     {AIM} %s     {JUMP} JET     {DODGE} DODGE%s", h->used_pad ? "FIRE" : "FIREC",
             h->weapon == WP_IRON ? "SWING" : coil ? "CHARGE" : "FIRE", coil ? "SCOPE" : "AIM",
             h->lipos > 0 ? "     {LIPO} LIPO" : own > 1 ? "     {WEAPONC} WEAPON" : "");
    float w = g_markup(c, 0, 0, CV_LEFT, F_MONO, 15, 0, C_CREAM, 22, h->used_pad, s, false);
    p_plate(c, 500 - w / 2 - 16, 1, w + 32, 32, 9, C_INK, 0.78f);
    g_markup(c, 500, 23, CV_CENTER, F_MONO, 15, 0, C_CREAM, 22, h->used_pad, s, true);
    layer_end(&L_hints, c);
  }
  hg_tex(&L_hints, 140, 684, a);
}

/* a card in the bottom centre: glyph line(s) and a small caption; k keys the repaint */
static void hint_card(layer_t *L, bool pad, const char *big, const char *small, uint32_t col, float alpha, uint32_t k) {
  cv *c = layer_begin(L, 560, 92, hash_str(big, hash_str(small, k + (uint32_t)pad * 977)));
  if (c) {
    float w = g_markup(c, 0, 0, CV_LEFT, F_DISPLAY, 26, 3, C_CREAM, 40, pad, big, false);
    float sw = small ? g_markup(c, 0, 0, CV_LEFT, F_MONO, 16, 0, C_CREAM, 22, pad, small, false) : 0;
    float pw = fminf(540, fmaxf(w, sw) + 56);
    p_plate(c, 280 - pw / 2, 4, pw, small ? 80 : 56, 14, C_INK, 0.9f);
    cv_fill_hex(c, col, 1); cv_fill_rect(c, 280 - pw / 2 + 14, 4, pw - 28, 3);
    g_markup(c, 280, 50, CV_CENTER, F_DISPLAY, 26, 3, C_CREAM, 40, pad, big, true);
    if (small) g_markup(c, 280, 75, CV_CENTER, F_MONO, 16, 0, 0xd7d0bf, 22, pad, small, true);
    layer_end(L, c);
  }
  hg_tex(L, 640 - 280, 468, alpha);
}

/* level 1's first-time prompts, in turn; time based, with the jetpack step ending when it is used */
static void play_firsttime(const hud_t *h) {
  static const struct { const char *big, *small; float dur; } ST[] = {
    {"{MOVE} MOVE", "Walk with the left stick or the keys.", 5.5f},
    {"{LOOK} LOOK", "Turn the camera. The mouse works too.", 5.f},
    {"{JUMP} JUMP", NULL, 3.5f},
    {"HOLD {JUMP} IN THE AIR TO JET", "The drone motors lift you onto tables.", 7.f},
    {"{FIRE} FIRE", "Rubber bands. They do not run out.", 5.f},
    {"{AIM} AIM", "Hold to aim down the sights.", 4.5f},
  };
  static float skip; static bool jetted;
  if (h->t < 1) { skip = 0; jetted = false; }
  if (h->level != 0 || h->t < 3.7f) return;
  float tt = h->t - 3.7f + skip, at = 0;
  for (int i = 0; i < 6; i++) {
    float d = ST[i].dur;
    if (tt < at + d) {
      if (i == 3 && h->jetting) { jetted = true; }
      if (i == 3 && jetted && tt < at + d - 1.f) { skip += at + d - 1.f - tt; tt = at + d - 1.f; }
      float into = tt - at, a = fminf(ds_clamp(into / 0.3f, 0, 1), ds_clamp((d - into) / 0.4f, 0, 1));
      hint_card(&L_h1, h->used_pad, ST[i].big, ST[i].small, C_HIVIS, a, (uint32_t)i);
      return;
    }
    at += d;
  }
}

/* battery low: land to recharge, shown a few seconds each time it runs down */
static bool play_lowjet(const hud_t *h) {
  static float prev = 1, shown;
  if (prev >= 0.22f && h->jet < 0.22f) shown = 4.f;
  prev = h->jet;
  if (h->jet > 0.45f) shown = 0;
  if (shown <= 0) return false;
  shown -= dt_;
  hint_card(&L_h2, h->used_pad, "BATTERY LOW", "LAND TO RECHARGE THE JETPACK", C_ALERT, ds_clamp(shown / 0.4f, 0, 1), 3);
  return true;
}

/* a weapon was just switched to: its name, its button and what it does */
static bool play_wcard(const hud_t *h) {
  static int last = -1; static float shown;
  if (h->mode == HUD_PLAY) {
    if (last >= 0 && h->weapon != last) shown = 3.f;
    last = h->weapon;
  }
  if (shown <= 0) return false;
  shown -= dt_;
  static const char *HOW[WP_COUNT] = {
    "{FIRE} FIRE   fast semi auto, endless",
    "{FIRE} LOB   slows them, leaves a puddle",
    "HOLD {FIRE} RELEASE   {AIM} SCOPE",
    "{FIRE} SWING   melts plastic, up close"};
  int w = h->weapon < 0 || h->weapon >= WP_COUNT ? 0 : h->weapon;
  hint_card(&L_h3, h->used_pad, h->weapon_name ? h->weapon_name : "WEAPON", HOW[w], C_TAN, ds_clamp(shown / 0.4f, 0, 1) * ds_clamp((3.f - shown) / 0.2f, 0, 1), (uint32_t)w + 20);
  return true;
}

/* the interact prompt: big, under the crosshair, pulsing; this is how the printers get shut down */
static void play_interact(const hud_t *h) {
  if (!h->prompt || !h->prompt[0]) return;
  char s[120]; snprintf(s, sizeof s, "{IRON} %s", h->prompt);
  cv *c = layer_begin(&L_prompt, 900, 96, hash_str(s, (uint32_t)h->used_pad + 41));
  if (c) {
    float w = g_markup(c, 0, 0, CV_LEFT, F_DISPLAY, 38, 3, C_CREAM, 54, h->used_pad, s, false);
    float pw = fminf(880, w + 64);
    p_plate(c, 450 - pw / 2, 4, pw, 84, 16, C_INK, 0.92f);
    p_hazard(c, 450 - pw / 2, 4, pw, 8, 14, C_HIVIS, C_INK);
    p_hazard(c, 450 - pw / 2, 80, pw, 8, 14, C_HIVIS, C_INK);
    g_markup(c, 450, 64, CV_CENTER, F_DISPLAY, 38, 3, C_CREAM, 54, h->used_pad, s, true);
    layer_end(&L_prompt, c);
  }
  float pulse = 1 + 0.05f * sinf((float)now_t * 8);
  float m[6]; mat_place(m, 640, 430, 0, 450, 46, pulse, pulse);
  hg_tex_m(&L_prompt, m, 0.8f + 0.2f * sinf((float)now_t * 8), 0xffffff);
}

/* ── the centre ── */
static void ticks_ring(float cx, float cy, float r, int n, float frac, uint32_t col, uint32_t dim, float len) {
  int lit = (int)(frac * n + 0.5f);
  for (int i = 0; i < n; i++) {
    float a = (float)i * 2 * DS_PI / n;
    bool on = i < lit;
    hg_rect_m(cx + sinf(a) * r, cy - cosf(a) * r, 4, len + (on ? 2.f : 0.f), a, on ? col : dim, on ? 1.f : 0.45f);
  }
}

/* the reticle follows the weapon: ticks for the repeater, a ring for glue, a diamond for the coil, a bracket for the iron;
 * a hit marker pops, the hurt arc points at where it came from */
static void centre_at(float cx, float cy, int weapon, float charge, float hit_marker, bool hit_weak) {
  switch (weapon) {
    case WP_GLUE:
      for (int i = 0; i < 12; i++) {
        float a = i * DS_PI / 6;
        hg_rect_m(cx + sinf(a) * 20, cy - cosf(a) * 20, 5, 5, a, C_INK, 0.7f);
        hg_rect_m(cx + sinf(a) * 20, cy - cosf(a) * 20, 3, 3, a, C_GOO, 1);
      }
      break;
    case WP_COIL:
      for (int i = 0; i < 4; i++) {
        float a = i * DS_PI / 2 + DS_PI / 4;
        hg_rect_m(cx + sinf(a) * 16, cy - cosf(a) * 16, 16, 5, a + DS_PI / 2, C_INK, 0.7f);
        hg_rect_m(cx + sinf(a) * 16, cy - cosf(a) * 16, 14, 3, a + DS_PI / 2, C_CREAM, 1);
      }
      break;
    case WP_IRON:
      for (int i = 0; i < 4; i++) {
        float sx = i & 1 ? 1.f : -1.f, sy = i & 2 ? 1.f : -1.f;
        hg_rect(cx + sx * 16 - (sx > 0 ? 0 : 10), cy + sy * 16 - 1.5f, 10, 3, C_CREAM, 1);
        hg_rect(cx + sx * 16 - 1.5f, cy + sy * 16 - (sy > 0 ? 0 : 10), 3, 10, C_CREAM, 1);
      }
      break;
    default:
      for (int i = 0; i < 4; i++) {
        float a = i * DS_PI / 2, dx = sinf(a), dy = -cosf(a), r = 17;
        hg_rect_m(cx + dx * r, cy + dy * r, 8, 18, a, C_INK, 0.65f);
        hg_rect_m(cx + dx * r, cy + dy * r, 4, 14, a, C_CREAM, 1);
      }
      break;
  }
  hg_rect_m(cx, cy, 8, 8, 0.785f, C_INK, 0.65f);
  hg_rect_m(cx, cy, 4, 4, 0.785f, C_HIVIS, 1);
  /* the coilgun's charge: a ring of ticks that fills, turns orange and pulses when it is ready */
  if (weapon == WP_COIL && charge > 0.01f) {
    bool full = charge >= 0.99f;
    float pulse = full ? 0.5f + 0.5f * sinf((float)now_t * 24) : 0.f;
    ticks_ring(cx, cy, 44, 40, charge, full ? C_HIVIS : C_TEAL, C_CREAM, 9 + 4 * pulse);
  }
  if (hit_marker > 0) {
    float k = ds_clamp(hit_marker / 0.25f, 0, 1), sc = 1 + 0.5f * k, a = ds_clamp(hit_marker / 0.1f, 0, 1);
    uint32_t col = hit_weak ? C_HIVIS : C_CREAM;
    float off = (hit_weak ? 24 : 19) * sc, len = hit_weak ? 16 : 12, th = hit_weak ? 5 : 3;
    for (int i = 0; i < 4; i++) {
      float ang = DS_PI / 4 + i * DS_PI / 2, dx = sinf(ang), dy = -cosf(ang);
      hg_rect_m(cx + dx * off, cy + dy * off, th + 3, len + 3, ang, C_INK, 0.7f * a);
      hg_rect_m(cx + dx * off, cy + dy * off, th, len, ang, col, a);
    }
  }
}
static void play_centre(const hud_t *h) { centre_at(640, 360, h->weapon, h->charge, h->hit_marker, h->hit_weak); }

static void hurt_at(float cx, float cy, float rx, float hurt, float angle) {
  if (hurt <= 0) return;
  float k = ds_clamp(hurt / 0.5f, 0, 1);
  hg_vignette_at(cx, cy, rx, 509, C_ALERT, 0.62f * k, 0.45f);
  cv *c = layer_begin(&L_arc, 360, 120, 1);
  if (c) {   /* an arc of a ring around the screen centre, drawn about a point below the layer */
    cv_save(c);
    cv_line_cap(c, CV_CAP_BUTT);
    for (int i = 0; i < 12; i++) {
      float a0 = -0.62f + i * 0.02f, a1 = 0.62f - i * 0.02f;
      cv_begin(c); cv_arc(c, 180, 290, 230 - i * 2.5f, -DS_PI / 2 + a0, -DS_PI / 2 + a1, false);
      cv_stroke_hex(c, C_ALERT, 0.14f + i * 0.07f); cv_line_width(c, 5); cv_stroke(c);
    }
    cv_restore(c);
    layer_end(&L_arc, c);
  }
  float sc = fminf(1, rx / 640);
  float m[6]; mat_place(m, cx, cy, angle, 180, 290, sc, sc);
  hg_tex_m(&L_arc, m, k, 0xffffff);
}
static void hurt_fx(const hud_t *h) { hurt_at(640, 360, 905, h->hurt, h->hurt_angle); }

/* ── the objective arrow ── */
/* the arrow over a part of the screen ox..ox+w wide; positions in that part's own pixels */
static void marker_at(float ox, float w, layer_t *Ld, bool on_, float mxp, float myp, bool behind, float dist) {
  if (!on_) return;
  cv *c = layer_begin(&L_mark, 64, 64, 1);
  if (c) {   /* a diamond with a ring: the place itself */
    cv_stroke_hex(c, C_INK, 0.8f); cv_line_width(c, 8);
    cv_begin(c); cv_move(c, 32, 6); cv_line(c, 58, 32); cv_line(c, 32, 58); cv_line(c, 6, 32); cv_close(c); cv_stroke(c);
    cv_stroke_hex(c, C_HIVIS, 1); cv_line_width(c, 4);
    cv_begin(c); cv_move(c, 32, 6); cv_line(c, 58, 32); cv_line(c, 32, 58); cv_line(c, 6, 32); cv_close(c); cv_stroke(c);
    cv_begin(c); cv_arc(c, 32, 32, 5, 0, 6.2832f, false); cv_fill_hex(c, C_CREAM, 1); cv_fill(c);
    layer_end(&L_mark, c);
  }
  c = layer_begin(&L_marrow, 64, 64, 1);
  if (c) {   /* a chevron pointing up, turned toward the place when it is off screen */
    cv_begin(c); cv_move(c, 32, 8); cv_line(c, 56, 52); cv_line(c, 32, 40); cv_line(c, 8, 52); cv_close(c);
    cv_fill_hex(c, C_INK, 0.85f); cv_stroke_hex(c, C_INK, 0.85f); cv_line_width(c, 8); cv_line_join(c, CV_JOIN_ROUND); cv_stroke(c); cv_fill(c);
    cv_begin(c); cv_move(c, 32, 8); cv_line(c, 56, 52); cv_line(c, 32, 40); cv_line(c, 8, 52); cv_close(c);
    cv_fill_hex(c, C_HIVIS, 1); cv_fill(c);
    layer_end(&L_marrow, c);
  }
  int cm = (int)(dist * 5.f + 2.5f) / 5 * 5;   /* units * 5 = centimetres, steady in fives */
  char b[24]; snprintf(b, sizeof b, "%d CM", cm);
  c = layer_begin(Ld, 140, 28, hash_str(b, 77));
  if (c) {
    p_plate(c, 0, 0, 140, 28, 7, C_INK, 0.85f);
    p_text(c, F_MONO, 17, 0, CV_CENTER, b, 70, 20, C_CREAM, 1);
    layer_end(Ld, c);
  }
  float pulse = 1 + 0.06f * sinf((float)now_t * 5);
  const float hw = w * 0.5f;
  const float mx0 = 70, my0 = 120, mx1 = w - 70, my1 = DS_H - 140;   /* keep clear of the plates */
  float dx = mxp - hw, dy = myp - 360;
  bool off = behind || mxp < mx0 || mxp > mx1 || myp < my0 || myp > my1;
  float px, py;
  if (!off) {
    px = mxp; py = myp;
  } else {
    if (behind) { dx = -dx; dy = -dy; }
    if (fabsf(dx) < 1 && fabsf(dy) < 1) dy = 1;   /* straight behind: point down */
    float hx = hw - mx0, hy = (dy > 0 ? my1 : my0) - 360;
    float sx = fabsf(dx) > 0.001f ? hx / fabsf(dx) : 1e9f, sy = fabsf(hy) / fmaxf(fabsf(dy), 0.001f);
    float s = fminf(sx, sy);
    px = hw + dx * s; py = 360 + dy * s;
    float ang = atan2f(dx, -dy);
    float m[6]; mat_place(m, ox + px, py, ang, 32, 32, pulse, pulse);
    hg_tex_m(&L_marrow, m, 1, 0xffffff);
  }
  if (!off) { float m[6]; mat_place(m, ox + px, py, 0, 32, 32, pulse, pulse); hg_tex_m(&L_mark, m, 1, 0xffffff); }
  float ly = py + (off ? (py > 360 ? -62.f : 38.f) : 34.f);
  hg_tex(Ld, ox + px - 70, ly, 0.95f);
}
static void objective_marker(const hud_t *h) { marker_at(0, DS_W, &L_mdist, h->marker_on, h->marker_x, h->marker_y, h->marker_behind, h->marker_dist); }

/* ── scope ── */
static void scope_layers(void) {
  for (int zoom = 1; zoom <= 3; zoom++) {
    cv *c = layer_begin(&L_ret[zoom - 1], 720, 720, (uint32_t)zoom);
    if (!c) continue;
    const float O = 360, R = 330, sp = 34 - zoom * 3;
    cv_line_cap(c, CV_CAP_BUTT);
    /* arms: thin and fine inside, heavy posts outside */
    for (int pass = 0; pass < 2; pass++) {
      cv_stroke_hex(c, pass ? 0xffffff : 0x000000, pass ? 1.f : 0.5f);
      for (int i = 0; i < 4; i++) {
        float a = i * DS_PI / 2, dx = sinf(a), dy = -cosf(a);
        cv_line_width(c, pass ? 2.f : 4.f);
        cv_begin(c); cv_move(c, O + dx * 22, O + dy * 22); cv_line(c, O + dx * 150, O + dy * 150); cv_stroke(c);
        cv_line_width(c, pass ? 5.f : 8.f);
        cv_begin(c); cv_move(c, O + dx * 150, O + dy * 150); cv_line(c, O + dx * (R - 4), O + dy * (R - 4)); cv_stroke(c);
      }
    }
    for (int i = 0; i < 4; i++) {   /* mil-dots */
      float a = i * DS_PI / 2, dx = sinf(a), dy = -cosf(a);
      for (int k = 1; k <= 4; k++) {
        float r = 22 + k * sp; if (r > 146) break;
        cv_begin(c); cv_arc(c, O + dx * r, O + dy * r, k == 4 ? 3.2f : 2.6f, 0, 6.2832f, false);
        cv_fill_hex(c, 0x000000, 0.5f); cv_fill(c);
        cv_begin(c); cv_arc(c, O + dx * r, O + dy * r, 2.f, 0, 6.2832f, false);
        cv_fill_hex(c, 0xffffff, 1); cv_fill(c);
      }
    }
    cv_stroke_hex(c, 0xffffff, 0.8f); cv_line_width(c, 2);   /* bezel ring and its ticks */
    cv_begin(c); cv_arc(c, O, O, R - 2, 0, 6.2832f, false); cv_stroke(c);
    cv_line_width(c, 1.5f);
    for (int a = 0; a < 360; a += 10) {
      float r = a % 30 == 0 ? 20.f : 10.f, rad = a * DS_PI / 180;
      cv_begin(c); cv_move(c, O + sinf(rad) * (R - 3), O - cosf(rad) * (R - 3)); cv_line(c, O + sinf(rad) * (R - 3 - r), O - cosf(rad) * (R - 3 - r)); cv_stroke(c);
    }
    cv_stroke_hex(c, 0xffffff, 0.22f); cv_line_width(c, 1.2f);
    cv_begin(c); cv_arc(c, O, O, 166, 0, 6.2832f, false); cv_stroke(c);
    cv_begin(c); cv_arc(c, O, O, 2.4f, 0, 6.2832f, false); cv_fill_hex(c, 0xffffff, 1); cv_fill(c);
    layer_end(&L_ret[zoom - 1], c);
  }
  cv *c = layer_begin(&L_brk, 96, 96, 1);
  if (c) {   /* the weak point bracket */
    cv_line_cap(c, CV_CAP_SQUARE);
    for (int pass = 0; pass < 2; pass++) {
      cv_stroke_hex(c, pass ? C_HIVIS : C_INK, pass ? 1.f : 0.8f); cv_line_width(c, pass ? 5.f : 9.f);
      for (int i = 0; i < 4; i++) {
        float sx = i & 1 ? 1.f : -1.f, sy = i & 2 ? 1.f : -1.f, x = 48 + sx * 38, y = 48 + sy * 38;
        cv_begin(c); cv_move(c, x - sx * 20, y); cv_line(c, x, y); cv_line(c, x, y - sy * 20); cv_stroke(c);
      }
    }
    layer_end(&L_brk, c);
  }
  c = layer_begin(&L_weak, 280, 40, 1);
  if (c) {
    p_plate(c, 0, 0, 280, 40, 10, C_HIVIS, 1);
    p_text(c, F_DISPLAY, 24, 4, CV_CENTER, "WEAK POINT", 140, 29, C_INK, 1);
    layer_end(&L_weak, c);
  }
}

/* paint the scope's big layers up front, so the first zoom in is not a hitch */
void hud_init(void) {
  hg_init();
  scope_layers();
}

static void scope_draw(const hud_t *h) {
  int zoom = (int)ds_clamp((float)h->zoom, 1, 3);
  if (!was_scoped) scope_t0 = now_t;
  was_scoped = true;
  const float cx = 640, cy = 360;
  float age = (float)(now_t - scope_t0);
  float R = 330 + 520 * (1 - hud_ease_out(age / 0.18f));   /* the lens irises in */
  hg_circle(cx, cy, R, C_ALERT, h->on_enemy ? 0.14f : 0);
  hg_scope(cx, cy, R < 330 ? 330 : R);
  float m[6]; mat_place(m, cx, cy, 0, 360, 360, R > 330 ? R / 330 : 1, R > 330 ? R / 330 : 1);
  hg_tex_m(&L_ret[zoom - 1], m, ds_clamp(age / 0.12f, 0, 1), h->on_enemy ? C_ALERT : 0xffffff);
  /* the coil's charge, a ring just inside the bezel */
  if (h->charge > 0.01f) {
    bool full = h->charge >= 0.99f;
    ticks_ring(cx, cy, 300, 72, h->charge, full ? C_HIVIS : C_TEAL, 0x000000, 14);
  }

  float pulse = 0.5f + 0.5f * sinf((float)now_t * 10);
  if (h->on_weak) {
    float s = 1.35f + 0.18f * pulse;
    float mm[6]; mat_place(mm, cx, cy, 0, 48, 48, s, s);
    hg_tex_m(&L_brk, mm, 1, 0xffffff);
    hg_tex(&L_weak, cx - 140, cy + 92, 0.55f + 0.45f * pulse);
  }
  if (h->weak_mark) {
    float s = 0.8f + 0.12f * pulse;
    float mm[6]; mat_place(mm, h->weak_x, h->weak_y, 0, 48, 48, s, s);
    hg_tex_m(&L_brk, mm, 0.9f, 0xffffff);
  }

  /* readouts in the dark corners beside the lens */
  char b[32];
  cv *c = layer_begin(&L_zl, 250, 250, (uint32_t)zoom | (uint32_t)(h->ammo & 255) << 8 | (uint32_t)h->used_pad << 20);
  if (c) {
    p_plate(c, 0, 0, 250, 250, 16, C_INK, 0.9f);
    p_tape(c, 18, 14, 22, F_DISPLAY, 14, 3, "ZOOM", C_HIVIS, C_INK);
    snprintf(b, sizeof b, "%dX", 1 << zoom);
    p_text(c, F_DISPLAY, 80, 0, CV_LEFT, b, 18, 118, C_CREAM, 1);
    for (int i = 1; i <= 3; i++) { cv_fill_hex(c, i <= zoom ? C_HIVIS : 0x2a2f3c, 1); cv_fill_rect(c, 20 + (i - 1) * 72, 136, 64, 10); }
    p_tape(c, 18, 166, 22, F_DISPLAY, 14, 3, "CELLS", C_TEAL, C_INK);
    snprintf(b, sizeof b, "%02d", h->ammo < 0 ? 0 : h->ammo);
    p_text(c, F_DISPLAY, 40, 2, CV_LEFT, b, 20, 232, h->ammo ? C_CREAM : C_ALERT, 1);
    g_markup(c, 232, 232, CV_RIGHT, F_MONO, 15, 0, 0x9aa1b2, 22, h->used_pad, "{ZOOM}", true);
    layer_end(&L_zl, c);
  }
  hg_tex(&L_zl, 40, 120, 1);
  int cm = (int)(h->range * 5.f + 0.5f);
  uint32_t kr = (uint32_t)(cm < 0 ? 0 : cm > 99999 ? 99999 : cm) | (uint32_t)h->on_enemy << 20 | (uint32_t)h->on_weak << 21 | (uint32_t)(h->range > 0.1f) << 22;
  c = layer_begin(&L_zr, 250, 250, kr);
  if (c) {
    p_plate(c, 0, 0, 250, 250, 16, C_INK, 0.9f);
    p_tape(c, 18, 14, 22, F_DISPLAY, 14, 3, "RANGE", C_HIVIS, C_INK);
    if (h->range > 0.1f) snprintf(b, sizeof b, "%d", cm); else snprintf(b, sizeof b, "...");
    p_text(c, F_DISPLAY, cm > 9999 ? 56 : 70, 0, CV_LEFT, b, 18, 108, C_CREAM, 1);
    p_text(c, F_DISPLAY, 24, 3, CV_LEFT, "CM", 20, 146, C_TEAL, 1);
    if (h->on_enemy) {
      p_plate(c, 18, 170, 214, 56, 10, h->on_weak ? C_HIVIS : C_ALERT, 1);
      p_text(c, F_DISPLAY, 24, 3, CV_CENTER, h->on_weak ? "WEAK POINT" : "ARMY MAN", 125, 207, h->on_weak ? C_INK : C_CREAM, 1);
    } else {
      cv_stroke_hex(c, 0x586070, 1); cv_line_width(c, 2); cv_begin(c); cv_rect(c, 19, 171, 212, 54); cv_stroke(c);
      p_text(c, F_DISPLAY, 18, 3, CV_CENTER, "NO TARGET", 125, 206, 0x7b8294, 1);
    }
    layer_end(&L_zr, c);
  }
  hg_tex(&L_zr, 1280 - 40 - 250, 120, 1);
  /* health bar, so scoping in does not blind you to dying */
  bar(40, 520, 200, 12, s_health, s_trail, s_health < 0.28f && sinf((float)now_t * 9) > 0 ? C_ALERT : C_HIVIS, C_CREAM);
  segs(1, 40, 520, 200, 12, 20);
}

/* ── two players: a half each ──
 * Player 1 on the left, player 2 on the right, each half its own crosshair,
 * vitals, kit, prompt, scope and DOWN card in the same shop-label look, drawn
 * clipped to the half. The objective, banners, the boss bar and the radio
 * stay shared, centred across the screen. */
static layer_t L_svit[2], L_swpn[2], L_sprompt[2], L_sdown[2], L_sread[2], L_smdist[2], L_join;
static float sp_health[2], sp_trail[2], sp_jet[2];
static double sp_scope_t0[2]; static bool sp_was_scoped[2];

/* name, health and the jetpack battery: a plate in the half's bottom left */
static void half_vitals(int i, const hud_player_t *p, float ox) {
  const float x = ox + 24, y = 616, w = 300, hh = 80;
  char tag[40]; snprintf(tag, sizeof tag, "P%d  %s", i + 1, p->name ? p->name : "");
  cv *c = layer_begin(&L_svit[i], (int)w, (int)hh, hash_str(tag, 61));
  if (c) {
    p_plate(c, 0, 0, w, hh, 14, C_INK, 0.86f);
    p_hazard(c, 0, 14, 8, hh - 14, 12, i ? C_TEAL : C_HIVIS, C_INK);
    p_tape(c, 20, 8, 20, F_DISPLAY, 13, 3, tag, C_CREAM, C_INK);
    p_tape(c, 218, 8, 18, F_DISPLAY, 11, 2, "JET", C_TEAL, C_INK);
    layer_end(&L_svit[i], c);
  }
  hg_tex(&L_svit[i], x, y, 1);
  float t = (float)(now_t * 9);
  bool low = sp_health[i] < 0.28f;
  uint32_t hc = low ? (sinf(t) > 0 ? C_ALERT : 0xff8a7a) : C_HIVIS;
  bar(x + 20, y + 46, 182, 12, sp_health[i], sp_trail[i], hc, C_CREAM);
  segs(2, x + 20, y + 46, 182, 12, 18.2f);
  bool jlow = sp_jet[i] < 0.25f, flash = jlow && sinf(t * 1.3f) > 0;
  battery(x + 218, y + 34, 64, 34, sp_jet[i], jlow ? C_ALERT : C_TEAL, flash ? C_ALERT : C_CREAM, p->jetting ? 0.6f + 0.4f * sinf(t * 2) : 0);
}

/* the weapon, its ammo, the four slots as ticks and the LiPos: a plate in the half's bottom right */
static void half_weapon(int i, const hud_player_t *p, float ox) {
  const float w = 280, hh = 80, x = ox + 640 - 24 - w, y = 616;
  int own = 0; for (int k = 0; k < 4; k++) own |= p->owned[k] << k;
  uint32_t key = (uint32_t)p->weapon | (uint32_t)(p->ammo + 1) << 4 | (uint32_t)(p->lipos & 15) << 16 | (uint32_t)own << 20 | (uint32_t)p->used_pad << 24;
  key = hash_str(p->weapon_name, key);
  cv *c = layer_begin(&L_swpn[i], (int)w, (int)hh, key);
  if (c) {
    p_plate(c, 0, 0, w, hh, 14, C_INK, 0.86f);
    p_hazard(c, w - 8, 0, 8, hh - 14, 12, C_HIVIS, C_INK);
    p_tape(c, 14, 8, 20, F_DISPLAY, 12, 2, p->weapon_name ? p->weapon_name : "WEAPON", C_HIVIS, C_INK);
    for (int k = 0; k < WP_COUNT; k++) {   /* the slots: cream for the one in hand, orange for found, dark for not yet */
      bool sel = p->weapon == k, own_k = p->owned[k];
      cv_fill_hex(c, sel ? C_CREAM : own_k ? C_HIVIS : 0x2a2f3c, 1);
      cv_fill_rect(c, 16 + k * 26, 36, 22, sel ? 7 : 4);
    }
    if (p->ammo < 0) p_text(c, F_DISPLAY, 18, 2, CV_LEFT, "ENDLESS", 16, 70, C_CREAM, 1);
    else {
      char b[16]; snprintf(b, sizeof b, "%02d", p->ammo);
      p_text(c, F_DISPLAY, 28, 1, CV_LEFT, b, 16, 72, p->ammo ? C_CREAM : C_ALERT, 1);
      p_text(c, F_MONO, 11, 0, CV_LEFT, AMMO_LABEL[p->weapon >= 0 && p->weapon < WP_COUNT ? p->weapon : 0], 62, 68, 0x9aa1b2, 1);
    }
    g_markup(c, w - 18, 46, CV_RIGHT, F_MONO, 12, 0, 0x9aa1b2, 16, p->used_pad, "{LIPO}", true);
    int n = p->lipos < 5 ? p->lipos : 5;
    for (int k = 0; k < 5; k++) {
      float px_ = w - 20 - 5 * 17 + k * 17, py = 56;
      if (k < n) {
        cv_fill_hex(c, C_HIVIS, 1); cv_fill_rect(c, px_, py + 3, 13, 11);
        cv_fill_hex(c, C_INK, 1); cv_fill_rect(c, px_ + 3, py, 7, 4); cv_fill_rect(c, px_ + 5, py + 6, 3, 5);
      } else { cv_stroke_hex(c, 0x586070, 1); cv_line_width(c, 1.5f); cv_begin(c); cv_rect(c, px_ + 0.75f, py + 3.75f, 11.5f, 9.5f); cv_stroke(c); }
    }
    layer_end(&L_swpn[i], c);
  }
  hg_tex(&L_swpn[i], x, y, 1);
}

/* the interact prompt under this half's crosshair */
static void half_prompt(int i, const hud_player_t *p, float ox) {
  if (!p->prompt || !p->prompt[0]) return;
  char s[120]; snprintf(s, sizeof s, "{IRON} %s", p->prompt);
  cv *c = layer_begin(&L_sprompt[i], 600, 70, hash_str(s, (uint32_t)p->used_pad + 43));
  if (c) {
    float w = g_markup(c, 0, 0, CV_LEFT, F_DISPLAY, 26, 3, C_CREAM, 38, p->used_pad, s, false);
    float pw = fminf(590, w + 48);
    p_plate(c, 300 - pw / 2, 4, pw, 60, 12, C_INK, 0.92f);
    p_hazard(c, 300 - pw / 2, 4, pw, 6, 12, C_HIVIS, C_INK);
    p_hazard(c, 300 - pw / 2, 58, pw, 6, 12, C_HIVIS, C_INK);
    g_markup(c, 300, 46, CV_CENTER, F_DISPLAY, 26, 3, C_CREAM, 38, p->used_pad, s, true);
    layer_end(&L_sprompt[i], c);
  }
  float pulse = 1 + 0.05f * sinf((float)now_t * 8);
  float m[6]; mat_place(m, ox + 320, 440, 0, 300, 34, pulse, pulse);
  hg_tex_m(&L_sprompt[i], m, 0.8f + 0.2f * sinf((float)now_t * 8), 0xffffff);
}

/* the coilgun's scope in a half: a smaller lens, its readouts in one strip under it */
static void half_scope(int i, const hud_player_t *p, float ox) {
  int zoom = (int)ds_clamp((float)p->zoom, 1, 3);
  if (!sp_was_scoped[i]) sp_scope_t0[i] = now_t;
  sp_was_scoped[i] = true;
  const float cx = ox + 320, cy = 340, R0 = 270, k = R0 / 330;
  float age = (float)(now_t - sp_scope_t0[i]);
  float R = R0 + 420 * (1 - hud_ease_out(age / 0.18f));
  hg_circle(cx, cy, R, C_ALERT, p->on_enemy ? 0.14f : 0);
  hg_scope(cx, cy, R < R0 ? R0 : R);
  float sc = R > R0 ? R / 330 : k;
  float m[6]; mat_place(m, cx, cy, 0, 360, 360, sc, sc);
  hg_tex_m(&L_ret[zoom - 1], m, ds_clamp(age / 0.12f, 0, 1), p->on_enemy ? C_ALERT : 0xffffff);
  if (p->charge > 0.01f) {
    bool full = p->charge >= 0.99f;
    ticks_ring(cx, cy, 300 * k, 72, p->charge, full ? C_HIVIS : C_TEAL, 0x000000, 12);
  }
  float pulse = 0.5f + 0.5f * sinf((float)now_t * 10);
  if (p->on_weak) { float s = 1.1f + 0.15f * pulse; float mm[6]; mat_place(mm, cx, cy, 0, 48, 48, s, s); hg_tex_m(&L_brk, mm, 1, 0xffffff); }
  if (p->weak_mark) { float s = 0.7f + 0.1f * pulse; float mm[6]; mat_place(mm, ox + p->weak_x, p->weak_y - 20, 0, 48, 48, s, s); hg_tex_m(&L_brk, mm, 0.9f, 0xffffff); }
  int cm = (int)(p->range * 5.f + 0.5f);
  char rb[24]; if (p->range > 0.1f) snprintf(rb, sizeof rb, "%d CM", cm); else snprintf(rb, sizeof rb, "...");
  char b[64]; snprintf(b, sizeof b, "%dX|%s|%02d", 1 << zoom, rb, p->ammo < 0 ? 0 : p->ammo);
  cv *c = layer_begin(&L_sread[i], 440, 44, hash_str(b, (uint32_t)p->on_enemy << 1 | (uint32_t)p->on_weak << 2 | (uint32_t)p->used_pad << 3));
  if (c) {
    p_plate(c, 0, 0, 440, 44, 10, C_INK, 0.92f);
    char zb[8]; snprintf(zb, sizeof zb, "%dX", 1 << zoom);
    float x = 12 + p_tape(c, 12, 11, 22, F_DISPLAY, 15, 2, zb, C_HIVIS, C_INK) + 10;
    p_text(c, F_MONO, 15, 0, CV_LEFT, "RANGE", x, 28, 0x9aa1b2, 1);
    p_text(c, F_DISPLAY, 17, 1, CV_LEFT, rb, x + 56, 29, C_CREAM, 1);
    char cb[16]; snprintf(cb, sizeof cb, "CELLS %02d", p->ammo < 0 ? 0 : p->ammo);
    if (p->on_enemy) p_tape(c, 300, 11, 22, F_DISPLAY, 12, 2, p->on_weak ? "WEAK POINT" : "ARMY MAN", p->on_weak ? C_HIVIS : C_ALERT, p->on_weak ? C_INK : C_CREAM);
    else p_text(c, F_MONO, 14, 0, CV_RIGHT, cb, 428, 28, p->ammo ? C_TEAL : C_ALERT, 1);
    layer_end(&L_sread[i], c);
  }
  hg_tex(&L_sread[i], ox + 100, 634, 1);
  bar(ox + 100, 690, 160, 10, sp_health[i], sp_trail[i], sp_health[i] < 0.28f && sinf((float)now_t * 9) > 0 ? C_ALERT : C_HIVIS, C_CREAM);
}

/* down: the half goes dim and red, a card counts the seconds until they get up */
static void half_down(int i, const hud_player_t *p, float ox) {
  hg_rect(ox, 0, 640, DS_H, C_INK, 0.42f);
  hg_vignette_at(ox + 320, 360, 452, 509, C_ALERT, 0.55f, 0.3f);
  int secs = (int)ceilf(p->down);
  cv *c = layer_begin(&L_sdown[i], 420, 150, (uint32_t)secs + 7);
  if (c) {
    p_plate(c, 0, 0, 420, 150, 18, C_INK, 0.92f);
    p_hazard(c, 0, 0, 420, 9, 14, C_ALERT, C_INK);
    p_text(c, F_DISPLAY, 72, 6, CV_CENTER, "DOWN", 213, 92, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, 72, 6, CV_CENTER, "DOWN", 210, 88, C_CREAM, 1);
    char b[32]; snprintf(b, sizeof b, "BACK UP IN %d", secs);
    p_text(c, F_MONO, 20, 2, CV_CENTER, b, 210, 128, C_HIVIS, 1);
    layer_end(&L_sdown[i], c);
  }
  float s = 1 + 0.04f * sinf((float)now_t * 4);
  float m[6]; mat_place(m, ox + 320, 330, -0.03f, 210, 75, s, s);
  hg_tex_m(&L_sdown[i], m, 1, 0xffffff);
}

static void play_split(const hud_t *h) {
  for (int i = 0; i < 2; i++) {
    const hud_player_t *p = &h->pl[i];
    float ox = i * 640.f;
    hg_clip(ox, 0, 640, DS_H);
    if (p->down > 0) { half_down(i, p, ox); half_vitals(i, p, ox); hg_noclip(); continue; }
    if (p->scoped) { half_scope(i, p, ox); hurt_at(ox + 320, 360, 452, p->hurt, p->hurt_angle); hg_noclip(); continue; }
    sp_was_scoped[i] = false;
    centre_at(ox + 320, 360, p->weapon, p->charge, p->hit_marker, p->hit_weak);
    marker_at(ox, 640, &L_smdist[i], p->marker_on, p->marker_x, p->marker_y, p->marker_behind, p->marker_dist);
    hurt_at(ox + 320, 360, 452, p->hurt, p->hurt_angle);
    half_vitals(i, p, ox);
    half_weapon(i, p, ox);
    half_prompt(i, p, ox);
    hg_noclip();
  }
}

/* one player, a second pad there: how to drop in */
void hud_join_hint(float y) {
  cv *c = layer_begin(&L_join, 320, 34, 3);
  if (c) {
    float w = g_markup(c, 0, 0, CV_LEFT, F_MONO, 14, 1, C_CREAM, 22, true, "P2 PRESS {=START} TO JOIN", false);
    p_plate(c, 320 - w - 28, 0, w + 28, 34, 8, C_INK, 0.8f);
    g_markup(c, 306, 23, CV_RIGHT, F_MONO, 14, 1, C_CREAM, 22, true, "P2 PRESS {=START} TO JOIN", true);
    layer_end(&L_join, c);
  }
  hg_tex(&L_join, 1240 - 320, y, 0.6f + 0.3f * (0.5f + 0.5f * sinf((float)now_t * 3)));
}

#ifdef SP_HUD_DEMO
/* build with EXTRA_CFLAGS=-DSP_HUD_DEMO: fills the play HUD with sample values so it can be shot without a level.
 * Options: -DSP_HUD_DEMO_PAD (pad prompts), -DSP_HUD_LEVEL=n (0 shows level 1's first-time prompts),
 * -DSP_HUD_SCOPE_AT=seconds (the scope comes up then), -DSP_HUD_DEMO_MODE=HUD_x (force a screen) */
#ifndef SP_HUD_LEVEL
#define SP_HUD_LEVEL 3
#endif
#ifndef SP_HUD_SCOPE_AT
#define SP_HUD_SCOPE_AT 1000
#endif
static hud_t demo_fill(const hud_t *in) {
  hud_t d = *in;
#ifdef SP_HUD_DEMO_PAD
  d.used_pad = true;
#else
  d.used_pad = false;
#endif
#ifdef SP_HUD_DEMO_MODE   /* force a screen: the HUD_* value (3 pause, 4 dead, 5 clear, 6 end, 7 cine) */
  d.mode = SP_HUD_DEMO_MODE; d.t = in->t + (SP_HUD_DEMO_MODE == HUD_END ? 60 : 0);
  d.level_blurb = "The army has set up a supply line between the bins. The glue gun is in one of them, and the bin is labelled.";
  d.level_name = "PRINT FARM"; d.quip = "Please do not feed the army men."; d.kills = 31; d.kills_total = 36;
  d.clear_time_s = 412.6f; d.best_time_ms = 398000; d.score = 18400; d.best_score = 16200; d.new_best = true; d.menu_sel = 5;
  d.music_on = true; d.invert_y = true; d.cine = 1; d.cap_speaker = "LUIS"; d.cap_text = "We are not opening anyone's bin."; d.cap_chars = 200; d.skip = 0.4f; d.flash = 0.15f;
  return d;
#endif
  if (d.mode == HUD_CINE || d.mode == HUD_INTRO) { d.mode = HUD_PLAY; d.cine = 0; d.cap_text = NULL; }   /* the game opens with a cutscene: fake the play state over it */
  if (d.mode != HUD_PLAY && d.mode != HUD_TITLE) return d;
  if (d.mode == HUD_TITLE) { d.unlocked = 4; return d; }
  d.hero_name = "MOHEEB"; d.buddy_name = "LUIS";
  d.health = 0.62f; d.buddy_health = 0.6f; d.jet = d.t > 40 ? 0.15f : 0.8f; d.jetting = false;
  d.weapon = d.t > 6 ? WP_GLUE : WP_BANDS; d.weapon_name = d.weapon == WP_GLUE ? "HOT GLUE GUN" : "RUBBER BAND REPEATER"; d.ammo = d.weapon == WP_GLUE ? 40 : -1;
  d.owned[0] = d.owned[1] = d.owned[2] = true; d.lipos = 3; d.charge = 0;
  d.score = 18400; d.time_s = 151.4f; d.level = SP_HUD_LEVEL;
  d.objective = "CANCEL THE PRINTS 2/4"; d.prompt = "PULL THE PLUG";
  d.marker_on = true; d.marker_x = 980; d.marker_y = 300; d.marker_dist = 168;
  if (d.t > SP_HUD_SCOPE_AT) { d.weapon = WP_COIL; d.weapon_name = "COILGUN"; d.charge = 0.7f; d.scoped = true; d.zoom = 2; d.range = 41.f; d.on_enemy = true; d.on_weak = true; d.weak_mark = true; d.weak_x = 760; d.weak_y = 300; d.ammo = 7; }
  return d;
}
#endif

void hud_draw(const hud_t *hin) {
#ifdef SP_HUD_DEMO
  hud_t demo = demo_fill(hin);
  const hud_t *h = &demo;
#else
  const hud_t *h = hin;
#endif
  double n = ds_now();
  dt_ = (float)ds_clamp((float)(n - now_t), 0, 0.1f);
  now_t = n;
  if (h->mode != HUD_PLAY || !h->scoped) was_scoped = false;
  /* glide the display values toward the real ones */
  s_health = ds_damp(s_health, h->health, 14, dt_);
  s_buddy = ds_damp(s_buddy, h->buddy_health, 10, dt_);
  s_jet = ds_damp(s_jet, h->jet, 16, dt_);
  s_trail = s_trail > s_health ? fmaxf(s_health, s_trail - 0.35f * dt_) : s_health;
  s_boss = ds_damp(s_boss, h->boss_health, 14, dt_);
  s_boss_trail = s_boss_trail > s_boss ? fmaxf(s_boss, s_boss_trail - 0.25f * dt_) : s_boss;
  if (h->mode != HUD_PLAY && h->mode != HUD_PAUSE) { s_health = s_trail = h->health; s_buddy = h->buddy_health; s_jet = h->jet; }
  for (int i = 0; i < 2; i++) {
    const hud_player_t *p = &h->pl[i];
    sp_health[i] = ds_damp(sp_health[i], p->health, 14, dt_);
    sp_jet[i] = ds_damp(sp_jet[i], p->jet, 16, dt_);
    sp_trail[i] = sp_trail[i] > sp_health[i] ? fmaxf(sp_health[i], sp_trail[i] - 0.35f * dt_) : sp_health[i];
    if (h->mode != HUD_PLAY && h->mode != HUD_PAUSE) { sp_health[i] = sp_trail[i] = p->health; sp_jet[i] = p->jet; }
    if (h->mode != HUD_PLAY) sp_was_scoped[i] = false;
  }

  hg_begin();
  if (h->split) {   /* the seam between the halves */
    hg_rect(DS_W / 2 - 4, 0, 8, DS_H, C_INK, 1);
    hg_rect(DS_W / 2 - 1, 0, 2, DS_H, C_HIVIS, 0.7f);
  }
  switch (h->mode) {
    case HUD_TITLE: scr_title(h, dt_); break;
    case HUD_INTRO: scr_intro(h); break;
    case HUD_PLAY:
      if (h->split) {
        play_split(h);
        play_objective(h);
        if (h->boss) play_boss(h);
        play_banner(h);
        break;
      }
      if (h->scoped) { scope_draw(h); hurt_fx(h); play_banner(h); break; }
      play_centre(h);
      objective_marker(h);
      hurt_fx(h);
      play_vitals(h); play_weapon(h); play_objective(h); play_stats(h);
      if (h->boss) play_boss(h);
      play_banner(h);
      play_strip(h);
      { bool w = play_wcard(h), l = play_lowjet(h); if (!w && !l) play_firsttime(h); }
      play_interact(h);
      if (h->join_hint) hud_join_hint(104);
      break;
    case HUD_PAUSE: scr_pause(h); break;
    case HUD_DEAD: scr_dead(h); break;
    case HUD_CLEAR: scr_clear(h); break;
    case HUD_END: scr_end(h); break;
    case HUD_CINE: break;
  }
  hud_cine_draw(h);   /* captions, letterbox and the radio go over everything */
  hg_end();
  t3_renderer_reset_bindings(DS_R);
}
