/* anim.c: see anim.h. The comments name the three.js r186 methods each part
 * follows (AnimationAction, AnimationMixer, PropertyMixer, Quaternion). */
#include "anim.h"
#include "ds.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- the rig file (tools/extract.mjs) ---- */
static uint32_t rd32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static float rdf(const uint8_t **p) { float v; memcpy(&v, *p, 4); *p += 4; return v; }
static void rdstr(const uint8_t **p, char *out, int cap) {
  int n = **p; (*p)++;
  int k = n < cap - 1 ? n : cap - 1;
  memcpy(out, *p, k); out[k] = 0; *p += n;
}

ck_rig *rig_load(const char *asset) {
  size_t len; uint8_t *b = ds_asset(asset, &len);
  if (!b || len < 12 || memcmp(b, "CKAN", 4)) { free(b); return NULL; }
  const uint8_t *p = b + 4;
  ck_rig *r = calloc(1, sizeof *r);
  rd32(&p);
  r->nb = (int)rd32(&p);
  for (int i = 0; i < r->nb && i < RIG_MAX_BONES; i++) rdstr(&p, r->bones[i], 24);
  r->nclips = (int)rd32(&p);
  for (int c = 0; c < r->nclips && c < RIG_MAX_CLIPS; c++) {
    ck_clip *k = &r->clips[c];
    rdstr(&p, k->name, sizeof k->name);
    k->dur = rdf(&p); k->nf = (int)rd32(&p);
    k->hand.peak = rdf(&p); k->hand.t = rdf(&p); k->hand.a0 = rdf(&p); k->hand.a1 = rdf(&p);
    k->foot.peak = rdf(&p); k->foot.t = rdf(&p); k->foot.a0 = rdf(&p); k->foot.a1 = rdf(&p);
    int nf = k->nf, nb = r->nb;
    k->times = malloc(sizeof(float) * nf);
    for (int o = 0; o < nf; o++) { float t = o / 30.f; k->times[o] = t < k->dur ? t : k->dur; }   /* Math.min(D.dur, o / 30) */
    k->quats = malloc(sizeof(float) * nf * nb * 4);
    /* lC(): the int16 frames are laid out frame-major; each is /32767 then normalised */
    for (int z = 0; z < nf; z++) for (int o = 0; o < nb; o++) {
      const uint8_t *q = p + ((size_t)(z * nb + o) * 4) * 2;
      int16_t v[4]; memcpy(v, q, 8);
      float c0 = v[0] / 32767.f, c1 = v[1] / 32767.f, c2 = v[2] / 32767.f, c3 = v[3] / 32767.f;
      float f = sqrtf(c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3); if (f == 0) f = 1;
      float *d = k->quats + ((size_t)z * nb + o) * 4;
      d[0] = c0 / f; d[1] = c1 / f; d[2] = c2 / f; d[3] = c3 / f;
    }
    p += (size_t)nf * nb * 8;
    k->hips = malloc(sizeof(float) * nf * 3);
    memcpy(k->hips, p, sizeof(float) * nf * 3);
    p += (size_t)nf * 12;
  }
  free(b);
  return r;
}

int rig_clip_index(const ck_rig *r, const char *name) {
  for (int i = 0; i < r->nclips; i++) if (!strcmp(r->clips[i].name, name)) return i;
  return -1;
}

/* ---- Quaternion.slerpFlat ---- */
static void slerp_flat(float *dst, const float *a, const float *b, float t) {
  float x0 = a[0], y0 = a[1], z0 = a[2], w0 = a[3];
  float x1 = b[0], y1 = b[1], z1 = b[2], w1 = b[3];
  if (t <= 0) { dst[0] = x0; dst[1] = y0; dst[2] = z0; dst[3] = w0; return; }
  if (t >= 1) { dst[0] = x1; dst[1] = y1; dst[2] = z1; dst[3] = w1; return; }
  if (w0 != w1 || x0 != x1 || y0 != y1 || z0 != z1) {
    float dot = x0 * x1 + y0 * y1 + z0 * z1 + w0 * w1;
    if (dot < 0) { x1 = -x1; y1 = -y1; z1 = -z1; w1 = -w1; dot = -dot; }
    float s = 1 - t;
    if (dot < 0.9995f) {
      float theta = acosf(dot), sn = sinf(theta);
      s = sinf(s * theta) / sn; t = sinf(t * theta) / sn;
      x0 = x0 * s + x1 * t; y0 = y0 * s + y1 * t; z0 = z0 * s + z1 * t; w0 = w0 * s + w1 * t;
    } else {
      x0 = x0 * s + x1 * t; y0 = y0 * s + y1 * t; z0 = z0 * s + z1 * t; w0 = w0 * s + w1 * t;
      float f = 1.f / sqrtf(x0 * x0 + y0 * y0 + z0 * z0 + w0 * w0);
      x0 *= f; y0 *= f; z0 *= f; w0 *= f;
    }
  }
  dst[0] = x0; dst[1] = y0; dst[2] = z0; dst[3] = w0;
}

