# Handoff notes

1.5 streaming/controller implementation is documented in docs/tts-streaming.md and docs/computer-use.md. New offline suite: nala-stream-tests. X2 is a separate pinned Python/TensorRT service; do not vendor its runtime or checkpoints into the Qt repository. Cold startup, PCM onset and physical audible onset are different measurements. Existing policy and turn-generation checks remain authoritative.

For whoever works on this next — person or agent. Read this, then
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Where things stand (1.3.10+)

`nala tts status` uses the same HTTP 2xx rule as doctor for X2 `/readyz` and
Fish `/v1/health`. A 4xx/5xx page leaves `m_voiceBroken` set.

1.3.10: mic recovery verified against a real PipeWire server. Qt does NOT report a
removed PipeWire source (no signal, stale list, and the server re-links the stream to
the default mic), so a selected microphone is watched via `pw-dump` every 3 s and
reopened only when PipeWire lists it again. Run `~/Projects/r3dg0d-maintenance/nala-live-mic.sh`-style
checks with a `pw-loopback` virtual source (see CHANGELOG). Older backends still use
Qt's error/device signals, unit-tested via a loss hook only —
the speaker also reports a dead output (`Speaker::pump`) instead of hanging;
a real unplug is **not verified live**.

1.3.9: `nala tts status` syncs the sticky voice-broken flag with live
Qwen/Fish probes (same `/v1/models` and `/v1/health` paths as doctor) — clear
when an applicable engine answers; mark broken when none do. Engine `none`
stays intentional silence.

1.3.8: `nala stt status` in auto mode syncs the sticky auto→cli flag with a
live probe (recover when whisper-server answers; report cli use when it does
not). Doctor already cleared sticky on a successful probe; status matches that
contract. Cli/server modes no longer claim a fallback.

1.3.7: command-router pause/forget durations accept `month`/`months`
(same as delete-older), so spoken month spans stay on the fast path; offline
tests cover STT `auto`→whisper-cli fallback when the server is down
(`m_serverDead` sticky until an `stt.*` setting change or a successful
`stt status` / doctor probe).

1.3.6: `status()` prefers paused over off when the killswitch is armed
while disabled; Settings Resume stays available in that case.

1.3.5: oneshot no longer arms screen capture when memory is enabled;
`nala memory status|pause|resume|clear screen` works without a live companion
(same headless path as doctor).

1.3.4 clears `m_voiceBroken` after the TtsChain cooldown so a returning voice
server is found without a settings tweak, and joins Fish/Qwen `/v1/...` paths
without doubling slashes when endpoints have a trailing slash.
1.3.3 adds a barge-in echo guard (stricter wake gate while she speaks with
`wake.bargeIn`) and ships the doctor exit contract: `nala doctor` exits `1`
when any required line is `FAIL` (optional `--` lines do not).
1.3.2 made `nala doctor` (and `model` / `stt` / `tts` status) work when the
companion is not running — a headless oneshot Assistant answers and exits.
1.3.1 was the packaging / headless-CLI bump: `nala --version` early-exits
before Qt, doctor prints the version first, install-check and CI assert it,
and NixOS docs show how to pin a release tag.

1.3.0 added the multi-model layer (see [docs/models.md](docs/models.md)):
main / fast / speed routing with fallback, the native Ollama API, streaming
speech, GPU-aware loading, context summarisation, the system tools, Qwen3-TTS
with a Fish Speech fallback, timing, `nala model|benchmark|latency|stt|tts`.
`nala-model-tests` (146 tests, no server or GPU) covers it against fake HTTP
servers.

**Verified live** (RTX 4090, Ollama 0.34.3, a local Qwen 27B and gemma4-coder):
routing to the right model, VRAM eviction before a load, the 16384 context at
100% GPU, warm first token 449 ms, `nala model status|list`, `nala doctor`,
`nala benchmark`, `nala latency`, whisper-server and ydotoold as services.

**TTS recovery (1.3.4):** after a TTS/speaker failure, `m_voiceBroken` clears
when the TtsChain cooldown ends so a returning server is found without a
settings tweak; covered by offline FakeServer tests.

**Not verified:** a live Qwen3-TTS server (none installed; the client is written
against the documented `/v1/audio/speech` API and tested against a fake), Fish
Speech, `gpt-oss:20b` and `qwen3:30b-a3b` (not installed on the development
machine), the speech path with a real audio device, `record.start` with a real
recorder, clipboard/notification tools against a live compositor.

