// Pregenerates Moheeb's, Luis's and the bosses' dialogue with Kyutai Pocket TTS
// (tools/voices_kyutai.py), cloning Moheeb and Luis from recordings of their
// own voices, so the cart plays real speech instead of synthesising it with
// Flite. Writes assets/voice/voices.bin:
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
//   PYTHON=<venv python with pocket-tts> HF_TOKEN=<token> MOHEEB_WAV=<wav> LUIS_WAV=<wav> node tools/voices.mjs
//
// The references are about 20 s of each person talking, mono 24 kHz. The
// Sergeant and the General use Pocket TTS's built-in voices, which voice.c
// then puts through a megaphone and a pull-string toy speaker.
import { readFileSync, writeFileSync, mkdirSync, unlinkSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

/* voice.h vo_speaker ids, and the reference voice for each */
const SPK = { MOHEEB: 0, LUIS: 1, 'THE SERGEANT': 2, 'THE GENERAL': 3 };
const LISTING = process.argv.includes('--list');
const PYTHON = process.env.PYTHON, REF = { 0: process.env.MOHEEB_WAV, 1: process.env.LUIS_WAV, 2: 'jean', 3: 'javert' };
if (!LISTING && (!PYTHON || !REF[0] || !REF[1])) { console.error('set PYTHON, MOHEEB_WAV and LUIS_WAV (see the top of this file)'); process.exit(1); }
const RATE = 24000;

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
/* any wav to mono 16 bit at RATE, through ffmpeg; trims leading and trailing silence */
function toRate(wav) {
  const raw = execFileSync('ffmpeg', ['-v', 'error', '-i', wav, '-af', 'silenceremove=start_periods=1:start_threshold=-50dB,areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse',
    '-ar', String(RATE), '-ac', '1', '-f', 's16le', '-'], { maxBuffer: 64 << 20 });
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

/* --list: the lines and their keys, nothing synthesised (to check a bank against the source) */
if (process.argv.includes('--list')) {
  for (const L of lines.values()) console.log(`${fnv(L.id, L.text).toString(16).padStart(8, '0')}\t${L.id}\t${L.say}`);
  process.exit(0);
}
/* every line in one run of the generator: the model loads once */
const work = join(tmpdir(), `sp-voices-${process.pid}`);
mkdirSync(work, { recursive: true });
const list = [...lines.values()];
const spec = { voices: { 0: REF[0], 1: REF[1], 2: REF[2], 3: REF[3] }, jobs: list.map((L, i) => ({ voice: String(L.id), text: speakable(L.say), out: join(work, `${i}.wav`) })) };
writeFileSync(join(work, 'jobs.json'), JSON.stringify(spec));
execFileSync(PYTHON, [join(ROOT, 'tools/voices_kyutai.py'), join(work, 'jobs.json')], { stdio: 'inherit' });
const ents = [], blobs = [];
let off = 0, secs = 0;
list.forEach((L, i) => {
  const pcm = toRate(join(work, `${i}.wav`));
  const data = adpcm(pcm);
  ents.push({ key: fnv(L.id, L.text), rate: RATE, n: pcm.length, off });
  blobs.push(data); off += data.length; secs += pcm.length / RATE;
  unlinkSync(join(work, `${i}.wav`));
});
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
