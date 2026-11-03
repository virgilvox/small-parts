"""Kyutai Pocket TTS, batch: the generator behind tools/voices.mjs.

Loads the model once, makes a voice state for each reference (a recording to
clone, or a built-in voice name), then writes one wav per job.

    python tools/voices_kyutai.py jobs.json

jobs.json: {"voices": {"moheeb": "<wav or name>", ...},
            "jobs": [{"voice": "moheeb", "text": "...", "out": "<wav path>"}, ...]}

The voice cloning weights are gated on Hugging Face (kyutai/pocket-tts):
accept the terms and pass a token in HF_TOKEN.
"""
import json
import sys
import wave

import numpy as np
from pocket_tts import TTSModel


# The model is stochastic and now and then stops a line halfway or runs on
# past it. Each take's speech is timed (ignoring silence) and checked against a
# plausible pace for its length; a few tries, then the take nearest the middle.
MS_PER_CHAR = (35, 110, 62)   # low, high, aim (Luis talks quickly)
TRIES = 6


def speech_span(pcm, rate):
    """seconds between the first and last 20 ms frame above a speech threshold"""
    frame = int(rate * 0.02)
    n = len(pcm) // frame
    if n == 0:
        return 0.0, 0, len(pcm)
    e = np.sqrt((pcm[: n * frame].reshape(n, frame) ** 2).mean(axis=1))
    on = np.nonzero(e > max(0.003, e.max() * 0.02))[0]   # low: quiet word endings count
    if len(on) == 0:
        return 0.0, 0, len(pcm)
    a, b = on[0] * frame, (on[-1] + 1) * frame
    return (b - a) / rate, a, b


def take(model, state, text):
    best, best_err = None, 1e9
    chars = max(len(text), 8)
    lo, hi, aim = MS_PER_CHAR
    hi = hi + (40 if chars < 24 else 0)   # short lines run long on their pauses
    for _ in range(TRIES):
        audio = model.generate_audio(state, text, max_tokens=200)
        pcm = np.clip(audio.detach().cpu().numpy().reshape(-1), -1, 1)
        secs, a, b = speech_span(pcm, model.sample_rate)
        ms = secs * 1000 / chars
        clip = pcm   # the whole take; voices.mjs trims only the silence
        if lo <= ms <= hi:
            return clip, ms, True
        err = abs(ms - aim)
        if err < best_err:
            best, best_err = (clip, ms), err
    return best[0], best[1], False


def main(path):
    spec = json.load(open(path))
    model = TTSModel.load_model()
    states = {k: model.get_state_for_audio_prompt(v) for k, v in spec["voices"].items()}
    for i, job in enumerate(spec["jobs"]):
        pcm, ms, ok = take(model, states[job["voice"]], job["text"])
        if not ok:
            print(f"best take off pace ({ms:.0f} ms a character): {job['text']}", flush=True)
        with wave.open(job["out"], "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(model.sample_rate)
            w.writeframes((pcm * 32767).astype("<i2").tobytes())
        print(f"{i + 1}/{len(spec['jobs'])}", flush=True)


if __name__ == "__main__":
    main(sys.argv[1])
