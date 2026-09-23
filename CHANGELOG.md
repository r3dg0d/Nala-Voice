# Changelog

This project follows [semantic versioning](https://semver.org/). The version
lives in one place, `project(VERSION)` in `CMakeLists.txt`, and is what
`nala --version` reports.

## 1.2.0 — 2026-09-23

### Added

- A local wake-word detector: openWakeWord's feature models (ONNX Runtime,
  one CPU thread, ≈1 % of a core) with a per-phrase detector trained on the
  user's own recordings -- a classifier and dynamic-time-warping templates
  that must both agree, calibrated against ~11 hours of real-world negative
  audio to a false-wake budget. Measured: 9/10 of the user's phrases, 0.83
  false wakes an hour on 3.6 h of real speech it never saw; see
  docs/WAKEWORD.md. Models are downloaded on request, checksummed, never
  shipped.
- Wake-word flow: she perks up straight from the detector, an optional
  chime, the request is transcribed only after a wake, follow-ups need no
  wake phrase for a while, and the detector is suspended while she speaks
  (with a grace period after) so she cannot wake herself; optional barge-in.
- A training wizard (six recordings, some ordinary talk, the room), per-phrase
  enable/retrain/delete, "delete all wake-word recordings", a live
  confidence meter, and `nala wakeword setup|train|test|eval|list|delete`.
- Assistant identity: a chosen name used in her system prompt, speech,
  whisper priming, commands ("open Nova's settings") and the tray; wake
  phrases independent of the name; personality, answer length and pet
  expressiveness; profile export/import (`nala profile …`) that never carries
  keys, memories or recordings.
- A first-run setup wizard (`nala setup`).
- Qwen3.8-Flash-Next as the recommended model, picked automatically when the
  server offers it; server detection (Ollama or other), thinking switched off
  in the form each server understands, model capability detection, idle
  unloading, readable out-of-GPU-memory errors, a `system_gpu` tool.
- A microphone indicator on her: off, listening for her name, taking a
  request, open, recording. Click-to-talk.
- `stt.gpu` to keep whisper-cli off the GPU.

### Fixed

- A misheard wake phrase after a real wake ("Hit Nala, resume screen
  recording") no longer falls through to the model.
- "Open Nala settings" was heard as "open all settings": her name is now
  primed inside commands too.
- After an upgrade, Qt's QML disk cache could keep serving the previous
  version's windows; Nala no longer uses it.
- The self-test wrote a QML cache into the real `~/.cache`; test runs now
  get a private cache directory, as they already did for configuration.
- A wake chime that could not play marked her voice as broken, silencing
  every later answer.
- The wake-word gate counted scores heard during a cooldown or the grace
  after she spoke towards the next detection.

### Changed

- `stt.wakePhrases` is replaced by `wake.phrases`; `stt.prompt` now defaults
  to one generated from her identity; `llm.vision` is `auto`/`on`/`off`.
- Depends on ONNX Runtime (optional at build time).

## 1.1.0 — 2026-09-22

### Added

- A local voice assistant, all of it optional:
  - speech recognition through whisper.cpp (`whisper-server` or
    `whisper-cli`), with push-to-talk, wake-word and always-listening modes,
    voice-activity detection, and a vocabulary prompt so her name and the
    privacy commands are heard correctly;
  - a fast command router for simple commands — stop, pause/resume screen
    memory, forget, pin, open settings, open/close/focus apps, close all
    windows, sleep, mute — that never waits on the model;
  - any OpenAI-compatible model (Ollama, llama.cpp, vLLM…) with tool calling
    and optional vision;
  - Fish Speech voice, streamed per sentence and interruptible, falling back
    to text when unavailable;
  - computer-use tools for windows, apps, files, browser, mouse and keyboard,
    and (off by default) the shell, with risk levels, confirmation, category
    switches and an audit log;
  - opt-in screen memory: privacy-gated capture, perceptual de-duplication,
    artifact extraction, retention by age and storage cap, pinning, full-text
    and time-phrase search, and a memory timeline window;
  - a privacy killswitch that works by voice without the model, persists
    immediately and discards frames already in flight.
- Her body shows what the assistant is doing: wider eyes while listening, the
  thinking tumble while thinking, working rings while acting, a voice-driven
  swell while speaking, covering her eyes when memory pauses, and a red dot or
  struck-through eye for screen memory.
- A speech bubble beside her with what she heard, what she says, a listening
  meter, and Yes/No for questions.
- Preferences tabs for the assistant, agent, memory, privacy and developer
  diagnostics.
- Control commands: `nala listen`, `ask`, `stop`, `doctor`, `timeline`,
  `memory pause [minutes] | resume | status`.
- `flake.nix` with a package (tests run in the build) and a dev shell.
- A unit-test suite for the assistant (76 tests) and opt-in tests against
  live whisper, model and Fish Speech servers.

### Fixed

- While something was fullscreen she stepped aside but her invisible window
  still took every click over its square: an empty input mask means "no
  mask" to Qt. It now takes none.
- The systemd user unit was installed under `lib64` on distributions and Nix
  where that is the library directory, where systemd never looks; and its
  `ExecStart` was hardcoded to `/usr/bin/nala`.
- An installed build could not find Qt libraries outside the system library
  path.
- A QML load failure now prints the reasons instead of only "could not build
  its window".
- The self-test no longer creates, removes or rewrites the real
  `~/.config/autostart/nala.desktop`.
- `nala --version` had its own hardcoded version.

### Changed

- The package depends on Qt Multimedia; the PKGBUILD builds this fork.

## 1.0.0

The companion as published by yappologistic.
