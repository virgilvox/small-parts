/* audio.c: the synth. No samples. A voice pool for effects (oscillator or
 * noise, an exponential pitch glide, a decay curve, a swept one pole filter,
 * a little FM and an echo send), two continuous voices that follow the game
 * (the jetpack's motors, the coilgun's charge), a stereo echo bus, and a 16
 * step tracker for the music moods (bass, pad, arp, lead, kick, snare, hat).
 * The synth runs at whatever rate the host asks for, so there is nothing to
 * resample. */
#include "audio.h"
#include "voice.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>

#define NV 28
#define TAU 6.28318530718f
enum { W_SINE, W_SQ, W_SAW, W_TRI, W_NOISE };

static float SR = 48000;
static uint32_t rs = 0x12345678u;
static float rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (float)(rs & 0xffffff) / 8388608.f - 1.f; }
static float sgn_frac(float x) { return x - floorf(x); }

static float osc(int w, float ph, float duty) {
  switch (w) {
    case W_SINE: return sinf(ph * TAU);
    case W_SQ: return (ph < duty ? 0.7f : -0.7f) - (2.f * duty - 1.f) * 0.7f;   /* no DC for thin pulses */
    case W_SAW: return ph * 2.f - 1.f;
    case W_TRI: return ph < 0.5f ? ph * 4.f - 1.f : 3.f - ph * 4.f;
    default: return rnd();
  }
}

/* ── effect voices ── */
typedef struct {
  bool on; int wave, curve, id;
  float f, mult, dur, age, delay, atk, vol, gl, gr, noise, duty, send;
  float lp, lp1, hp, fm_r, fm_i, ph, mph, lz, hz, drive, wob;
} voice;
static voice vox[NV];

static float pan_clamp(float p) { return p < -1 ? -1 : p > 1 ? 1 : p; }

/* start a voice: wave, start and end frequency, duration, volume, pan */
static voice *vx(int id, int wave, float f0, float f1, float dur, float vol, float pan) {
  int k = 0; float worst = -1;
  for (int i = 0; i < NV; i++) {
    if (!vox[i].on) { k = i; break; }
    float r = vox[i].age / vox[i].dur;
    if (r > worst) { worst = r; k = i; }
  }
  voice *v = &vox[k];
  memset(v, 0, sizeof *v);
  v->on = true; v->id = id; v->wave = wave; v->f = f0; v->dur = dur;
  v->mult = (f0 > 0 && f1 > 0) ? powf(f1 / f0, 1.f / (dur * SR)) : 1.f;
  v->vol = vol; v->curve = 2; v->duty = 0.5f; v->atk = 0.002f;
  float a = (pan_clamp(pan) + 1.f) * 0.78539816f;
  v->gl = cosf(a); v->gr = sinf(a);
  return v;
}

/* ── echo bus ── */
#define ECH 32768
static float eb[2][ECH]; static int ew; static float elp[2];

/* ── music ── */
typedef struct {
  float bpm; int root, nch; const char *chords;   /* chord roots as semitone letters 'a'+n, then quality in quals */
  const char *quals;
  const char *bass, *kick, *snare, *hat, *arp, *lead;   /* 16 step patterns; lead is 64 */
  int major, answer, bars_once;                          /* bars_once: play this many bars and stop (jingle) */
  float v_bass, v_pad, v_arp, v_lead, v_drum;
  int bass_w, lead_w, lead_shift;                        /* bass: 0 saw 1 square 2 tri; lead: 0 soft, 1 pulse, 2 saws; shift in semitones */
  float bass_lp, drive, bass_dec, arp_dec;               /* per sample decay of a bass note and an arp ping */
  float snare_dec, snare_f;                              /* 0 = the default rattle; a bigger decay and a higher pitch make a tight toy snare */
} song;

/* patterns: bass r root, f fifth, o octave, s b7, t third; arp digits 0..5 chord tones (root, 3rd, 5th, then octave);
 * lead digits 0..9 a..f pentatonic degrees, '-' holds, '.' rests */
