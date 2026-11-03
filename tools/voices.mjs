// Pregenerates Moheeb's, Luis's and the bosses' dialogue with a neural voice
// (Piper VITS models run by sherpa-onnx), so the cart plays real recordings
// instead of synthesising them with Flite. Writes assets/voice/voices.bin:
//
//   "SPVB" u32 version, u32 count, count x { u32 key, u32 rate, u32 samples, u32 offset }, then
//   IMA ADPCM data (4 bits a sample, low nibble first, the predictor and step index
//   restart at 0 for each line)
//
// The key is FNV-1a over the voice.h speaker id byte and the exact text the game
// passes to voice_say (voice.c computes the same). Lines come from the source:
// cutscene.c's say() and say_as() (the spoken form), mission.c's walkie table,
// enemy.c's boss shouts and game.c's buddy line. Army men and alerts stay on
// Flite in the cart.
//
//   SHERPA=<sherpa-onnx dir> MODELS=<dir of vits-piper-* folders> node tools/voices.mjs
//
// Voices (all public domain or CC0 datasets, see docs/DESIGN.md): Moheeb is
// en_US-bryce (soft, younger), Luis is en_US-norman (older, gruffer), the
// Sergeant and the General are en_US-john, which voice.c then puts through a
// megaphone and a pull-string toy speaker.
import { readFileSync, writeFileSync, mkdirSync, existsSync, unlinkSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SHERPA = process.env.SHERPA, MODELS = process.env.MODELS;
if (!SHERPA || !MODELS) { console.error('set SHERPA and MODELS (see the top of this file)'); process.exit(1); }

/* voice.h vo_speaker ids, and the voice and pace for each */
const SPK = { MOHEEB: 0, LUIS: 1, 'THE SERGEANT': 2, 'THE GENERAL': 3 };
const VOICE = {
  0: { model: 'bryce', length: 1.0, noise: 0.6 },
  1: { model: 'norman', length: 1.1, noise: 0.75 },
  2: { model: 'john', length: 0.9, noise: 0.667 },
  3: { model: 'john', length: 1.2, noise: 0.5 },
};

/* ── the lines ── */
const src = (f) => readFileSync(join(ROOT, 'src', f), 'utf8');
const unq = (s) => s.replace(/\\"/g, '"').replace(/\\\\/g, '\\');
const lines = new Map();   // key -> { spk, text, say }
function add(who, text, said) {
  const id = SPK[who];
  if (id === undefined) return;
  const t = unq(text);
  lines.set(`${id}|${t}`, { id, text: t, say: said ? unq(said) : t });
}
const STR = '"((?:[^"\\\\]|\\\\.)*)"';
for (const m of src('cutscene.c').matchAll(new RegExp(`\\bsay\\(\\s*${STR}\\s*,\\s*${STR}`, 'g'))) add(m[1], m[2]);
for (const m of src('cutscene.c').matchAll(new RegExp(`\\bsay_as\\(\\s*${STR}\\s*,\\s*${STR}\\s*,\\s*${STR}`, 'g'))) add(m[1], m[3]);
/* the walkie table: {MO, "line"} and {LU, "line"} (macros for the two names) */
const WHO = { MO: 'MOHEEB', LU: 'LUIS' };
for (const m of src('mission.c').matchAll(new RegExp(`\\{\\s*(MO|LU)\\s*,\\s*${STR}\\s*\\}`, 'g'))) add(WHO[m[1]], m[2]);
/* boss shouts: shout("WHO", "line"), and the line tables read by shout("WHO", L[...]) */
const en = src('enemy.c');
for (const m of en.matchAll(new RegExp(`\\bshout\\(\\s*${STR}\\s*,\\s*${STR}`, 'g'))) add(m[1], m[2]);
for (const m of en.matchAll(new RegExp(`\\bshout\\(\\s*${STR}\\s*,\\s*[a-z_]+\\s*\\?\\s*${STR}\\s*:\\s*${STR}`, 'g'))) { add(m[1], m[2]); add(m[1], m[3]); }
for (const m of en.matchAll(/static const char \*const L\[\] = \{([^}]*)\};[^]*?shout\(\s*"([^"]+)"\s*,\s*L\[/g))
  for (const s of m[1].matchAll(new RegExp(STR, 'g'))) add(m[2], s[1]);
/* the buddy getting back up */
for (const who of ['MOHEEB', 'LUIS']) add(who, "I'm fine. Give me a second.");

/* ── synthesis ── */
const fnv = (id, text) => {
  let h = 0x811c9dc5;
  const bytes = [id & 255, ...Buffer.from(text, 'utf8')];
  for (const b of bytes) { h ^= b; h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
};
/* what the voice reads: shouting in capitals reads better in sentence case, and a few
 * hackerspace words need spelling out */
function speakable(t) {
  let s = t;
  if (s === s.toUpperCase()) s = s.toLowerCase().replace(/(^|[.!?]\s+)([a-z])/g, (a, p, c) => p + c.toUpperCase());
  s = s.replace(/\bLiPo\b/gi, 'lie-po').replace(/\bLEGO\b/g, 'Lego').replace(/\bLED\b/g, 'L E D').replace(/\bTV\b/g, 'T V');
  s = s.replace(/\bESD\b/g, 'E S D').replace(/\bCNC\b/g, 'C N C').replace(/_/g, ' ');
  return s;
}
function synth(v, text, out) {
  const V = VOICE[v], M = join(MODELS, `vits-piper-en_US-${V.model}-medium`);
  execFileSync(join(SHERPA, 'bin', 'sherpa-onnx-offline-tts'), [
    `--vits-model=${join(M, `en_US-${V.model}-medium.onnx`)}`, `--vits-tokens=${join(M, 'tokens.txt')}`,
    `--vits-data-dir=${join(M, 'espeak-ng-data')}`, `--vits-length-scale=${V.length}`, `--vits-noise-scale=${V.noise}`,
    `--output-filename=${out}`, speakable(text)], { stdio: 'pipe' });
}
/* any wav to mono 16 bit at 16 kHz, through ffmpeg; trims leading and trailing silence */
function to16k(wav) {
  const raw = execFileSync('ffmpeg', ['-v', 'error', '-i', wav, '-af', 'silenceremove=start_periods=1:start_threshold=-50dB,areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse',
    '-ar', '16000', '-ac', '1', '-f', 's16le', '-'], { maxBuffer: 64 << 20 });
  return new Int16Array(raw.buffer, raw.byteOffset, raw.length >> 1);
}

/* ── IMA ADPCM ── */
const STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
const IDX = [-1, -1, -1, -1, 2, 4, 6, 8];
function adpcm(pcm) {
  const out = Buffer.alloc((pcm.length + 1) >> 1);
  let pred = 0, idx = 0;
  for (let i = 0; i < pcm.length; i++) {
    const step = STEP[idx];
    let diff = pcm[i] - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let d = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; d += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; d += step >> 1; }
    if (diff >= step >> 2) { code |= 1; d += step >> 2; }
    pred = Math.max(-32768, Math.min(32767, pred + (code & 8 ? -d : d)));
    idx = Math.max(0, Math.min(88, idx + IDX[code & 7]));
    if (i & 1) out[i >> 1] |= code << 4; else out[i >> 1] = code;
  }
  return out;
}

const tmp = join(tmpdir(), `sp-voice-${process.pid}.wav`);
const ents = [], blobs = [];
let off = 0, secs = 0;
for (const L of lines.values()) {
  synth(L.id, L.say, tmp);
  const pcm = to16k(tmp);
  const data = adpcm(pcm);
  ents.push({ key: fnv(L.id, L.text), rate: 16000, n: pcm.length, off });
  blobs.push(data); off += data.length; secs += pcm.length / 16000;
  process.stdout.write('.');
}
if (existsSync(tmp)) unlinkSync(tmp);
const keys = new Set(ents.map((e) => e.key));
if (keys.size !== ents.length) { console.error('\nkey collision'); process.exit(1); }
ents.sort((a, b) => a.key - b.key);   /* voice.c binary searches the table */
const head = Buffer.alloc(12 + ents.length * 16);
head.write('SPVB', 0); head.writeUInt32LE(1, 4); head.writeUInt32LE(ents.length, 8);
ents.forEach((e, i) => { const o = 12 + i * 16; head.writeUInt32LE(e.key, o); head.writeUInt32LE(e.rate, o + 4); head.writeUInt32LE(e.n, o + 8); head.writeUInt32LE(e.off, o + 12); });
mkdirSync(join(ROOT, 'assets/voice'), { recursive: true });
writeFileSync(join(ROOT, 'assets/voice/voices.bin'), Buffer.concat([head, ...blobs]));
const by = {}; for (const L of lines.values()) by[L.id] = (by[L.id] || 0) + 1;
console.log(`\n${ents.length} lines (${Object.entries(by).map(([k, n]) => `${Object.keys(SPK).find((s) => SPK[s] == k)} ${n}`).join(', ')}), ${secs.toFixed(0)} s, voices.bin ${((head.length + off) / 1e6).toFixed(2)} MB`);
