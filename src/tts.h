/* tts.h: text to speech on CMU Flite, compiled into the cart (third_party/flite).
 * Used by voice.c, which colours and caches the result. */
#ifndef DS_TTS_H
#define DS_TTS_H
#include <stdint.h>

/* Turn a script line into something Flite reads as words: lower-case the
 * shouting, drop quote marks, tidy punctuation. out is NUL-terminated. */
void tts_normalise(const char *in, char *out, int cap);

/* Synthesise text (already normalised) into mono 16-bit PCM at tts_rate().
 * pitch and pitch_range scale the voice's own mean f0 and its spread; stretch
 * scales durations (above 1 is slower). Returns a malloc'd buffer and its
 * sample count, or NULL. The first call loads the voices. */
typedef enum { TTS_SLT, TTS_RMS, TTS_VOICES } tts_voice;   /* CMU's US female and US male cluster voices */
int16_t *tts_synth(tts_voice v, const char *text, float pitch, float pitch_range, float stretch, int *n);
int tts_rate(void);   /* both voices run at the same rate */

#endif