static const song SONGS[6] = {
  /* TITLE: a late night bench, slow, minor, a wide pad and a lonely lead; Dm Bb F C Dm Bb Gm A */
  { 84, 38, 8, "aidkaifh", "mMMMmMmM",
    "r.......r.f.....", "x.......x.......", "................", "..x...x...x...x.", "0.2.1.2.0.2.4.2.",
    "7---5---4---3---" "5---4---2---0---" "4---5---7-------" "9---7---5---4---",
    0, 0, 0, 0.8f, 0.6f, 0.25f, 0.5f, 0.45f, 2, 0, 0, 520, 0.f, 0.00006f, 0.0003f, 0, 0 },
  /* EXPLORE: the lab after hours, a pedal bass, ticks like machines cooling, notes held too long */
  { 98, 40, 8, "aabaakba", "mmMmmMMm",
    "r...r.r.r...r.f.", "x.......x.x.....", "........x.......", "x...x...x...x.x.", "0.....2...1.....",
    "..4-------3-----" "..5-----4-------" "..4-----2-------" "3-----------0---",
    0, 3, 0, 0.8f, 0.45f, 0.28f, 0.5f, 0.32f, 1, 0, 0, 700, 0.f, 0.0002f, 0.0006f, 0, 0 },
  /* FIGHT: fast driving eighths, a running arp and a toy soldier snare march on top */
  { 152, 33, 8, "aidkaikh", "mMMMmMMM",
    "rrorrorrrorrorrf", "x...x...x...x...", "x.x.xxx.x.x.xxxx", "..x...x...x...x.", "0202121202021210",
    "4-3-4-5-7-5-4-3-" "5-4-5-7-9-7-5-4-" "7-9-7-5-4-5-7---" "9-7-5-4-3-4-5---",
    0, 2, 0, 0.7f, 0.25f, 0.4f, 0.5f, 0.6f, 2, 1, 0, 1800, 0.25f, 0.0004f, 0.0009f, 0.0032f, 270.f },
  /* BOSS: heavy and slower, a saw bass pushed into clipping, a stomping march and a siren lead */
  { 116, 31, 8, "aabaaakb", "mmMmmmMM",
    "r..rr..r..rr.s..", "x..x..x...x.x...", "x...x.x.x...x.xx", "x...x...x...x...", "0.....0.0.....0.",
    "0-------1-------" "0-------3-------" "0---1---0-----3-" "4---------------",
    0, 2, 0, 0.95f, 0.45f, 0.45f, 0.5f, 0.85f, 0, 1, -12, 900, 0.7f, 0.00025f, 0.0006f, 0.0022f, 215.f },
  /* CLEAR: a short major fanfare, played once */
  { 132, 36, 4, "ahfa", "MMMM",
    "r.o.f.o.r.o.f.o.", "x...x...x...x...", "....x.......x...", "x.x.x.x.x.x.x.x.", "0.1.2.3.2.1.2.3.",
    "5.7.8.a---a-8-7-" "8.8.7.8.a-------" "a.8.a.c---c-a-8-" "d---------------",
    1, 0, 4, 0.6f, 0.45f, 0.4f, 0.65f, 0.7f, 2, 2, 0, 1800, 0.f, 0.0003f, 0.0008f, 0, 0 },
  /* CINE: slow and wide under the cutscenes, a pad, one ping at a time, almost no drums */
  { 68, 45, 8, "aidkaidk", "mMMMmMMM",
    "r.......r.......", "................", "................", "..............x.", "0.....2.....1...",
    "7---------------" "................" "5-------4-------" "................",
    0, 0, 0, 0.7f, 0.75f, 0.3f, 0.35f, 0.3f, 2, 0, 0, 480, 0.f, 0.00005f, 0.0002f, 0, 0 },
};

typedef struct {   /* a note voice for the sequencer */
  float f, ph, env, rate, z, ph2;
} nv;
static int cur = -1, want = MOOD_SILENT;
static float mgain, mtarget = 1, music_on = 1;
static double step_ctr; static int step, bar;
static bool once_done;
static nv bass_v, lead_v, arp_v, pad_v[3], kick_v, snare_v, hat_v;
static float pad_z[2], lead_hold, vibt, hat_open;

static float mtof(float n) { return 440.f * powf(2.f, (n - 69.f) / 12.f); }
static float pent_min[5] = {0, 3, 5, 7, 10}, pent_maj[5] = {0, 2, 4, 7, 9};
static int degree_semi(int d, int major) { return (d / 5) * 12 + (major ? pent_maj : pent_min)[d % 5]; }
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static void song_reset(void) {
  step = 0; bar = 0; step_ctr = 0; once_done = false;
  memset(&bass_v, 0, sizeof bass_v); memset(&lead_v, 0, sizeof lead_v); memset(&arp_v, 0, sizeof arp_v);
  memset(pad_v, 0, sizeof pad_v); memset(&kick_v, 0, sizeof kick_v); memset(&snare_v, 0, sizeof snare_v); memset(&hat_v, 0, sizeof hat_v);
  pad_z[0] = pad_z[1] = 0; lead_hold = 0;
}

static int chord_root(const song *s, int b) { return s->chords[b % s->nch] - 'a'; }
static int chord_third(const song *s, int b) { return s->quals[b % s->nch] == 'M' ? 4 : 3; }

static void trigger(const song *s) {
  int st = step & 15, b = bar;
  int cr = chord_root(s, b), ct = chord_third(s, b);
  float root = (float)(s->root + cr);
  char bc = s->bass[st];
  if (bc != '.') {
    int off = bc == 'f' ? 7 : bc == 'o' ? 12 : bc == 's' ? 10 : bc == 't' ? ct : 0;
    bass_v.f = mtof(root + 12 + off);   /* an octave up, so small speakers carry it */ bass_v.env = 1; bass_v.rate = 0;
  }
  char ac = s->arp[st];
  if (ac != '.') {
    int d = ac - '0'; int tone = d % 3, oct = d / 3;
    int off = tone == 0 ? 0 : tone == 1 ? ct : 7;
    arp_v.f = mtof(root + 24 + off + oct * 12); arp_v.env = 1;
  }
  if (s->kick[st] == 'x') { kick_v.env = 1; kick_v.f = 0; kick_v.ph = 0; }
  if (s->snare[st] == 'x') snare_v.env = 1;
  if (s->hat[st] == 'x') { hat_v.env = (st & 3) == 2 ? 1.f : 0.7f; hat_open = (st == 14) ? 1.f : 0.f; }
  /* lead: the 64 step line, answered a few degrees higher on the second four bars */
  int idx = (b & 3) * 16 + st;
  char lc = s->lead[idx];
  int d = hexv(lc);
  if (d >= 0) {
    d += ((b >> 2) & 1) * s->answer;
    lead_v.f = mtof((float)(s->root + 24 + s->lead_shift + degree_semi(d, s->major)));
    vibt = 0; lead_hold = 1;
  } else if (lc == '.') lead_hold = 0;
  if (cur == MOOD_CLEAR && st == 0 && b == 3) hat_v.env = 1;
}

