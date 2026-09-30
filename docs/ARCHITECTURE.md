# Architecture

Nala is two things in one process: the companion (unchanged in spirit from
upstream) and the assistant, which is built as a static library
(`nala_assistant`) so its unit tests link exactly the code the app runs.

```
            ┌──────────── companion ─────────────┐
            │ Mascot (behaviour)  Backend (glue) │◄── control socket: nala <command>
            │ Mascot.qml / shader  Settings.qml  │
            └──────▲──────────────────▲──────────┘
      state → cue  │                  │ settings / timeline windows
            ┌──────┴──────────────────┴──────────────────────────────┐
            │ Assistant (src/assistant/assistant.*)                  │
            │                                                        │
 mic ─► Microphone+VAD ─► SpeechToText ─► CommandRouter ─fast─► action│
            │   (audio.*)      (speech.*)       │                    │
            │                                   └─else─► LlmClient ◄─┼─► OpenAI-compatible server
            │                                             │  ▲       │
            │                                   tool calls▼  │results│
            │            ToolRegistry + policy (tools.*, policy.*)   │
            │              │           │            │                │
            │          desktop.*    MemoryStore   PathPolicy         │
            │   (Hyprland IPC, apps,  (memory.*)                     │
            │    wtype/ydotool, grim)    ▲                           │
            │                            │ ScreenMemory (screenmemory.*)
            │  reply ─► bubble + TextToSpeech (Fish Speech) ─► Speaker│
            └────────────────────────────────────────────────────────┘
```

## Hearing her name

```
microphone ─► frames ─► WakeWordBackend (wakeword.*) ── detected ──┐
     │                   CPU, 1 thread, nothing stored              │
     │                                                              ▼
     └─► voice-activity detector ─► utterance      Mascot::perk(), chime, "armed"
                                        │                           │
                                        ▼                           │
                           armed / follow-up / push-to-talk? ◄──────┘
                              no ─► dropped, never transcribed
                              yes ─► whisper.cpp ─► CommandRouter ─fast─► action
                                                          └─else─► model
```

The perk comes straight from the detection signal (`Assistant::wakeDetected`
→ `Mascot::perk()` in `main.cpp`), before whisper or the model do anything.
While she speaks the backend is paused and resumed `wake.postSpeechMs` after,
so her own voice cannot wake her. After a detected wake, a misheard wake
phrase is forgiven (`CommandRouter::routeAfterWake`).

## Identity

`identity.*` (name, personality, answer length, expressiveness, voice) and
`wake.*` are read into one `Identity` value (`identity.h`). It produces the
model's system prompt, whisper's priming prompt, the list of ways of
addressing her that are stripped from requests, and the router's name for
"open Nova's settings". Nothing in the AI code writes "Nala" itself; the pet,
the project and the tray icon are still Nala. `profile::export/import` moves
an allow-listed set of these settings between machines.

## Modules

