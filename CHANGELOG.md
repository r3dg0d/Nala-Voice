# Changelog

## Unreleased
- Report truncated SSE/Ollama streams as failures instead of accepting partial answers; preserve completion-marker compatibility and verify next-request recovery with fake-server tests.


This project follows [semantic versioning](https://semver.org/). The version
lives in one place, `project(VERSION)` in `CMakeLists.txt`, and is what
`nala --version` reports.

## Unreleased

## 1.4.2 — 2026-09-30

- Add local Qwen3-TTS 0.6B and Fish Speech 1.5 API adapters with persistent-service setup instructions and live chain verification.
- Prevent HTTP error bodies from starting PCM playback and blocking the fallback; require successful voice health responses.
- Ground actions in the focused window, independently validate control bounds on a clean crop, verify actual cursor position, and reject local layout changes before clicking.
- Require semantic post-action verification even without an explicit expected state. Keep landmark centers clear and recenter crops near their edges.
- Add regression coverage for neighboring controls, target movement, unrelated animation, cursor metadata and real primary/fallback synthesis, plus smaller-control vision fixtures.

## 1.4.1 — 2026-09-30

- Match window focus by app/title/workspace, refuse ambiguous selections, and verify the compositor's active window. Named focus commands stay on the fast path.
- Add opt-in, key-free DuckDuckGo Lite search and configurable SearXNG, current-question retrieval, bounded public-source fetching, evidence-ID checks and clickable source buttons.
- Keep private memories out of automatic web queries; further searches after untrusted tool reads require confirmation. Add offline backend, privacy, parsing, routing and provenance tests.

## 1.4.0 — 2026-09-30

- Add named-target, closed-loop GUI grounding with cropped observations, cursor landmarks, normalized coordinates, bounded refinement, fresh retry confirmations and post-action verification.
- Add asynchronous local embeddings, hybrid FTS/dense/artifact/entity/fact retrieval, optional contextual rewriting and reranking, and `memory search --debug`.
- Add transactional provenance-aware durable facts, entity associations, history/conflicts, dependency invalidation and complete forgetting cascades.
- Keep deterministic commands fast and memory answers buffered for evidence-ID checks. Embeddings remain opt-in; screen recording and automatic durable extraction remain off by default.
- Add offline fake-backend tests and project-local retrieval/vision benchmarks. Detect NixOS's ydotool daemon socket for input helpers.

## 1.3.10 — 2026-09-30

Microphone recovery that actually works on PipeWire, found by testing with a real
device instead of a simulated failure.

### Fixed

- **Selected microphone disappearing on PipeWire.** 1.3.9's recovery listened for Qt's
  device-change signals and stream errors. Measured against a live PipeWire server, Qt
  reports neither when a source is removed: no signal, no state change, a stale device
  list, and the server silently re-links the capture stream to the *default* microphone.
  So Nala kept listening on a different device without saying so, and the recovery never
  ran. When a specific microphone is selected she now asks PipeWire (`pw-dump`) every
  3 s; if it is gone she stops listening, logs `microphone-lost`, retries with backoff,
  and only reopens once PipeWire lists the device again (`microphone-recovered`).
  Verified live: lost ~2 s after removal, recovered <1 s after it returned.
  If `pw-dump` is missing or unreadable she assumes the device is present, so nothing
  changes without PipeWire's tools. With "system default" selected, PipeWire's own
  routing applies as before.

### Tests

- Parsing of `pw-dump` output (source by description/name/nick, sinks and
  garbage not mistaken for a microphone) and the wait-until-it-returns retry loop.

## 1.3.9 — 2026-09-29

Audio device recovery: the microphone and the speaker survive a device
disappearing mid-session, and `nala tts status` agrees with doctor.

### Fixed

- **Microphone recovery**: if the input device is unplugged, errors, or PipeWire
  restarts mid-session, the mic used to stay "open" but deaf until a settings
  toggle. It now notices (audio-source error state and device-list changes),
  logs `microphone-lost` once, retries quietly with 1 s → 15 s backoff, and
  logs `microphone-recovered` when it is back. A mic that follows the system
  default moves to a new default device when it changes. Turning the mic off
  cancels the retry.
- **Speaker device loss**: an output device that errors mid-reply used to leave
  playback waiting on a sink that never drained, so she stayed "speaking". The
  speaker now reports the failure, and she drops the rest of the reply and
  settles (voice marked broken until the TTS cooldown ends).
- **TTS status sticky sync**: `nala tts status` aligns the sticky voice-broken
  flag with live Qwen3-TTS / Fish Speech probes (same paths and recovery
  contract as doctor) — clears the failure and reports `Voice server is back`
  when an applicable engine answers, and marks voice broken with the cooldown
  note when none do. Engine `none` stays intentional silence. Status probes
  `/v1/models` and `/v1/health` like doctor so both agree on "up".

### Tests

- **Audio recovery**: retry backoff and one-shot loss reporting for the
  microphone; a speaker sink error reports failure and stops playback.
- **TTS status sticky**: offline coverage for clearing voice-broken when a
  server responds, keeping it when both stay down, and marking it when status
  finds no voice server.

## 1.3.8 — 2026-09-29

STT status syncs the auto→cli sticky flag with a live whisper-server probe.

### Fixed

- **STT status sticky sync**: `nala stt status` (auto mode) aligns the
  auto→cli sticky flag with a live whisper-server probe — marks dead and
  reports `Using whisper-cli for now` when the server is down, and clears the
  sticky failure with `whisper-server is back` when it responds again (same
  recovery doctor already performed). Cli/server modes no longer claim a
  fallback.

### Tests

- **STT status sticky**: offline coverage for auto-only fallback wording,
  sticky kept while the server stays down, and sticky cleared when status
  finds the server up again so the next utterance retries whisper-server.

## 1.3.7 — 2026-09-29

Router month units on pause/forget, and STT auto→cli fallback tests.

### Fixed

- **Router month durations**: `pause` / `forget the last …` fast-path commands
  accept `month` / `months` (same unit list as `delete … older than`). Spoken
  "pause memory for a month" and "forget the last month" no longer fall through
  to the model.

### Tests

- **STT auto→cli fallback**: offline coverage for `stt.mode=auto` falling back
  to whisper-cli when whisper-server fails (`m_serverDead` sticky until an
  `stt.*` setting changes), plus `server` mode (no fallback) and `cli` mode
  (skips server).

## 1.3.6 — 2026-09-29

Memory status no longer hides an armed privacy pause behind "off".

### Fixed

- **Memory status vs pause**: `ScreenMemory::status()` (and therefore
  `nala memory status`, doctor, settings copy, and tool replies) now reports
  `paused` / `paused until …` when the privacy killswitch is armed, even if
  `memory.enabled` is false. Previously disabled+paused looked like plain
  `off`, hiding a pause that still survives restart. Settings also keeps the
  Resume button visible while a pause is armed with the toggle off.

## 1.3.5 — 2026-09-29

Oneshot privacy for screen memory, and `nala memory` when she is not running.

### Fixed

- **Oneshot privacy**: `nala doctor` / `model` / `stt|tts status` (and the new
  headless memory CLI) no longer arm screen capture or retention sweeps when
  `memory.enabled` is on. Previously oneshot left the capture timer running.

### Added

- **`nala memory` without a companion**: `status`, `pause [minutes]`, `resume`
  and `clear screen [all]` work headless (same oneshot path as doctor), so you
  can pause or wipe screen history before she starts. Pause still persists in
  settings across restarts.

## 1.3.4 — 2026-09-29

TTS recovery after chain cooldown, and safer Fish/Qwen endpoint path joins.

### Fixed

- **TTS voice recovery**: after Qwen3-TTS / Fish Speech (or the speaker) fail,
  `m_voiceBroken` clears once the TtsChain cooldown ends, so a voice server that
  comes back is found without changing a settings key. `nala tts status` notes
  the cooldown retry.
- **Fish / Qwen endpoint paths**: trailing slashes on `tts.endpoint` /
  `tts.qwen.endpoint` no longer produce `//v1/...` routes (doctor probes and
  synthesise share the same join helper).

## 1.3.3 — 2026-09-29

Scripting-friendly doctor exits, and safer barge-in without echo cancellation.

### Fixed

- **Barge-in echo guard**: with `wake.bargeIn` on, the wake gate temporarily
  demands three consecutive windows and a stricter sensitivity while she is
  speaking, so brief echoes of her own TTS are less likely to interrupt her.
  A clear user wake still stops her. Default half-duplex (detector paused
  while she talks) is unchanged.

### Changed

- `nala doctor` exits `1` when any required (non-optional) line is `FAIL`, so
  scripts can trust the exit status. Optional misses still print as `--` and
  do not fail the exit. Docs and CI assert the contract.

## 1.3.2 — 2026-09-29

Doctor and status work when she is not running. Troubleshooting no longer
requires starting the companion first.

### Fixed

- `nala doctor`, `nala model status|list|…`, `nala stt status` and
  `nala tts status` run headless when no companion is listening on the control
  socket. Previously they printed `Nala is not running.` and exited 1, which
  contradicted the docs (start troubleshooting with `nala doctor`). Oneshot
  mode loads settings and wake models, answers, and exits — no window, no
  microphone, no setup wizard.

### Changed

- CI smokes `nala doctor` under offscreen QPA and asserts the report starts
  with `nala <version>`.

## 1.3.1 — 2026-09-29

Headless and packaging polish on top of 1.3.0. Ships the CLI fix that stopped
GitHub Actions `nala --version` from aborting (exit 134) on runners with no
display.

### Fixed

- `nala --version` (and other CLI entry points) no longer abort with exit 134
  on headless hosts: the version flag prints before Qt starts, and when there
  is neither `DISPLAY` nor `WAYLAND_DISPLAY` the QPA platform defaults to
  `offscreen`. CI smoke checks both an explicit `offscreen` QPA and the
  auto-selected path, and asserts the printed line matches CMake.

### Changed

- `nala doctor` leads with the binary version (`nala 1.3.1`).
- `scripts/check-install.sh` asserts `--version` matches `CMakeLists.txt`.
- NixOS docs show how to pin the flake input to a release tag.

## 1.3.0 — 2026-09-29

A local multi-model brain, and a faster, more capable voice path. Details in
[docs/models.md](docs/models.md), [docs/voice-pipeline.md](docs/voice-pipeline.md)
and [docs/tools.md](docs/tools.md).

### Added

- **Three local models** -- main (Qwen3.8-27B), fast (`gpt-oss:20b`) and speed
  (`qwen3:30b-a3b`), names configurable and matched loosely. A cheap word-based
  router sends reasoning, code and long requests to main and short chat to
  fast; `llm.mode`, `llm.autoRouting` and `llm.route.*` override it. Missing
  models are replaced by the next role, a model that fails at run time is retried
  once on another local model, and nothing ever falls back to a non-local one.
  "Use the fast model", "use the smart model for this", "switch back to
  automatic model selection" by voice, without rewriting defaults unless asked
  ("from now on").
- **Native Ollama API** (`llm.ollamaNative`): the context window
  (`llm.contextTokens`, 16384), keep-alive, exact thinking control and Ollama's
  own token timings. Measured on an RTX 4090: a 27B whose Modelfile says
  131072 went from 27 GB / 22% on CPU / 52 s to first token to 18 GB / 100% GPU /
  449 ms. The OpenAI-compatible path remains for llama.cpp, vLLM and others.
- **Streaming** answers, spoken from the first full sentence (never a clipped
  fragment, never the model's reasoning), with the bubble filling in as it
  arrives. `llm.thinking` gained `auto`.
- **GPU-aware loading**: unload other models first if the next would not fit,
  and prefer a smaller role over a model bigger than the card (`llm.manageVram`).
- **Context management**: recent turns within a token budget, older turns folded
  into a running summary by the fast model, tool output clipped.
- **Deterministic commands** for volume (`wpctl`), media (`playerctl`), the
  clock, locking, screenshots and video recording, plus model tools for the
  clipboard, notifications, system info and an allow-listed set of read-only
  commands; `tts.confirmCommands` makes trivial commands silent.
- **Qwen3-TTS** through any OpenAI-compatible speech server, with automatic
  fallback to Fish Speech (`tts.engine`: auto, qwen, fish, none). Not yet run
  against a live Qwen3-TTS server.
- **CLI**: `nala model status|list|main|fast|speed|mode|thinking|routing|unload`,
  `nala stt status`, `nala tts status`, `nala latency`, `nala benchmark
  [role]`, `nala --ptt`, `nala memory clear screen [all]`. `nala doctor` checks
  the three models, both voices, GPU spill and the desktop helpers.
- **Latency**: every stage timed; `nala latency`, and printed as it happens with
  `developer.debug`.
- **Screen history retention** by name (`memory.screenshotRetention`: off, 1h,
  1d, 7d, 30d, manual), checked every five minutes.

### Changed

- `llm.preferred` starts with `qwen3.8-27b`; Flash-Next is now an option for a
  bigger machine rather than the recommendation. `llm.thinking` defaults to
  `auto`, `llm.contextTurns` to 20. `tts.engine` defaults to `auto`.
- The flake's runtime tools include `wpctl`, `playerctl`, `wl-clipboard` and
  `notify-send`, and its check phase runs the new test suite.

### Fixed

- Found by measuring: a model whose capability list omits thinking (gemma4)
  still reasons silently when `think` is left out, so a short answer used its
  whole token budget on it. "Off" is now always sent.
- A key named `..._token` in a log record was redacted as a secret; timing keys
  no longer look like credentials.
- PKGBUILD still advertised 1.2.0 and skipped `nala-model-tests`. It now matches
  1.3.0, runs all three test binaries, and lists the new runtime optdepends.
- GitHub Actions runs `nix build` (the flake already executes the unit suites
  and the self-test in `checkPhase`) and checks that CMakeLists, flake.nix and
  PKGBUILD share a version.

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