/* sequencer, advance one sample; returns the stereo music sample into m[2] and the echo send */
static void music_sample(const song *s, float *mL, float *mR, float *snd) {
  if (step_ctr <= 0) {
    if (!once_done) {
      trigger(s);
    }
    step_ctr += SR * 60.0 / s->bpm / 4.0;
    step++;
    if ((step & 15) == 0) {
      bar++;
      if (s->bars_once && bar >= s->bars_once) { once_done = true; mtarget = 0; }
    }
  }
  step_ctr -= 1;
  if (once_done && mgain < 0.002f) { *mL = *mR = *snd = 0; return; }
  float dt = 1.f / SR;
  int b = step > 0 ? (step - 1) >> 4 : 0;   /* the bar of the step just triggered */
  int cr = chord_root(s, b), ct = chord_third(s, b);
  float root = (float)(s->root + cr);
  float L = 0, R = 0, sd = 0;

  /* bass: saw and sub through a one pole whose cutoff follows the note */
  if (bass_v.env > 0.0005f) {
    bass_v.ph = sgn_frac(bass_v.ph + bass_v.f * dt);
    float w = s->bass_w == 1 ? osc(W_SQ, bass_v.ph, 0.5f) : s->bass_w == 2 ? osc(W_TRI, bass_v.ph, 0) : osc(W_SAW, bass_v.ph, 0);
    bass_v.ph2 = sgn_frac(bass_v.ph2 + bass_v.f * 0.5f * dt);
    float x = w * 0.55f + osc(W_SINE, bass_v.ph2, 0) * 0.65f;
    float cut = s->bass_lp * (0.35f + 0.65f * bass_v.env);
    bass_v.z += (x - bass_v.z) * fminf(1.f, cut * TAU * dt);
    float y = bass_v.z;
    if (s->drive > 0) y = tanhf(y * (1.f + s->drive * 5.f)) * 0.8f;
    y *= bass_v.env * s->v_bass;
    L += y; R += y;
    bass_v.env *= 1.f - s->bass_dec * (48000.f / SR);
  }
  /* pad: a triad of detuned saws, slow to follow the chord */
  {
    float fr[3] = {mtof(root + 12), mtof(root + 12 + ct), mtof(root + 19)};
    float acc = 0;
    for (int i = 0; i < 3; i++) {
      pad_v[i].f += (fr[i] - pad_v[i].f) * 0.0008f * (48000.f / SR);
      if (pad_v[i].f < 20) pad_v[i].f = fr[i];
      pad_v[i].ph = sgn_frac(pad_v[i].ph + pad_v[i].f * dt);
      pad_v[i].ph2 = sgn_frac(pad_v[i].ph2 + pad_v[i].f * 1.006f * dt);
      acc += osc(W_SAW, pad_v[i].ph, 0) + osc(W_SAW, pad_v[i].ph2, 0);
    }
    pad_z[0] += (acc - pad_z[0]) * 0.035f * (48000.f / SR);
    pad_z[1] += (pad_z[0] - pad_z[1]) * 0.035f * (48000.f / SR);
    float y = pad_z[1] * s->v_pad * 0.14f;
    L += y * 0.9f; R += y * 1.0f; sd += y * 0.2f;
  }
  /* arp: a thin pulse that pings and fades, alternating sides */
  if (arp_v.env > 0.0005f) {
    arp_v.ph = sgn_frac(arp_v.ph + arp_v.f * dt);
    float y = osc(W_SQ, arp_v.ph, 0.25f) * arp_v.env * s->v_arp * 0.3f;
    L += y * 0.8f; R += y * 0.5f; sd += y * 0.35f;
    arp_v.env *= 1.f - s->arp_dec * (48000.f / SR);
  }
  /* lead: a pulse with late vibrato, held until the next note or rest */
  if (lead_hold > 0 || lead_v.env > 0.001f) {
    vibt += dt;
    float vib = 1.f + 0.006f * sinf(vibt * 38.f) * fminf(1.f, vibt * 3.f);
    lead_v.ph = sgn_frac(lead_v.ph + lead_v.f * vib * dt);
    lead_v.ph2 = sgn_frac(lead_v.ph2 + lead_v.f * vib * 1.004f * dt);
    float w = s->lead_w == 2 ? osc(W_SAW, lead_v.ph, 0) + osc(W_SAW, lead_v.ph2, 0) : s->lead_w == 1 ? osc(W_SQ, lead_v.ph, 0.4f) * 1.2f + osc(W_SAW, lead_v.ph2, 0) * 0.5f : osc(W_TRI, lead_v.ph, 0) * 1.4f + osc(W_SINE, lead_v.ph2, 0);
    lead_v.z += (w - lead_v.z) * 0.22f * (48000.f / SR);
    float target = lead_hold > 0 ? 1.f : 0.f;
    lead_v.env += (target - lead_v.env) * (target > lead_v.env ? 0.004f : 0.0012f) * (48000.f / SR);
    float y = lead_v.z * lead_v.env * s->v_lead * 0.22f;
    L += y * 0.8f; R += y * 0.9f; sd += y * 0.5f;
  }
  /* drums */
  float dr = 0;
  if (kick_v.env > 0.0005f) {
    kick_v.f = 55.f + 110.f * kick_v.env * kick_v.env * kick_v.env;
    kick_v.ph = sgn_frac(kick_v.ph + kick_v.f * dt);
    dr += sinf(kick_v.ph * TAU) * kick_v.env * 1.1f;
    kick_v.env *= 1.f - 0.00055f * (48000.f / SR) * 1.f;
    kick_v.env -= 0.00006f * (48000.f / SR);
  }
  if (snare_v.env > 0.0005f) {
    snare_v.ph = sgn_frac(snare_v.ph + (s->snare_f > 0 ? s->snare_f : 185.f) * dt);
    float n = rnd();
    snare_v.z += (n - snare_v.z) * 0.55f;
    dr += ((n - snare_v.z) * 0.8f + osc(W_TRI, snare_v.ph, 0) * 0.4f) * snare_v.env * 0.6f;
    snare_v.env *= 1.f - (s->snare_dec > 0 ? s->snare_dec : 0.0012f) * (48000.f / SR);
  }
  if (hat_v.env > 0.0005f) {
    float n = rnd();
    hat_v.z += (n - hat_v.z) * 0.7f;
    dr += (n - hat_v.z) * hat_v.env * 0.28f;
    hat_v.env *= 1.f - (hat_open > 0 ? 0.0004f : 0.004f) * (48000.f / SR);
  }
  dr *= s->v_drum;
  L += dr; R += dr;
  *mL = L; *mR = R; *snd = sd;
}