| File | Responsibility |
| --- | --- |
| `settings.*` | Every assistant setting, declared once with its default and validation; persisted to `~/.config/nala/assistant.json` (0600). Unknown keys and bad values are refused, not clamped. |
| `eventlog.*` | Structured JSON-lines log (`~/.local/state/nala/assistant.log`, rotated at 4 MB) and a ring buffer for the developer page. Everything is redacted on the way in. |
| `commandrouter.*` | Pure, table-driven fast path from a transcript to an action. |
| `audioutil.*` | Pure audio: WAV encode/parse (streaming headers included), mono/16 kHz resampling, the voice-activity detector. |
| `audio.*` | Qt Multimedia devices: `Microphone` (with VAD) and `Speaker` (push mode, interruptible, reports level). |
| `speech.*` | `SpeechToText` (`WhisperServer`, `WhisperCli`) and `TextToSpeech` (`FishSpeech`) interfaces and backends. |
| `llm.*` | OpenAI-compatible chat client with tool calls; pure reply parser; strips `<think>` reasoning. |
| `tools.*` | Tool registry: JSON-schema arguments, validation, risk, category, OpenAI schema export. |
| `policy.*` | Pure rules: risk → allow/confirm; file path policy; the screen-memory privacy gate; time-phrase parsing. |
| `desktop.*` | Hyprland IPC (windows, monitors, cursor), `.desktop` application index and launch, `wtype`/`ydotool` input, `grim` capture. Never uses a shell. |
| `memory.*` | SQLite store (FTS5 when available), artifacts, retention, perceptual hash, artifact extraction. |
| `guigrounder.*`, `assistant_gui.cpp` | Bounded marked visual grounding, crop transforms, privacy/focus checks, confirmed actions and verification; injected desktop ports for offline tests. |
| `retrieval.*`, `semantic.*`, `memory_semantic.cpp` | Async local embedding/rewrite/rerank calls, SQLite migrations, vector scoring, RRF, durable facts, entity links and provenance checks. |
| `screenmemory.*` | The capture pipeline, pause/resume, retention timer, optional vision judging and descriptions. |
| `assistant.*` | Orchestration: state machine, fast actions, the agent loop, confirmations, speaking, tool implementations, diagnostics, timeline data. |
| `assistant_models.cpp` | The local-model layer: choosing main / fast / speed per request, fallback and retry, GPU preparation, streaming the answer into the voice, conversation summary, timing, and the `nala model`, `stt`, `tts`, `benchmark` commands. See [models.md](models.md). |
| `assistant_tools.cpp` | Volume, media, clock, clipboard, notifications, screenshots, video recording and read-only commands, and their fast path. See [tools.md](tools.md). |
| `modelrouter.*` | Pure: request → role, fallback order, loose name matching, "use the fast model" phrases. |
| `contextbudget.*` | Pure: the token budget, recent-turn window, tool-output clipping and the summary request. |
| `modelcatalog.*` | Ollama's model lists and `nvidia-smi` parsed; the VRAM eviction plan. |
| `sentencestream.*` | Pure: sentence chunking for speech, markdown clean-up, and the filter that keeps `<think>` out of the voice. |
| `latency.*` | Per-stage timings and the report. |
| `systemtools.*` | Pure: command lines for the desktop tools, and the allow-list. |
| `tts.*` | Qwen3-TTS (OpenAI-compatible speech API) and the chain that falls back to Fish Speech. |
| `assistant_voice.cpp` | The wake word in the assistant: arming, the chime, training sessions, model download, profiles, setup. |
| `identity.*` | Who she is, and everything derived from it; profiles. |
| `wakeword.*` | Wake-word engine: ONNX features, negative bank, trainer (augmentation, logistic regression, DTW templates, calibration), gate, `WakeWordBackend` / `NeuralBackend`. |
| `wakecli.*` | `nala wakeword …`, and the checksummed model download. |

## State

`Assistant::settle()` derives one state from a few flags, in priority order:
`error` > `speaking` > `acting` > `thinking` > `listening` > `sleeping` >
`idle` (`listening` also covers being armed by the wake word and the
follow-up window). `main.cpp` forwards it to `Mascot::setCue()`, which reuses her own
vocabulary (the thinking tumble, the exclamation, sleep) and does nothing at
all for `idle`, so the companion's own behaviour and its self-test checks
are untouched when the assistant is quiet.

Screen-memory privacy is not a state: the assistant stays fully usable while
memory is paused. It is shown separately, as a mark on her body.

## Threading

Everything runs on the GUI thread except frame scaling/hashing/encoding,
wake-word training and the model download (`QtConcurrent`, each with its own
ONNX session). The wake-word detector itself runs on the GUI thread: about a
millisecond of work per 80 ms block. Network (model, whisper-server, Fish Speech) and helper
processes (`whisper-cli`, `grim`, shell) are asynchronous; nothing blocks
the UI except short Hyprland IPC calls (sub-millisecond, 600 ms timeout).
Every in-flight operation can be cancelled: `stop()` bumps the turn counter so
late results are dropped, and screen memory has its own epoch for the same
purpose.

## The agent loop

1. Transcript → `CommandRouter`. A match runs a fast action (tools included,
   through the same permission path). "Use the fast model" and similar phrases
   are handled here too.
2. Otherwise `modelrouter` picks a role, the model list decides which model
   that is (with fallback), the GPU is made ready, and the model gets: system
   prompt (persona, time, memory status, vision capability), a short summary of
   older turns, the recent turns that fit `llm.contextTokens`, the request, and
   the tool schema for enabled categories. The answer streams; the voice starts
   on the first full sentence ([voice-pipeline.md](voice-pipeline.md)).
3. Each tool call is validated against its schema, checked against its
   category switch and risk, possibly confirmed with the user, run, logged,
   and its result fed back. Screenshots go back as images.
4. Up to `agent.maxSteps` rounds, then the final text is spoken.

Only the user's request and the final answer are kept in history, not the
tool traffic, so stale tool results are not trusted later and the context
stays small.

## Grounding and retrieval

Single named clicks reach `computer.locate_and_click` directly through the
router, then use a dedicated vision client. App/window tools stay deterministic.
The GUI loop owns its deadline and cancellation; retries require confirmation.
Memory intent alone invokes hybrid retrieval, with optional fast query rewriting.
Memory answers wait for evidence ID validation before speech. Backend indexing
is asynchronous and uses the existing SQL connection on its owning thread;
in-process cosine scoring currently runs there too. No mandatory ML libraries
or databases were added. See [computer-use.md](computer-use.md),
[semantic-search.md](semantic-search.md) and [memory.md](memory.md).