/* Interpolant.evaluate over the clip's key times: the interval and alpha
 * (before the first key: the first value; after the last: the last) */
static void key_at(const ck_clip *k, float t, int *i0, int *i1, float *alpha) {
  int n = k->nf;
  if (n <= 1 || t <= k->times[0]) { *i0 = *i1 = 0; *alpha = 0; return; }
  if (t >= k->times[n - 1]) { *i0 = *i1 = n - 1; *alpha = 0; return; }
  int lo = (int)(t * 30.f); if (lo > n - 2) lo = n - 2; if (lo < 0) lo = 0;
  while (lo > 0 && k->times[lo] > t) lo--;
  while (lo < n - 2 && k->times[lo + 1] <= t) lo++;
  float t0 = k->times[lo], t1 = k->times[lo + 1];
  *i0 = lo; *i1 = lo + 1;
  *alpha = t1 > t0 ? (t - t0) / (t1 - t0) : 0;
}

void mixer_init(ck_mixer *m, const ck_rig *rig, t3_object *model) {
  memset(m, 0, sizeof *m);
  m->rig = rig; m->time_scale = 1;
  for (int i = 0; i < rig->nb; i++) {
    t3_object *o = t3_object_get_by_name(model, rig->bones[i]);
    m->bone[i] = o;
    if (o) { m->orig_q[i][0] = o->quaternion.x; m->orig_q[i][1] = o->quaternion.y; m->orig_q[i][2] = o->quaternion.z; m->orig_q[i][3] = o->quaternion.w; }
    else { m->orig_q[i][3] = 1; ds_log("anim: no bone %s", rig->bones[i]); }
  }
  m->hips = t3_object_get_by_name(model, "Hips");
  if (m->hips) { m->orig_p[0] = m->hips->position.x; m->orig_p[1] = m->hips->position.y; m->orig_p[2] = m->hips->position.z; }
  for (int i = 0; i < RIG_MAX_CLIPS; i++) m->actions[i].active_index = -1;
}

void action_reset(ck_action *a) {
  /* reset(): paused false, enabled true, time 0, _loopCount -1, stopFading, stopWarping */
  a->paused = false; a->enabled = true; a->time = 0; a->loop_count = -1; a->fading = false;
}

ck_action *mixer_action(ck_mixer *m, int clip) {
  if (clip < 0 || clip >= m->rig->nclips) return NULL;
  ck_action *a = &m->actions[clip];
  if (!m->made[clip]) {
    m->made[clip] = true;
    memset(a, 0, sizeof *a);
    a->clip = clip; a->time_scale = 1; a->weight = 1; a->enabled = true; a->loop = LOOP_REPEAT;
    a->loop_count = -1; a->active_index = -1; a->effective_weight = 1;
  }
  return a;
}

/* _activateAction / _deactivateAction: the active actions are the first
 * nActiveActions of _actions; activating swaps the action to the end of that
 * run, deactivating swaps it with the last active one */
void action_play(ck_mixer *m, ck_action *a) {
  if (a->active_index >= 0) return;
  a->active_index = m->nactive;
  m->active[m->nactive++] = a->clip;
}
void action_stop(ck_mixer *m, ck_action *a) {
  if (a->active_index >= 0) {
    int i = a->active_index, last = --m->nactive;
    int moved = m->active[last];
    m->active[i] = moved; m->actions[moved].active_index = i;
    a->active_index = -1;
  }
  action_reset(a);
}
void mixer_stop_all(ck_mixer *m) {
  for (int i = m->nactive - 1; i >= 0; i--) action_stop(m, &m->actions[m->active[i]]);
  for (int i = 0; i < RIG_MAX_CLIPS; i++) if (m->made[i]) action_reset(&m->actions[i]);
}

void action_set_effective_weight(ck_action *a, float w) {
  /* setEffectiveWeight: weight, _effectiveWeight (0 when disabled), stopFading */
  a->weight = w; a->effective_weight = a->enabled ? w : 0; a->fading = false;
}
static void schedule_fading(ck_mixer *m, ck_action *a, float d, float w0, float w1) {
  a->fading = true; a->f_t0 = m->time; a->f_t1 = m->time + d; a->f_v0 = w0; a->f_v1 = w1;
}
void action_fade_in(ck_mixer *m, ck_action *a, float d) { schedule_fading(m, a, d, 0, 1); }
void action_fade_out(ck_mixer *m, ck_action *a, float d) { schedule_fading(m, a, d, 1, 0); }
void action_cross_fade_from(ck_mixer *m, ck_action *a, ck_action *from, float d) {
  action_fade_out(m, from, d);
  action_fade_in(m, a, d);
}