/* ── api ── */
static float sfx_vol = 0.9f, mus_vol = 0.5f;

void audio_init(uint32_t rate) {
  SR = rate ? (float)rate : 48000.f;
  memset(vox, 0, sizeof vox); memset(eb, 0, sizeof eb);
  cur = -1; want = MOOD_SILENT; mgain = 0; mtarget = 1; ew = 0;
  voice_init(rate);
}

void music_mood(mood_t m) { want = (int)m; }
void audio_set_music(int on) { music_on = on ? 1.f : 0.f; }

/* ── continuous voices ──
 * The jetpack's motors and the coilgun's charge are called every frame while
 * they last, so they are not pool voices that stack: each call refreshes a
 * slot's gain and time to live, and the render loop runs one oscillator set
 * per slot, fading in and out around the calls. Two jet slots, so the buddy's
 * motors (heard from their side) sit beside the player's. */
typedef struct { float ttl, gain, pan, level, lpan, ph[3], z[3], t; } cont;
static cont jets[2], coil_c;

static cont *cont_slot(cont *arr, int n, float pan) {
  cont *free_ = NULL, *weak = &arr[0];
  for (int i = 0; i < n; i++) {
    if (arr[i].ttl > 0 && fabsf(arr[i].pan - pan) < 0.3f) return &arr[i];
    if (arr[i].ttl <= 0 && !free_) free_ = &arr[i];
    if (arr[i].level < weak->level) weak = &arr[i];
  }
  return free_ ? free_ : weak;
}
static void cont_call(cont *arr, int n, float pan, float gain) {
  cont *c = cont_slot(arr, n, pan);
  if (c->ttl <= 0) c->lpan = pan;
  c->ttl = 0.09f; c->gain = gain; c->pan = pan;
}

/* one sample of a drone motor: two detuned saws that beat, a harmonic, and air through the props; pitch climbs with thrust */
static float jet_sample(cont *c, float dt, int idx, float sd_[1]) {
  float lv = c->level;
  float f = (300.f + 320.f * lv) * (idx ? 1.07f : 1.f);
  c->ph[0] = sgn_frac(c->ph[0] + f * dt);
  c->ph[1] = sgn_frac(c->ph[1] + f * 1.013f * dt);
  c->ph[2] = sgn_frac(c->ph[2] + f * 2.01f * dt);
  float x = osc(W_SAW, c->ph[0], 0) * 0.45f + osc(W_SAW, c->ph[1], 0) * 0.45f + osc(W_TRI, c->ph[2], 0) * 0.25f;
  c->z[0] += (x - c->z[0]) * fminf(1.f, (1500.f + 2400.f * lv) * TAU * dt);
  float n = rnd();
  c->z[1] += (n - c->z[1]) * fminf(1.f, 2400.f * TAU * dt);
  c->t += dt;
  float am = 1.f + 0.12f * sinf(c->t * TAU * 86.f);   /* blade flutter */
  float y = (c->z[0] * 0.55f + c->z[1] * (0.35f + 0.4f * lv)) * am * lv * 0.34f;
  sd_[0] += y * 0.1f;
  return y;
}

/* the coilgun charging: a capacitor whine climbing with the charge, a mains hum under it, crackle near the top */
static float coil_sample(cont *c, float dt, float sd_[1]) {
  float k = c->gain;   /* the charge itself: call gain is 0..1 */
  float f = 180.f + 2700.f * powf(k, 1.6f);
  c->ph[0] = sgn_frac(c->ph[0] + f * dt);
  c->ph[1] = sgn_frac(c->ph[1] + f * 1.5f * dt);
  c->ph[2] = sgn_frac(c->ph[2] + 100.f * dt);
  float w = sinf(c->ph[0] * TAU + (1.2f + 2.f * k) * sinf(c->ph[1] * TAU));
  c->z[0] += (osc(W_SAW, c->ph[2], 0) - c->z[0]) * fminf(1.f, 260.f * TAU * dt);
  float x = w * 0.5f + c->z[0] * 0.5f;
  if (k > 0.5f && rnd() > 1.f - 0.003f * k * k * k) c->z[1] = 1.f;   /* a spark */
  c->z[1] *= 1.f - 220.f * dt;
  x += c->z[1] * rnd() * 0.8f;
  float y = x * c->level * (0.06f + 0.22f * k);
  sd_[0] += y * 0.15f;
  return y;
}

