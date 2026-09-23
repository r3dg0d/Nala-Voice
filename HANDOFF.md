# Handoff notes

For whoever works on this next — person or agent. Read this, then
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Where things stand (1.1.0)

This fork adds a local voice assistant to yappologistic's Nala companion. The
companion itself is unchanged apart from bug fixes and assistant cues; all
original self-test checks still pass, plus one new one.

Verified on this project's development machine (NixOS, Hyprland, RTX 4090):

- build clean under `-Wall -Wextra -Wpedantic`; `nix build` runs both suites;
- 76 unit tests, 142 self-test checks, install check;
- whisper.cpp 1.9.2, both `whisper-server` and `whisper-cli`, on synthesised
  speech (~350 ms per utterance with `tiny.en`);
- the full agent loop against Ollama 0.33 with a local Qwen model: it chose
  `memory_search` itself and answered from the result;
- the real app in a headless Wayland compositor: bubble, privacy mark,
  preferences tabs, timeline, `nala ask/doctor/memory/timeline`.

**Not verified against a live install:** Fish Speech (implemented against its
`/v1/tts` API and covered by parser tests and the opt-in `liveFishSpeech`
test); screen capture and window tools on a live Hyprland session (the code
paths are unit-tested with injected frames and windows, but a real capture
loop has not been run in this environment); `ydotool` clicking; the
microphone path with real hardware (VAD tested on synthetic signals).

## Known limitations

- Wake word is transcript-based: in wake mode every utterance is transcribed.
- No echo cancellation: with an always-open mic, only "stop"-type commands
  are honoured while she is speaking.
- Screen memory and window tools require Hyprland (window identity is what the
  privacy gate works from). Other compositors: the assistant works, those
  features report themselves unavailable.
- Mouse coordinates map from the last screenshot of the focused monitor;
  multi-monitor pointing across outputs is untested.
- Title-based privacy rules cannot see window contents (see
  [docs/PRIVACY.md](docs/PRIVACY.md)).
- Search is full-text plus time ranges; no embeddings.

## Suggested next steps

1. Run Nala on a live Hyprland session with screen memory on for a day and
   tune `memory.dedupeDistance` from real frames.
2. Stand up Fish Speech and run `liveFishSpeech`; tune sentence splitting.
3. A real wake-word model (openWakeWord or similar) to stop transcribing
   everything in wake mode.
4. Embedding search: store vectors from the model server's `/embeddings`
   (sqlite-vec would keep it in one file).
5. Streaming model replies, so speech can start before the answer is done.
6. A browser integration (native messaging) for real URLs rather than titles.

## Conventions

- Settings: declared once in `src/assistant/settings.cpp`; validated, never
  clamped.
- Anything consequential goes through `Assistant::callTool()`.
- Never add a model-facing way to resume screen memory.
- Tests must not touch the real desktop: keep `HYPRLAND_INSTANCE_SIGNATURE`
  unset in them.
- moc cannot read raw string literals in a file with `Q_OBJECT`: it silently
  generates nothing. Build JSON fixtures in code instead.
