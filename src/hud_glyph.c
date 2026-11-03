/* hud_glyph.c: control glyphs painted with cv. Round pad buttons in their
 * standard colours, shoulder shapes, stick and d-pad icons, keyboard keycaps
 * and a mouse with the right button lit. A control is named once by action
 * (g_act) and each device has its own spec string, so every prompt in the
 * game says the right thing for the pad or the keyboard. Specs are tokens
 * joined by '|': A B X Y LB RB LT RT LS RS DUD DLR SEL START, k:LABEL for a
 * keycap, m:L m:R m:W m:M for the mouse (left, right, wheel, moving). */
#include "hud_int.h"

#define INK 0x101218u

typedef struct { const char *name, *pad, *kb; } act_t;
static const act_t ACTS[] = {
  {"MOVE",   "LS",        "k:W|k:A|k:S|k:D"},
  {"LOOK",   "RS",        "m:M|k:ARROWS"},
  {"JUMP",   "A",         "k:SPACE"},
  {"FIRE",   "RT",        "m:L|k:J|k:F"},
  {"FIREC",  "RT",        "m:L|k:J"},
  {"AIM",    "LT",        "m:R|k:K"},
  {"DODGE",  "B",         "k:SHIFT"},
  {"IRON",   "X",         "k:E"},
  {"LIPO",   "Y",         "k:Q|k:G"},
  {"WEAPON", "LB|RB",     "k:1-4|k:TAB|m:W"},
  {"WEAPONC","LB|RB",     "k:1-4"},
  {"ZOOM",   "DUD|LB|RB", "k:R|k:V|m:W"},
  {"VIEW",   "SEL",       "k:C"},
  {"PAUSE",  "START",     "k:ESC|k:P"},
  {"PICK",   "A",         "k:ENTER"},
  {"BACK",   "B",         "k:ESC"},
  {"SKIP",   "A|START",   "k:ENTER"},
  {"LEFTRIGHT", "DLR",    "k:LEFT|k:RIGHT"},
  {"UPDOWN", "DUD",       "k:UP|k:DOWN"},
};

const char *g_act(const char *name, bool pad) {
  for (size_t i = 0; i < sizeof ACTS / sizeof *ACTS; i++) if (!strcmp(ACTS[i].name, name)) return pad ? ACTS[i].pad : ACTS[i].kb;
  return "";
}

static void path_rr(cv *c, float x, float y, float w, float h, float r) { cv_begin(c); cv_round_rect(c, x, y, w, h, r); }
static void txt_mid(cv *c, int font, float px, float ls, const char *s, float cx, float cy, uint32_t col) {
  p_text(c, font, px, ls, CV_CENTER, s, cx, cy + px * 0.36f, col, 1);
}