## Where things stood at 1.2.0

This fork adds a local voice assistant to yappologistic's Nala companion. 1.1.0
added the assistant; 1.2.0 added the wake word, the assistant's identity, the
setup wizard and Qwen3.8-Flash-Next as the recommended model.

Verified on this project's development machine (NixOS, Hyprland, RTX 4090,
32 GB RAM):

- clean build under `-Wall -Wextra -Wpedantic`; `nix build` runs both suites;
- 88 unit tests (3 more opt-in against live servers), the self-test, the
  install check;
- the wake-word engine against held-out synthetic speech and 3.6 h of real
  speech (LibriSpeech): numbers in docs/WAKEWORD.md;
- the whole voice path in the real app in a headless Wayland session, with
  audio played in through `nala hear` (developer mode): "Hey Nala" → perk and
  listening → "Open Nala settings" (fast path, no model) → "Hey Nala, open
  terminal" (Ghostty launched) → pause / resume screen recording (fast path,
  including a misheard "Hit Nala") → a question answered by the local model in
  2.7 s → unaddressed speech dropped untranscribed → rename to Nova by profile
  import, train "Hey Nova", restart: "Hey Nala" ignored, "Hey Nova" works,
  "Hey Nova, turn off screen recording" pauses memory;
- whisper.cpp 1.9.2 (server and cli), Ollama 0.33 tool calling and thinking
  control.

**Not verified:** Fish Speech (not installed here; covered by parser tests
and the opt-in `liveFishSpeech`), so spoken answers, interrupting speech by
voice and the chime were not heard; a real microphone and a real human voice
(all speech was synthetic); Qwen3.8-Flash-Next itself (72.5 GB at its
smallest, does not fit this machine); screen capture on a live Hyprland
session; `ydotool`.

## Known limitations

- The wake word is personalised: other people are caught about half the
  time, and a phrase one sound away in the same voice ("Hey Nova" for a "Hey
  Nala" model) can wake her.
- Measured 0.83 false wakes an hour on real speech, above openWakeWord's 0.5
  target. More negative data (openWakeWord's 2000 h ACAV100M features, 17 GB)
  would likely help; not tried.
- No echo cancellation: the detector is suspended while she speaks, so
  interrupting her by voice needs barge-in (which applies a temporary
  stricter wake gate / echo guard, still not true AEC) or a tap.
- The microphone indicator shows "off" when audio is injected for testing.
- Screen memory and window tools require Hyprland.
- Search is full-text plus time ranges; no embeddings.

## Suggested next steps

1. Real-voice wake-word evaluation: record a few people, run
   `nala wakeword eval`, and tune the calibration (currently the centre of
   the feasible region; see `Trainer::train`).
2. Stand up Fish Speech and run `liveFishSpeech`; check barge-in with
   headphones.
3. Acoustic echo cancellation (WebRTC AEC or PipeWire's echo-cancel module)
   would allow interrupting her by voice safely.
4. Run Qwen3.8-Flash-Next on a machine that holds it and check tool calling
   through vLLM's parser.
5. Embedding search for memories; a browser integration for real URLs.

## Conventions

- Model choice lives in `modelrouter`/`contextbudget`/`modelcatalog` (pure,
  tested); `assistant_models.cpp` only connects them. Do not put routing rules in
  `assistant.cpp`.
- Every desktop command line is built in `systemtools.cpp` as program + args,
  never a shell string.
- Ollama's OpenAI-compatible endpoint cannot set the context window; that is why
  `LlmClient` has a native path. Check `nala model status` for CPU spill after
  changing models.

- Settings: declared once in `src/assistant/settings.cpp`; validated, never
  clamped.
- Anything consequential goes through `Assistant::callTool()`.
- Never add a model-facing way to resume screen memory.
- Nothing in the assistant writes "Nala": use `Identity` (`m_identity.name`).
- Wake-word audio is never written to disk; only training recordings are,
  and only on an explicit Record.
- Tests must not touch the real desktop: keep `HYPRLAND_INSTANCE_SIGNATURE`
  unset in them.
- moc cannot read raw string literals in a file with `Q_OBJECT`: it silently
  generates nothing. Build JSON fixtures in code instead.
