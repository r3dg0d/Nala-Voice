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
| `screenmemory.*` | The capture pipeline, pause/resume, retention timer, optional vision judging and descriptions. |
| `assistant.*` | Orchestration: state machine, fast actions, the agent loop, confirmations, speaking, tool implementations, diagnostics, timeline data. |

## State

`Assistant::settle()` derives one state from a few flags, in priority order:
`error` > `speaking` > `acting` > `thinking` > `listening` > `sleeping` >
`idle`. `main.cpp` forwards it to `Mascot::setCue()`, which reuses her own
vocabulary (the thinking tumble, the exclamation, sleep) and does nothing at
all for `idle`, so the companion's own behaviour and its 142 self-test checks
are untouched when the assistant is quiet.

Screen-memory privacy is not a state: the assistant stays fully usable while
memory is paused. It is shown separately, as a mark on her body.

## Threading

Everything runs on the GUI thread except frame scaling/hashing/encoding
(`QtConcurrent`). Network (model, whisper-server, Fish Speech) and helper
processes (`whisper-cli`, `grim`, shell) are asynchronous; nothing blocks
the UI except short Hyprland IPC calls (sub-millisecond, 600 ms timeout).
Every in-flight operation can be cancelled: `stop()` bumps the turn counter so
late results are dropped, and screen memory has its own epoch for the same
purpose.

## The agent loop

1. Transcript → `CommandRouter`. A match runs a fast action (tools included,
   through the same permission path).
2. Otherwise the model gets: system prompt (persona, time, memory status,
   vision capability), the last `llm.contextTurns` exchanges, the request, and
   the tool schema for enabled categories.
3. Each tool call is validated against its schema, checked against its
   category switch and risk, possibly confirmed with the user, run, logged,
   and its result fed back. Screenshots go back as images.
4. Up to `agent.maxSteps` rounds, then the final text is spoken.

Only the user's request and the final answer are kept in history, not the
tool traffic, so stale tool results are not trusted later and the context
stays small.