/* one token; returns its width */
static float tok(cv *c, float x, float cy, float h, const char *t, bool draw) {
  if (!strcmp(t, "A") || !strcmp(t, "B") || !strcmp(t, "X") || !strcmp(t, "Y")) {
    if (draw) {
      uint32_t col = t[0] == 'A' ? 0x5fbf6a : t[0] == 'B' ? 0xe8433a : t[0] == 'X' ? 0x4a8fe8 : 0xf0c43a;
      cv_begin(c); cv_arc(c, x + h / 2, cy, h / 2, 0, 6.2832f, false); cv_fill_hex(c, INK, 1); cv_fill(c);
      cv_begin(c); cv_arc(c, x + h / 2, cy, h / 2 - 2, 0, 6.2832f, false); cv_fill_hex(c, col, 1); cv_fill(c);
      char b[2] = {t[0], 0}; txt_mid(c, F_DISPLAY, h * 0.52f, 0, b, x + h / 2, cy, INK);
    }
    return h;
  }
  if (!strcmp(t, "LB") || !strcmp(t, "RB")) {   /* a flat bumper */
    float w = h * 1.7f, ph = h * 0.62f;
    if (draw) { path_rr(c, x, cy - ph / 2, w, ph, ph * 0.4f); cv_fill_hex(c, 0xcfd2d8, 1); cv_fill(c); txt_mid(c, F_DISPLAY, h * 0.38f, 0, t, x + w / 2, cy, INK); }
    return w;
  }
  if (!strcmp(t, "LT") || !strcmp(t, "RT")) {   /* a deeper, darker trigger */
    float w = h * 1.7f, ph = h * 0.86f;
    if (draw) {
      path_rr(c, x, cy - ph / 2, w, ph, ph * 0.38f); cv_fill_hex(c, 0xcfd2d8, 1); cv_fill(c);
      path_rr(c, x + 2, cy - ph / 2 + 2, w - 4, ph - 4, ph * 0.32f); cv_fill_hex(c, 0x3b4252, 1); cv_fill(c);
      txt_mid(c, F_DISPLAY, h * 0.38f, 0, t, x + w / 2, cy, C_CREAM);
    }
    return w;
  }
  if (!strcmp(t, "LS") || !strcmp(t, "RS")) {
    if (draw) {
      float r = h / 2;
      cv_begin(c); cv_arc(c, x + r, cy, r, 0, 6.2832f, false); cv_fill_hex(c, 0xcfd2d8, 1); cv_fill(c);
      cv_begin(c); cv_arc(c, x + r, cy, r - 2.5f, 0, 6.2832f, false); cv_fill_hex(c, 0x2b3040, 1); cv_fill(c);
      cv_begin(c); cv_arc(c, x + r, cy, r * 0.58f, 0, 6.2832f, false); cv_fill_hex(c, 0x586070, 1); cv_fill(c);
      char b[2] = {t[0], 0}; txt_mid(c, F_DISPLAY, h * 0.36f, 0, b, x + r, cy, C_CREAM);
    }
    return h;
  }
  if (!strcmp(t, "DUD") || !strcmp(t, "DLR")) {
    if (draw) {
      bool ud = t[1] == 'U';
      float a = h * 0.34f, o = (h - a) / 2;
      cv_fill_hex(c, 0x586070, 1); cv_fill_rect(c, x, cy - a / 2, h, a); cv_fill_rect(c, x + o, cy - h / 2, a, h);
      cv_fill_hex(c, C_HIVIS, 1);
      if (ud) { cv_fill_rect(c, x + o, cy - h / 2, a, o + a * 0.2f); cv_fill_rect(c, x + o, cy + a / 2 - a * 0.2f, a, o + a * 0.2f); }
      else { cv_fill_rect(c, x, cy - a / 2, o + a * 0.2f, a); cv_fill_rect(c, x + h - o - a * 0.2f, cy - a / 2, o + a * 0.2f, a); }
    }
    return h;
  }
  if (!strcmp(t, "SEL") || !strcmp(t, "START")) {
    const char *s = t[0] == 'S' && t[1] == 'E' ? "SELECT" : "START";
    float px = h * 0.34f;
    cv_font(c, F_DISPLAY, px); cv_letter_spacing(c, 1);
    float w = cv_measure(c, s) + h * 0.6f, ph = h * 0.58f;
    if (draw) { path_rr(c, x, cy - ph / 2, w, ph, ph / 2); cv_fill_hex(c, 0x586070, 1); cv_fill(c); txt_mid(c, F_DISPLAY, px, 1, s, x + w / 2 + 0.5f, cy, C_CREAM); }
    return w;
  }
  if (t[0] == 'k' && t[1] == ':') {
    const char *s = t + 2;
    int n = (int)strlen(s);
    float px = n <= 1 ? h * 0.5f : n <= 3 ? h * 0.42f : h * 0.37f;
    cv_font(c, F_MONO, px); cv_letter_spacing(c, 0);
    float w = fmaxf(h, cv_measure(c, s) + h * 0.7f), top = cy - h / 2;
    if (draw) {
      path_rr(c, x, top + h * 0.08f, w, h * 0.92f, h * 0.2f); cv_fill_hex(c, 0xa89e84, 1); cv_fill(c);   /* the key's side */
      path_rr(c, x, top, w, h * 0.88f, h * 0.2f); cv_fill_hex(c, C_CREAM, 1); cv_fill(c);
      txt_mid(c, F_MONO, px, 0, s, x + w / 2, top + h * 0.44f, INK);
    }
    return w;
  }
  if (t[0] == 'm' && t[1] == ':') {
    char k = t[2];
    float bw = h * 0.62f, extra = k == 'M' ? h * 0.5f : 0, w = bw + extra * 2;
    if (draw) {
      float bx = x + extra, top = cy - h / 2, split = top + h * 0.42f;
      path_rr(c, bx, top, bw, h, bw / 2); cv_fill_hex(c, C_CREAM, 1); cv_fill(c);
      cv_save(c); path_rr(c, bx, top, bw, h, bw / 2); cv_clip(c);
      cv_fill_hex(c, C_HIVIS, 1);
      if (k == 'L') cv_fill_rect(c, bx, top, bw / 2, split - top);
      if (k == 'R') cv_fill_rect(c, bx + bw / 2, top, bw / 2, split - top);
      cv_restore(c);
      cv_stroke_hex(c, INK, 1); cv_line_width(c, 1.6f);
      path_rr(c, bx, top, bw, h, bw / 2); cv_stroke(c);
      cv_begin(c); cv_move(c, bx, split); cv_line(c, bx + bw, split); cv_move(c, bx + bw / 2, top); cv_line(c, bx + bw / 2, split); cv_stroke(c);
      if (k == 'W') { path_rr(c, bx + bw / 2 - h * 0.07f, top + h * 0.08f, h * 0.14f, h * 0.28f, h * 0.07f); cv_fill_hex(c, C_HIVIS, 1); cv_fill(c); cv_stroke(c); }
      if (k == 'M') {   /* motion chevrons either side */
        cv_stroke_hex(c, C_HIVIS, 1); cv_line_width(c, 2.2f); cv_line_cap(c, CV_CAP_ROUND);
        cv_begin(c); cv_move(c, x + extra - 3, cy - h * 0.14f); cv_line(c, x + 3, cy); cv_line(c, x + extra - 3, cy + h * 0.14f); cv_stroke(c);
        cv_begin(c); cv_move(c, x + w - extra + 3, cy - h * 0.14f); cv_line(c, x + w - 3, cy); cv_line(c, x + w - extra + 3, cy + h * 0.14f); cv_stroke(c);
      }
    }
    return w;
  }
  return 0;
}

