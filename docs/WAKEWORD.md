# The wake word

Say "Hey Nala" (or whatever you have taught her) and she looks up within a
fraction of a second, before anything else has run. Nothing you say is
transcribed, sent anywhere or kept until then.

## How it works

```
microphone ─► 80 ms blocks ─► mel spectrogram ─► speech embedding (96-d)
                                   (ONNX)              (ONNX)
                                                          │
                     ┌────────────────────────────────────┤
                     ▼                                    ▼
          classifier over the last        template matching (DTW) against
          ~1.3 s of embeddings            the user's own recordings
                     └──────────────► both must pass ◄────┘
                                          │
                               gate: threshold, cooldown,
                               suspended while she speaks
                                          │
                                   wake detected
                     ┌────────────────────┼──────────────────────┐
                     ▼                    ▼                      ▼
             she perks up at once   soft chime (optional)   the next utterance
                                                            goes to whisper.cpp
```

- **Feature models.** The mel spectrogram and speech-embedding models are
  [openWakeWord](https://github.com/dscripka/openWakeWord)'s, run with ONNX
  Runtime on one CPU thread. The embedding model derives from Google's
  `speech_embedding` (Apache-2.0); openWakeWord distributes its models under
  **CC BY-NC-SA 4.0**, fine for personal use. They are **not shipped** with
  Nala: `nala wakeword setup` (or Preferences → Wake word → Download)
  fetches them, pinned by SHA-256, into `~/.local/share/nala/wakeword/features`.
- **Negative data.** The same download includes openWakeWord's published
  "false-positive validation set": about 11 hours of real audio (dinner-party
  conversation, conversational English, reverberated music), already
  turned into embeddings (185 MB, same licence). Training uses 85% of it as
  examples of what is *not* the phrase and holds the last 15% (1.6 h) back
  to measure false wakes per hour.
- **The per-phrase detector** is Nala's own, trained on this computer from
  your recordings, so any phrase in any language works. Two independent
  tests have to agree:
  1. a regularised logistic-regression classifier over the embeddings
     (openWakeWord's own "custom verifier" idea), which knows what the phrase
     sounds like against thousands of other sounds;
  2. subsequence dynamic time warping against each of your recordings,
     which knows what *your* phrase sounds like, at whatever speed.
  Either alone was measured to be far worse (below).
- **The gate** turns scores into a wake: over the threshold, then a cooldown
  (`wake.cooldownMs`, 2 s) during which nothing can wake her again. While she
  is speaking the detector is suspended, and after she stops it waits
  `wake.postSpeechMs` (800 ms) before listening again, so her own voice
  cannot wake her. With **barge-in** on, it keeps listening while she speaks,
  and hearing her name stops her; without echo cancellation that risks
  self-triggering, so it is off by default.

Everything lives in `src/assistant/wakeword.*` behind a `WakeWordBackend`
interface (`initialize`, `start`, `stop`, `pause`, `resume`, `processAudio`,
`registerWakeword`, `removeWakeword`, `setSensitivity`, `setConfirmation`,
signals `detected` / `scored`). The assistant only sees detections.

## Setting it up

Preferences → **Wake word**, or the first-run wizard:

1. **Download the detector** (≈190 MB, once).
2. **Train**: say the phrase six times, then optionally read a short
   paragraph (≈15 s) and record four seconds of your room. The paragraph and
   the room matter: they teach it what your voice sounds like when it is *not*
   saying the phrase. Training takes about 10–20 seconds and reports how often
   the result would have false-woken on the held-out audio.
3. Set **Listening** to *Wake word*.

From the command line:

```bash
nala wakeword setup
nala wakeword train "hey nala" me-1.wav … me-6.wav --negative talking.wav room.wav
nala wakeword test          # live confidence meter from the microphone
nala wakeword eval "hey nala" clip.wav …   # scores and detections for recordings
nala wakeword list
nala wakeword delete "hey nala"
```

**Tuning.** *Sensitivity* (0–1, 0.5 = as trained) moves the threshold. Turn on
Developer → debug logging for a rolling confidence meter against the
threshold, and the score of every detection in the log.

## Names and phrases

The assistant's name (`identity.name`) and her wake phrases
(`wake.phrases`) are separate: she can be called Nova and wake on
"computer". Each phrase has its own model, recordings and threshold, and can
be switched on and off. *Also wake on the name alone* adds the bare name as a
phrase (it needs training like any other). Renaming her retires the old
name: "Hey Nala" stops working unless it stays enabled.

## Privacy

- Wake-word audio is processed in 80 ms blocks in memory and discarded; it is
  never written to disk. While only the detector is listening, the indicator
  on her is a thin ring and the tray says "listening for her name".
- In wake-word mode, speech that did not follow a wake is dropped **before**
  transcription.
- Training recordings are made only when you press Record, are stored under
  `~/.local/share/nala/wakeword/phrases/<phrase>/samples/` (owner-only), and
  can be deleted per phrase or all at once in Preferences, or with
  `nala wakeword delete`.

## Measured results

Evaluated on this project's development machine with **synthetic speech**
(piper, `en_US-libritts_r-medium`) standing in for a user: one voice
recorded "Hey Nala" six times plus about 30 seconds of ordinary talk; the
tests below never saw any of the training audio. Real human voices will
differ; treat these as indicative, not guarantees.

| | result |
| --- | --- |
| the user's own held-out "Hey Nala" (10 clips, some followed by a command) | 9 / 10 detected |
| other speakers saying "Hey Nala" (8 voices) | 5 / 8 (it is personalised) |
| other speakers' ordinary sentences (200, 40 voices) | 0 / 200 false wakes |
| close confusables ("Hey now", "Hey Nolan", "Nala." alone, "Hey Nina"… ×4 voices) | 11 / 68 false wakes |
| **real human speech** (LibriSpeech test-clean, 40 speakers, 3.6 h, never trained on) | **0.83 false wakes per hour** |
| held-out real-world benchmark used for calibration (1.6 h) | 0 false wakes |
| cost | 103× faster than real time on one core (i9-14900K) ≈ **1 % of a core** |
| decision time after the last audio block | 1–9 ms |

What was tried and measured before settling on this design:

| variant | own voice | others' sentences | real speech |
| --- | --- | --- | --- |
| classifier only, user's recordings as the only negatives | 4/10 | 137/200 | — |
| + the real-world negative bank | 4/10 | 5/200 | 2.2 /h |
| classifier + templates, strictest thresholds | 7/10 | 0/200 | 0.28 /h |
| **classifier + templates, centre of the feasible region (shipped)** | **9/10** | **0/200** | **0.83 /h** |
| + synthetic multi-speaker positives | 6/10 | 0/200 | 0.83 /h |
| + synthetic confusable negatives | 7/10 | 1/200 | 4.1 /h |
| + another phrase's recordings as negatives | 7/10 | — | 1.9 /h |

## Limitations

- **Similar phrases in the same voice cross-trigger.** "Hey Nova" woke a
  "Hey Nala" model trained on the same speaker. Choose phrases that differ by
  more than one sound, or lower the sensitivity.
- It is **personalised**: other people saying the phrase are caught about
  half the time. Anyone who should wake her should record samples too
  (recordings from several people can be trained together).
- **No echo cancellation**: hence the suspension while she speaks.
- openWakeWord's target is under 0.5 false wakes an hour; on real speech this
  measured 0.83. The sensitivity slider trades that against recall.
- Without the downloaded models, or before a phrase is trained, wake-word mode
  falls back to transcribing everything and listening for her name in the
  text, which costs more and is less private. The status line says so.
