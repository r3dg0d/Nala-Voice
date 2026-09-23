# Handoff notes

For whoever works on this next — person or agent. Read this, then
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Where things stand (1.2.0)

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
  interrupting her by voice needs barge-in (which risks self-wakes) or a tap.
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
