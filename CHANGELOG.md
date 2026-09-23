# Changelog

This project follows [semantic versioning](https://semver.org/). The version
lives in one place, `project(VERSION)` in `CMakeLists.txt`, and is what
`nala --version` reports.

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