/* ── effects ── */
void sfx(sfx_t s, float pan, float gain) {
  if (s < 0 || s >= SFX_COUNT) return;
  float g = gain <= 0 ? 0 : gain > 1 ? 1 : gain, p = pan;
  voice *v;
  float j = 1.f + rnd() * 0.08f;   /* a little pitch jitter so repeats do not machine gun */
#define V(w, f0, f1, d, vol) vx((int)s, w, f0, f1, d, (vol) * g, p)
  switch (s) {
    case SFX_JET: cont_call(jets, 2, pan, g); break;
    case SFX_COIL_CHARGE: cont_call(&coil_c, 1, pan, g); break;
    case SFX_BAND: {   /* a rubber band repeater: the band's twang over a plywood thock and a snap */
      v = V(W_SINE, 620 * j, 330 * j, 0.2f, 0.5f); v->fm_r = 1.5f; v->fm_i = 2.4f; v->curve = 2; v->wob = 70.f; v->send = 0.15f;
      v = V(W_TRI, 310 * j, 300 * j, 0.14f, 0.3f); v->curve = 2; v->wob = 45.f;
      v = V(W_NOISE, 0, 0, 0.018f, 0.55f); v->hp = 2200; v->lp = 9000; v->curve = 2;
      v = V(W_SINE, 150, 70, 0.07f, 0.55f); v->curve = 2;
      break; }
    case SFX_GLUE: {   /* a wet pop out of the nozzle and a small bubble burst after it */
      v = V(W_SINE, 340 * j, 80, 0.1f, 0.7f); v->curve = 2; v->wob = 40.f;
      v = V(W_NOISE, 0, 0, 0.07f, 0.4f); v->lp = 2600; v->lp1 = 500; v->curve = 3;
      v = V(W_TRI, 260, 1100, 0.06f, 0.35f); v->delay = 0.045f; v->curve = 1;
      v = V(W_NOISE, 0, 0, 0.16f, 0.14f); v->hp = 3500; v->lp = 8000; v->atk = 0.03f; v->curve = 2;
      break; }
    case SFX_COIL: {   /* crack, chest thump, a ringing rail and a long roll of tail */
      v = V(W_NOISE, 0, 0, 0.05f, 0.9f); v->lp = 14000; v->lp1 = 3500; v->curve = 3;
      v = V(W_SINE, 150, 32, 0.55f, 1.0f); v->curve = 2; v->drive = 2.f;
      v = V(W_SAW, 1100, 90, 0.14f, 0.4f); v->lp = 5000; v->lp1 = 400; v->curve = 3;
      v = V(W_NOISE, 0, 0, 1.4f, 0.4f); v->lp = 3200; v->lp1 = 180; v->curve = 3; v->atk = 0.03f; v->send = 0.7f;
      v = V(W_SINE, 2100, 2100, 0.7f, 0.1f); v->fm_r = 1.5f; v->fm_i = 3.f; v->send = 0.7f; v->curve = 3;
      break; }
    case SFX_IRON: {   /* the iron through the air: a hot hiss over a low whoosh */
      v = V(W_NOISE, 0, 0, 0.3f, 0.45f); v->hp = 2600; v->lp = 9500; v->atk = 0.07f; v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.26f, 0.4f); v->lp = 500; v->lp1 = 2200; v->hp = 150; v->atk = 0.09f; v->curve = 2;
      v = V(W_TRI, 210, 120, 0.2f, 0.25f); v->curve = 2;
      break; }
    case SFX_MELT: {   /* plastic meets the iron: a sizzle of tiny crackles and a drip */
      v = V(W_NOISE, 0, 0, 0.55f, 0.3f); v->hp = 4200; v->lp = 11000; v->atk = 0.02f; v->curve = 2; v->wob = 0;
      for (int i = 0; i < 6; i++) { v = V(W_NOISE, 0, 0, 0.008f, 0.55f); v->hp = 1500; v->lp = 7000; v->delay = 0.02f + (float)i * 0.07f + (rnd() + 1.f) * 0.02f; v->curve = 1; }
      v = V(W_SINE, 950, 300, 0.12f, 0.3f); v->delay = 0.22f; v->curve = 2; v->send = 0.2f;
      v = V(W_SINE, 140, 90, 0.18f, 0.4f); v->curve = 2; v->wob = 30.f;
      break; }
    case SFX_LIPO_THROW: {   /* a pouch tossed: a soft whoosh and a flump */
      v = V(W_NOISE, 0, 0, 0.22f, 0.4f); v->lp = 600; v->lp1 = 2400; v->hp = 200; v->atk = 0.06f; v->curve = 2;
      v = V(W_TRI, 330, 520, 0.1f, 0.35f); v->curve = 2;
      v = V(W_SQ, 1400, 1400, 0.03f, 0.3f); v->duty = 0.2f; v->lp = 3500; v->delay = 0.16f; v->curve = 1;   /* the pack's beep */
      break; }
    case SFX_LIPO_PUFF: {   /* the pack swelling: a rising pressurised hiss with a creak */
      v = V(W_NOISE, 0, 0, 0.7f, 0.45f); v->lp = 700; v->lp1 = 4200; v->hp = 300; v->atk = 0.05f; v->curve = 1; v->send = 0.15f;
      v = V(W_SAW, 70, 130, 0.6f, 0.2f); v->lp = 700; v->wob = 18.f; v->curve = 1;
      v = V(W_SINE, 90, 50, 0.12f, 0.5f); v->curve = 2;
      break; }
    case SFX_BOOM: {
      v = V(W_SINE, 90, 22, 1.2f, 1.0f); v->curve = 2; v->drive = 1.5f;
      v = V(W_SAW, 170, 40, 0.8f, 0.4f); v->lp = 600; v->curve = 2; v->drive = 1.f;
      v = V(W_NOISE, 0, 0, 1.3f, 0.7f); v->lp = 3000; v->lp1 = 90; v->curve = 3; v->atk = 0.004f; v->send = 0.4f;
      v = V(W_NOISE, 0, 0, 0.08f, 0.8f); v->hp = 2000; v->lp = 9000; v->curve = 3;
      break; }
    case SFX_PELLET: {   /* an army rifle: a tiny plastic click and pop */
      v = V(W_NOISE, 0, 0, 0.014f, 0.5f); v->hp = 2800; v->lp = 10000; v->curve = 2;
      v = V(W_SINE, 1900 * j, 650, 0.025f, 0.4f); v->curve = 2;
      v = V(W_SQ, 1250 * j, 420, 0.018f, 0.2f); v->duty = 0.3f; v->curve = 1; v->lp = 4000;
      break; }
    case SFX_MG: {   /* the gunner's rattle, one shot of it: lower and snappier than a rifle */
      v = V(W_NOISE, 0, 0, 0.035f, 0.5f); v->lp = 6000; v->lp1 = 1400; v->curve = 3;
      v = V(W_SQ, 280 * j, 110, 0.04f, 0.4f); v->duty = 0.3f; v->curve = 2; v->lp = 2200;
      v = V(W_SINE, 1500 * j, 500, 0.02f, 0.25f); v->curve = 2;
      break; }
    case SFX_PLINK: {   /* a hit on plastic: a bright tick and a short ring */
      v = V(W_SINE, 2300 * j, 1900, 0.09f, 0.4f); v->fm_r = 3.1f; v->fm_i = 1.6f; v->curve = 2; v->send = 0.15f;
      v = V(W_NOISE, 0, 0, 0.01f, 0.5f); v->hp = 3000; v->lp = 11000; v->curve = 2;
      v = V(W_SINE, 880 * j, 700, 0.05f, 0.3f); v->curve = 2;
      break; }
    case SFX_TOPPLE: {   /* a hollow figure falls: three clacks, each smaller than the last, then a rattle */
      static const float f0[3] = {760, 640, 560}, vl[3] = {0.7f, 0.45f, 0.28f}, dl[3] = {0, 0.1f, 0.19f};
      for (int i = 0; i < 3; i++) {
        v = V(W_SINE, f0[i] * j, f0[i] * 0.4f, 0.08f, vl[i]); v->delay = dl[i]; v->curve = 2; v->fm_r = 1.9f; v->fm_i = 0.8f; v->send = 0.12f;
        v = V(W_NOISE, 0, 0, 0.02f, vl[i] * 0.8f); v->delay = dl[i]; v->lp = 4500; v->lp1 = 900; v->curve = 3;
      }
      v = V(W_NOISE, 0, 0, 0.22f, 0.1f); v->delay = 0.2f; v->hp = 1800; v->lp = 6000; v->wob = 0; v->curve = 3; v->atk = 0.01f;
      break; }
    case SFX_HURT: {
      v = V(W_SAW, 320, 80, 0.3f, 0.55f); v->lp = 2400; v->lp1 = 300; v->curve = 2;
      v = V(W_SQ, 233, 100, 0.2f, 0.3f); v->duty = 0.3f; v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.18f, 0.5f); v->lp = 3500; v->lp1 = 400; v->curve = 2;
      break; }
    case SFX_JUMP: {   /* a hop and a puff of the motors */
      v = V(W_TRI, 170, 520, 0.2f, 0.6f); v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.18f, 0.4f); v->lp = 700; v->lp1 = 3200; v->hp = 300; v->atk = 0.05f; v->curve = 2;
      break; }
    case SFX_LAND: {
      v = V(W_SINE, 110, 36, 0.28f, 1.0f); v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.14f, 0.5f); v->lp = 1200; v->lp1 = 200; v->curve = 3;
      v = V(W_TRI, 220, 75, 0.16f, 0.6f); v->curve = 2;
      break; }
    case SFX_DODGE: {
      v = V(W_NOISE, 0, 0, 0.32f, 0.9f); v->lp = 500; v->lp1 = 3000; v->hp = 200; v->atk = 0.08f; v->curve = 2;
      v = V(W_TRI, 160, 80, 0.2f, 0.5f); v->curve = 2;
      break; }
    case SFX_JET_EMPTY: {   /* the battery gives out: a sputter and a sagging whine */
      v = V(W_SAW, 520, 90, 0.5f, 0.4f); v->lp = 1800; v->lp1 = 300; v->wob = 34.f; v->curve = 1;
      v = V(W_SQ, 640, 220, 0.25f, 0.25f); v->duty = 0.3f; v->lp = 2500; v->wob = 50.f; v->curve = 2;
      for (int i = 0; i < 2; i++) { v = V(W_NOISE, 0, 0, 0.012f, 0.45f); v->lp = 3000; v->delay = 0.32f + (float)i * 0.09f; v->curve = 1; }
      break; }
    case SFX_PICKUP: {
      static const float n[3] = {880, 1108.7f, 1318.5f};
      for (int i = 0; i < 3; i++) { v = V(W_SINE, n[i], n[i], 0.32f - i * 0.04f, 0.4f); v->delay = i * 0.06f; v->fm_r = 2.f; v->fm_i = 1.2f; v->send = 0.35f; v->curve = 2; v->atk = 0.004f; }
      break; }
    case SFX_WEAPON: {   /* a new tool on the bench: four square notes up, then a click of the latch */
      static const float n[4] = {523.3f, 659.3f, 784.0f, 1046.5f};
      for (int i = 0; i < 4; i++) { v = V(W_SQ, n[i], n[i], 0.16f + (i == 3 ? 0.25f : 0.f), 0.4f); v->delay = i * 0.07f; v->duty = 0.35f; v->lp = 4500; v->curve = 2; v->send = 0.3f; }
      v = V(W_NOISE, 0, 0, 0.012f, 0.5f); v->delay = 0.32f; v->hp = 2000; v->curve = 2;
      break; }
    case SFX_PRINT_STOP: {   /* a print cancelled: the steppers wind down, two clunks and a long error tone */
      v = V(W_SAW, 640, 70, 0.8f, 0.35f); v->lp = 1800; v->lp1 = 250; v->wob = 80.f; v->curve = 1;
      v = V(W_SQ, 330, 60, 0.7f, 0.2f); v->duty = 0.25f; v->wob = 70.f; v->lp = 1500; v->curve = 1;
      for (int i = 0; i < 2; i++) { v = V(W_SINE, 120, 45, 0.14f, 0.7f); v->delay = 0.5f + i * 0.14f; v->curve = 2; }
      v = V(W_SQ, 440, 440, 0.5f, 0.3f); v->delay = 0.85f; v->duty = 0.5f; v->lp = 2800; v->curve = 1; v->atk = 0.01f; v->send = 0.3f;
      break; }
    case SFX_WHISTLE: {   /* a pea whistle: two blasts, a fast trill on the pea */
      for (int i = 0; i < 2; i++) {
        float d = i ? 0.5f : 0.34f, dl = i ? 0.42f : 0;
        v = V(W_SINE, 2650, 2850, d, 0.38f); v->wob = 62.f; v->curve = 1; v->atk = 0.015f; v->delay = dl; v->send = 0.3f; v->fm_r = 3.f; v->fm_i = 0.3f;
        v = V(W_NOISE, 0, 0, d, 0.12f); v->hp = 2400; v->lp = 7500; v->curve = 1; v->atk = 0.02f; v->delay = dl;
      }
      break; }
    case SFX_UI: {
      v = V(W_SQ, 1900, 1500, 0.03f, 0.4f); v->duty = 0.3f; v->curve = 1; v->lp = 5000;
      break; }
    case SFX_UI_OK: {
      v = V(W_TRI, 660, 660, 0.09f, 0.4f); v->curve = 1;
      v = V(W_TRI, 990, 990, 0.16f, 0.4f); v->delay = 0.07f; v->curve = 2; v->send = 0.25f;
      break; }
    case SFX_BOSS_HIT: {   /* a big hollow figure struck: a deep thunk, a plastic crack and a short ring */
      v = V(W_SINE, 240, 65, 0.3f, 0.9f); v->curve = 2; v->drive = 1.2f;
      v = V(W_SINE, 470, 470, 0.5f, 0.3f); v->fm_r = 1.41f; v->fm_i = 5.f; v->curve = 2; v->send = 0.3f;
      v = V(W_NOISE, 0, 0, 0.05f, 0.7f); v->hp = 1500; v->lp = 9000; v->curve = 2;
      v = V(W_SINE, 110, 45, 0.2f, 0.7f); v->curve = 2;
      break; }
    case SFX_ALARM: {
      for (int i = 0; i < 4; i++) { v = V(W_SQ, i & 1 ? 660 : 880, i & 1 ? 660 : 880, 0.17f, 0.6f); v->delay = i * 0.18f; v->duty = 0.4f; v->lp = 3200; v->curve = 1; v->atk = 0.006f; v->send = 0.2f; }
      break; }
    default: break;
  }