/* _updateWeight */
static float update_weight(ck_action *a, double time) {
  float w = 0;
  if (a->enabled) {
    w = a->weight;
    if (a->fading) {
      float v;
      if (time <= a->f_t0) v = a->f_v0;
      else if (time >= a->f_t1) v = a->f_v1;
      else v = a->f_v0 + (a->f_v1 - a->f_v0) * (float)((time - a->f_t0) / (a->f_t1 - a->f_t0));
      w *= v;
      if (time > a->f_t1) { a->fading = false; if (v == 0) a->enabled = false; }
    }
  }
  a->effective_weight = w;
  return w;
}

/* _updateTime (no ping pong, repetitions Infinity for LoopRepeat) */
static float update_time(ck_mixer *m, ck_action *a, float dt) {
  float dur = m->rig->clips[a->clip].dur;
  float time = a->time + dt;
  if (dt == 0) return time;
  if (a->loop == LOOP_ONCE) {
    if (a->loop_count == -1) a->loop_count = 0;
    if (time >= dur) time = dur;
    else if (time < 0) time = 0;
    else { a->time = time; return time; }
    if (a->clamp_when_finished) a->paused = true; else a->enabled = false;
    a->time = time;
  } else {
    if (a->loop_count == -1) a->loop_count = 0;
    if (time >= dur || time < 0) {
      float ld = floorf(time / dur);
      time -= dur * ld;
      a->loop_count += (int)fabsf(ld);
      a->time = time;
    } else a->time = time;
  }
  return time;
}

static void accumulate(float *acc, float *cum, const float *v, float w, int n) {
  /* PropertyMixer.accumulate: the first contribution is copied, each next one
   * mixed in by w / cumulativeWeight (slerp for quaternions, lerp otherwise) */
  if (*cum == 0) { memcpy(acc, v, sizeof(float) * n); *cum = w; return; }
  *cum += w;
  float mix = w / *cum;
  if (n == 4) slerp_flat(acc, acc, v, mix);
  else for (int i = 0; i < n; i++) acc[i] += (v[i] - acc[i]) * mix;
}

void mixer_update(ck_mixer *m, float dt) {
  dt *= m->time_scale;
  m->time += dt;
  double time = m->time;
  const ck_rig *r = m->rig;
  for (int b = 0; b < r->nb; b++) m->cum_q[b] = 0;
  m->cum_p = 0;
  for (int i = 0; i < m->nactive; i++) {
    ck_action *a = &m->actions[m->active[i]];
    if (!a->enabled) { update_weight(a, time); continue; }
    float ts = a->paused ? 0 : a->time_scale;   /* _updateTimeScale (no warping) */
    float clip_t = update_time(m, a, dt * ts);
    float w = update_weight(a, time);
    if (w <= 0) continue;
    const ck_clip *k = &r->clips[a->clip];
    int i0, i1; float al;
    key_at(k, clip_t, &i0, &i1, &al);
    for (int b = 0; b < r->nb; b++) {
      float q[4];
      const float *q0 = k->quats + ((size_t)i0 * r->nb + b) * 4, *q1 = k->quats + ((size_t)i1 * r->nb + b) * 4;
      slerp_flat(q, q0, q1, al);   /* QuaternionLinearInterpolant */
      accumulate(m->acc_q[b], &m->cum_q[b], q, w, 4);
    }
    const float *p0 = k->hips + i0 * 3, *p1 = k->hips + i1 * 3;
    float p[3] = {p0[0] + (p1[0] - p0[0]) * al, p0[1] + (p1[1] - p0[1]) * al, p0[2] + (p1[2] - p0[2]) * al};
    accumulate(m->acc_p, &m->cum_p, p, w, 3);
  }
  /* PropertyMixer.apply: below a full weight the rest pose fills in */
  for (int b = 0; b < r->nb; b++) {
    t3_object *o = m->bone[b];
    if (!o) continue;
    float *q = m->acc_q[b];
    if (m->cum_q[b] == 0) memcpy(q, m->orig_q[b], sizeof(float) * 4);
    else if (m->cum_q[b] < 1) slerp_flat(q, q, m->orig_q[b], 1 - m->cum_q[b]);
    o->quaternion.x = q[0]; o->quaternion.y = q[1]; o->quaternion.z = q[2]; o->quaternion.w = q[3];
  }
  if (m->hips) {
    float *p = m->acc_p;
    if (m->cum_p == 0) memcpy(p, m->orig_p, sizeof(float) * 3);
    else if (m->cum_p < 1) for (int i = 0; i < 3; i++) p[i] += (m->orig_p[i] - p[i]) * (1 - m->cum_p);
    m->hips->position.x = p[0]; m->hips->position.y = p[1]; m->hips->position.z = p[2];
  }
}
