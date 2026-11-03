/* hud_int.h: the HUD's private pieces. Painted layers (cv text and shapes,
 * redrawn only when their key changes, uploaded with glTexSubImage2D) and the
 * little GL program that composites them over the frame, plus procedural
 * shapes (bars, stripes, vignettes, the scope mask) that cost no CPU paint. */
#ifndef DS_HUD_INT_H
#define DS_HUD_INT_H
#include "hud.h"
#include "cv.h"
#include <stdio.h>
#include <string.h>

typedef struct { unsigned tex; int w, h; uint32_t key; bool ok; } layer_t;

/* a layer's canvas: NULL when the key is unchanged (draw the old texture), else
 * a cleared w x h region of the shared scratch canvas to paint, then layer_end */
cv *layer_begin(layer_t *L, int w, int h, uint32_t key);
void layer_end(layer_t *L, cv *c);
uint32_t hash_str(const char *s, uint32_t h);

void hg_init(void);
void hg_begin(void);
void hg_end(void);
void hg_tex(const layer_t *L, float x, float y, float alpha);
void hg_tex_m(const layer_t *L, const float m[6], float alpha, uint32_t tint);
void hg_sub(const layer_t *L, int sx, int sy, int sw, int sh, float x, float y, float alpha);
void hg_rect(float x, float y, float w, float h, uint32_t rgb, float a);
void hg_rect_m(float cx, float cy, float w, float h, float ang, uint32_t rgb, float a);   /* centred, rotated */
void hg_grad(float cx, float cy, float w, float h, float ang, uint32_t rgb, float a0, float a1);   /* alpha a0 at the left to a1 at the right */
void hg_scope(float cx, float cy, float r);
void hg_circle(float cx, float cy, float r, uint32_t rgb, float a);
void hg_vignette(uint32_t rgb, float a, float inner);
void hg_vgrad(float y0, float y1, uint32_t rgb, float a0, float a1);    /* full width, alpha a0 at y0 to a1 at y1 */
void hg_stripes(float x, float y, float w, float h, uint32_t c1, float a1, uint32_t c2, float a2, float period, float scroll);
void hg_halftone(float x, float y, float w, float h, uint32_t rgb, float a, float cell, bool flip);
void hg_scan(float a);
void hg_shine(const layer_t *L, float x, float y, float pos, float width, float alpha);
/* place a layer: texel (ox,oy) lands on (cx,cy), turned by ang and scaled */
void mat_place(float m[6], float cx, float cy, float ang, float ox, float oy, float sx, float sy);

/* paint helpers (layer space) */
void p_text(cv *c, int font, float px, float ls, int align, const char *s, float x, float y, uint32_t rgb, float a);
void p_plate(cv *c, float x, float y, float w, float h, float cut, uint32_t rgb, float a);
void p_hazard(cv *c, float x, float y, float w, float h, float period, uint32_t c1, uint32_t c2);
/* a strip of label-maker tape (cut square at the left, angled at the right): text on a tinted band, returns its width */
float p_tape(cv *c, float x, float y, float h, int font, float px, float ls, const char *s, uint32_t bg, uint32_t fg);
void p_cut(cv *c, float x, float y, float w, float h);   /* stencil gap: erases */
void p_wrap(cv *c, const char *s, float x, float y, float w, float lh, uint32_t rgb);   /* word wrap, returns nothing */
const char *key_name(const hud_t *h, const char *pad, const char *kb);

static inline float hud_ease_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }
static inline void hud_fmt_time(char *b, size_t n, float s) {   /* 02:31.4 */
  int t = (int)((s > 0 ? s : 0) * 10);
  snprintf(b, n, "%02d:%02d.%d", t / 600, t / 10 % 60, t % 10);
}
static inline void hud_fmt_int(char *b, size_t n, int v) {      /* 12,400 */
  char t[24]; snprintf(t, sizeof t, "%d", v < 0 ? 0 : v);
  int l = (int)strlen(t), o = 0;
  for (int i = 0; i < l && o < (int)n - 2; i++) { if (i && (l - i) % 3 == 0) b[o++] = ','; b[o++] = t[i]; }
  b[o] = 0;
}
/* the speaker's colour for captions and the radio (cutscene.h names) */
uint32_t hud_speaker_col(const char *who);

/* control glyphs (hud_glyph.c): see the top of that file for the spec strings */
const char *g_act(const char *name, bool pad);   /* an action's spec on a device: "MOVE", "JUMP", "FIRE", ... */
float g_glyphs(cv *c, float x, float cy, float h, const char *spec, bool draw);   /* returns the width; draw false just measures */
/* text with glyphs inline: {ACTION} or {=spec}; align CV_LEFT / CV_CENTER / CV_RIGHT about x; returns the width */
float g_markup(cv *c, float x, float base, int align, int font, float px, float ls, uint32_t col, float gh, bool pad, const char *s, bool draw);
/* the controls diagram for one device: two columns of glyphs and names, 6 rows of row_h, returns its height */
float ctl_panel(cv *c, float x, float y, float w, float row_h, bool pad, const char *title);

/* screens (hud_screens.c) */
void scr_title(const hud_t *h, float dt);
void scr_intro(const hud_t *h);
void scr_pause(const hud_t *h);
void scr_dead(const hud_t *h);
void scr_clear(const hud_t *h);
void scr_end(const hud_t *h);
void scr_dim(float a);   /* shared: dim and scanline backdrop */

extern void render_target(void);
#endif