float g_glyphs(cv *c, float x, float cy, float h, const char *spec, bool draw) {
  float x0 = x;
  char t[24]; int n = 0;
  for (const char *p = spec;; p++) {
    if (*p == '|' || !*p) {
      t[n] = 0; n = 0;
      if (t[0]) x += tok(c, x, cy, h, t, draw);
      if (!*p) break;
      float sp = h * 0.22f;
      cv_font(c, F_MONO, h * 0.5f); cv_letter_spacing(c, 0);
      if (draw) p_text(c, F_MONO, h * 0.5f, 0, CV_LEFT, "/", x + sp, cy + h * 0.18f, 0x9aa1b2, 1);
      x += sp * 2 + cv_measure(c, "/");
    } else if (n < 22) t[n++] = *p;
  }
  return x - x0;
}

/* text with glyphs inline: {ACTION} is that action's glyphs on this device, {=spec} a literal spec */
float g_markup(cv *c, float x, float base, int align, int font, float px, float ls, uint32_t col, float gh, bool pad, const char *s, bool draw) {
  float cy = base - px * 0.36f;
  if (draw && align != CV_LEFT) {
    float w = g_markup(c, 0, base, CV_LEFT, font, px, ls, col, gh, pad, s, false);
    x -= align == CV_CENTER ? w / 2 : w;
  }
  float x0 = x;
  const char *p = s;
  while (*p) {
    if (*p == '{') {
      const char *e = strchr(p, '}'); if (!e) break;
      char nm[40]; int k = (int)(e - p - 1); if (k > 39) k = 39; memcpy(nm, p + 1, (size_t)k); nm[k] = 0;
      const char *spec = nm[0] == '=' ? nm + 1 : g_act(nm, pad);
      x += g_glyphs(c, x, cy, gh, spec, draw) + 6;
      p = e + 1;
    } else {
      char seg[160]; int k = 0;
      while (*p && *p != '{' && k < 159) seg[k++] = *p++;
      seg[k] = 0;
      cv_font(c, font, px); cv_letter_spacing(c, ls);
      if (draw) p_text(c, font, px, ls, CV_LEFT, seg, x, base, col, 1);
      x += cv_measure(c, seg);
    }
  }
  return x - x0;
}

/* ── the controls diagram ── */
float ctl_panel(cv *c, float x, float y, float w, float row_h, bool pad, const char *title) {
  static const char *LAB[12] = {"MOVE", "LOOK", "JUMP / JET", "FIRE", "AIM / SCOPE", "DODGE",
                                "IRON / USE", "THROW LIPO", "WEAPON", "SCOPE ZOOM", "VIEW", "PAUSE"};
  static const char *ACT[12] = {"MOVE", "LOOK", "JUMP", "FIRE", "AIM", "DODGE", "IRON", "LIPO", "WEAPON", "ZOOM", "VIEW", "PAUSE"};
  float hh = 40 + 6 * row_h + 6;
  p_plate(c, x, y, w, hh, 14, C_INK, 0.9f);
  p_hazard(c, x, y + 14, 8, hh - 14, 12, pad ? C_HIVIS : C_TAN, C_INK);
  p_tape(c, x + 22, y + 8, 22, F_DISPLAY, 14, 3, title, pad ? C_HIVIS : C_TAN, C_INK);
  float colw = (w - 30) / 2, gh = row_h * 0.8f;
  for (int i = 0; i < 12; i++) {
    float cx = x + 22 + (i / 6) * colw, ry = y + 44 + (i % 6) * row_h, cy = ry + row_h / 2;
    p_text(c, F_MONO, 15, 0, CV_LEFT, LAB[i], cx, cy + 5, 0xd7d0bf, 1);
    g_glyphs(c, cx + 118, cy, gh, g_act(ACT[i], pad), true);
    if (i % 6) { cv_fill_hex(c, C_CREAM, 0.08f); cv_fill_rect(c, cx, ry, colw - 18, 1); }
  }
  return hh;
}