#undef V
}

void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double ms) {
  if (rate && (float)rate != SR) SR = (float)rate;
  static double carry;
  double exact = SR * ms / 1000.0 + carry;
  uint32_t n = (uint32_t)exact; carry = exact - n;
  if (n > cap) n = cap;
  float dt = 1.f / SR;
  /* the voice mixes into a scratch block; its ducking target pulls the music down ~60% while it speaks */
  static float vbuf[8192 * 2];
  static float duck;
  if (n > 8192) n = 8192;
  memset(vbuf, 0, n * 2 * sizeof(float));
  float duck_to = voice_mix(vbuf, (int)n);
  int e1 = (int)(0.19f * SR), e2 = (int)(0.253f * SR);
  if (e1 >= ECH) e1 = ECH - 1; if (e2 >= ECH) e2 = ECH - 1;
  for (uint32_t i = 0; i < n; i++) {
    /* mood changes fade the old tune down, then start the new one from its top */
    float mtgt = (want == cur && cur >= 0 && cur != MOOD_SILENT) ? mtarget : 0.f;
    if (want == MOOD_SILENT) mtgt = 0.f;
    mgain += (mtgt * music_on - mgain) * (mtgt > mgain ? 0.0004f : 0.0006f) * (48000.f / SR);
    if (want != cur && mgain < 0.01f) { cur = want; mtarget = 1; song_reset(); mgain = 0; }
    float mL = 0, mR = 0, msd = 0;
    if (cur >= 0 && cur < MOOD_SILENT && (mgain > 0.0005f || mtgt > 0)) {
      music_sample(&SONGS[cur], &mL, &mR, &msd);
      mL *= mgain * mus_vol; mR *= mgain * mus_vol; msd *= mgain * mus_vol;
    }
    duck += (duck_to - duck) * (duck_to > duck ? 0.0025f : 0.0004f) * (48000.f / SR);
    float dk = 1.f - 0.6f * duck;
    mL *= dk; mR *= dk; msd *= dk;
    float L = mL, R = mR, sd = msd;
    for (int k = 0; k < NV; k++) {
      voice *v = &vox[k];
      if (!v->on) continue;
      if (v->delay > 0) { v->delay -= dt; continue; }
      float u = 1.f - v->age / v->dur;
      if (u <= 0) { v->on = false; continue; }
      float env = u; for (int c = 1; c < v->curve; c++) env *= u;
      if (v->age < v->atk) env *= v->age / v->atk;
      v->f *= v->mult;
      float f = v->f;
      if (v->wob > 0) f *= 1.f + 0.12f * sinf(v->age * v->wob);
      float x;
      if (v->wave == W_NOISE) x = rnd();
      else {
        v->ph = sgn_frac(v->ph + f * dt);
        if (v->fm_i > 0) {
          v->mph = sgn_frac(v->mph + f * v->fm_r * dt);
          x = sinf(v->ph * TAU + v->fm_i * env * sinf(v->mph * TAU));
        } else x = osc(v->wave, v->ph, v->duty);
      }
      if (v->lp > 0) {
        float fc = v->lp1 > 0 ? v->lp + (v->lp1 - v->lp) * (v->age / v->dur) : v->lp;
        v->lz += (x - v->lz) * fminf(1.f, fc * TAU * dt);
        x = v->lz;
      }
      if (v->hp > 0) { v->hz += (x - v->hz) * fminf(1.f, v->hp * TAU * dt); x -= v->hz; }
      if (v->drive > 0) x = tanhf(x * (1.f + v->drive));
      x *= env * v->vol;
      L += x * v->gl; R += x * v->gr; sd += x * v->send;
      v->age += dt;
    }
    /* continuous voices: refreshed by calls, they fade in and out around them */
    {
      float sdv[1] = {0};
      for (int q = 0; q < 2; q++) {
        cont *c = &jets[q];
        c->ttl -= dt;
        float tg = c->ttl > 0 ? c->gain : 0.f;
        c->level += (tg - c->level) * (tg > c->level ? 0.0012f : 0.0007f) * (48000.f / SR);
        if (c->level < 0.0005f && tg <= 0) { c->level = 0; continue; }
        c->lpan += (c->pan - c->lpan) * 0.002f;
        float a = (pan_clamp(c->lpan) + 1.f) * 0.78539816f;
        float y = jet_sample(c, dt, q, sdv);
        L += y * cosf(a); R += y * sinf(a);
      }
      cont *cc = &coil_c;
      cc->ttl -= dt;
      float tg = cc->ttl > 0 ? 1.f : 0.f;
      cc->level += (tg - cc->level) * (tg > cc->level ? 0.002f : 0.004f) * (48000.f / SR);
      if (cc->level > 0.0005f) {
        float y = coil_sample(cc, dt, sdv);
        L += y; R += y;
      } else cc->level = 0;
      sd += sdv[0];
    }
    /* echo bus, a touch darker on each repeat */
    int r1 = (ew - e1 + ECH) % ECH, r2 = (ew - e2 + ECH) % ECH;
    float dl = eb[0][r1], dr = eb[1][r2];
    elp[0] += (dl - elp[0]) * 0.35f; elp[1] += (dr - elp[1]) * 0.35f;
    eb[0][ew] = sd * 0.9f + elp[1] * 0.38f;
    eb[1][ew] = sd * 0.9f + elp[0] * 0.38f;
    ew = (ew + 1) % ECH;
    L += elp[0] * 0.5f; R += elp[1] * 0.5f;
    L *= sfx_vol; R *= sfx_vol;
    /* a soft clip, so stacked effects and the music never overshoot */
    L = tanhf(L * 0.85f); R = tanhf(R * 0.85f);
    L = fmaxf(-1.f, fminf(1.f, L + vbuf[i * 2])); R = fmaxf(-1.f, fminf(1.f, R + vbuf[i * 2 + 1]));
    uint32_t w = (*cursor + i) % cap;
    ring[w * 2] = L; ring[w * 2 + 1] = R;
  }
  *cursor = (*cursor + n) % cap;
}

/* positional effects: pan and distance from the listener game.c places each frame */
static float lx, ly, lz, lyaw;
void audio_listener(float x, float y, float z, float yaw) { lx = x; ly = y; lz = z; lyaw = yaw; }
void sfx_at(sfx_t s, float x, float y, float z, float gain) {
  float dx = x - lx, dy = y - ly, dz = z - lz, d = sqrtf(dx * dx + dy * dy + dz * dz);
  float g = gain / (1.f + d * d / 900.f);
  if (g < 0.02f) return;
  float side = dx * cosf(lyaw) - dz * sinf(lyaw);
  sfx(s, d > 0.5f ? fmaxf(-0.8f, fminf(0.8f, side / d)) : 0.f, g);
}
