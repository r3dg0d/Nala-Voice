# Models

Nala runs three local models, each for what it is good at, and chooses between
them per request. Nothing is ever sent to a cloud model: if a local model is
missing or fails, another local one stands in, and if none can, she says so.

| Role | Default name | Used for |
| --- | --- | --- |
| **main** | Qwen3.8-27B (`llm.mainModel`) | reasoning, planning, code and debugging, long or multi-step requests, tool use, the default when in doubt |
| **fast** | `gpt-oss:20b` (`llm.fastModel`) | short chat, simple questions, summaries, jokes, classification |
| **speed** | `qwen3:30b-a3b` (`llm.speedModel`) | a second low-latency model; light conversation if you route it there |

The names are **configurable and matched loosely** against what your server
lists (case and punctuation are ignored), so `qwen3-30b-a3b` finds
`qwen3:30b-a3b`. Nala does not assume an exact tag, and it downloads nothing:
a missing model is reported with the command to fix it, never fetched behind
your back.

## Getting the models

```bash
ollama pull gpt-oss:20b
ollama pull qwen3:30b-a3b
```

For the main model, use whichever Qwen3.8-27B build you have. Nala's default
`llm.mainModel` is empty, meaning "the first of `llm.preferred` the server has"
(`qwen3.8-27b`, then Flash-Next, then other Qwens). If your build has another
tag, tell Nala: `nala model main <tag>`. The exact Ollama tag for a 27B Q4_K_M
is not something this repository can vouch for; check your library
(`nala model list`), or import a GGUF (`ollama create`, or
`ollama pull hf.co/<repo>:Q4_K_M`).

`nala model status` shows what is configured, what is installed, what is loaded
and how much of it is on the GPU.

## How a request is routed

1. **Deterministic commands never reach a model.** "Turn the volume down to
   40%", "pause", "next song", "what time is it", "open Firefox", "lock the
   screen", "stop screen recording" are matched by the command router in
   microseconds. See [tools.md](tools.md).
2. **A cheap word-based classifier** (no model call, microseconds) picks a role
   for everything else:

   | Request | Goes to |
   | --- | --- |
   | "Tell me a joke." / "How are you?" | fast |
   | "Explain how Nix flakes work." | main |
   | "Help me debug this Rust compiler error." | main |
   | "Analyze this source tree and explain why it crashes." | main |
   | a request over ~30 words | main |
   | "Summarize this." | fast |

3. **Your overrides win.** `llm.mode` = `main` / `fast` / `speed` forces one
   role; `llm.autoRouting = false` sends everything to main;
   `llm.route.conversation|tools|coding|summary|classify` set a role per kind
   of request (`auto` leaves it to the classifier). Reasoning requests still go
   to main even when conversation is routed elsewhere.

Say it instead: "**use the fast model**" (the next question only), "**use the
smart model for this**", "**from now on use the main model**", "**switch back to
automatic model selection**". Only wording like "from now on" or "always"
changes your saved defaults.

## Fallback

If the model for a role is not installed, the next role stands in:

```
main  -> fast  -> speed
fast  -> speed -> main
speed -> fast  -> main
```

Reorder with `llm.fallback.main|fast|speed` (for example `["speed","fast"]`).
If none of the three is installed and `llm.useAnyLocalModel` is on (default), the
best other local model is used, again never a cloud one. If a model fails while
running (out of memory, a broken file) and nothing has been said yet, the same
request is retried once on the next local model. `nala model status` and
`nala doctor` say which role is standing in.

## Thinking

`llm.thinking`: `off`, `on`, `auto` (default) or `server`.

`auto` turns thinking on only for the **main** model on requests that need
reasoning, and off otherwise, because a spoken answer should not wait on hidden
reasoning. The reasoning text is never spoken, never shown, and never stored in
the conversation. `gpt-oss` cannot be switched off; it is held at its `low`
level instead. Models that do not think are not sent `think: true`.

## Context and memory

`llm.contextTokens` (default 16384) is the context window **Nala asks Ollama
for**. This matters more than it sounds: measured on the development machine, a
27B model whose Modelfile says `num_ctx 131072` loaded at that size through
Ollama's OpenAI-compatible endpoint, took 27 GB, ran 22% from system RAM, and
needed 52 s to a first token. The same model at 16384 through the native API
is 18 GB, 100% on the GPU, and answers a casual question in 449 ms to first
token (1.26 s total).

- Recent turns: the last `llm.contextTurns` exchanges (default 20), within the
  token budget.
- Older turns are folded into a short running summary by the fast model
  (`llm.conversationSummary`), not resent and not simply dropped.
- Tool output is clipped to `llm.maxToolOutputChars` (default 12000) before it
  is shown to the model or kept.
- `llm.keepAlive` (default `30m`) keeps a model in memory between requests so
  the next one does not wait for a reload.

## The GPU

With `llm.manageVram` (default on), before a model that is not already
resident is loaded, Nala asks `nvidia-smi` how much room there is. If it would
not fit alongside what is loaded, the largest other models are unloaded first
(so the new one loads entirely on the GPU instead of partly in system RAM). In
automatic mode, a model bigger than the whole card is passed over for a smaller
role. An explicit `llm.mode` is your choice and is left alone. Nothing here is
specific to a particular card; it reads what `nvidia-smi` reports.

Screen memory can describe pictures with a different model
(`llm.visionModel`) so a large vision model need not stay loaded.

## Backends

`llm.provider`: `auto` (asks the server who it is), `ollama`, or `llamacpp`.

- **Ollama** (default): Nala uses its native `/api/chat` (`llm.ollamaNative`)
  for the context window, keep-alive, exact thinking control and real token
  timings. Set it false to use the OpenAI-compatible endpoint instead.
- **llama.cpp**: run `llama-server -m model.gguf -c 16384 -ngl 99 --port 8081`,
  then set `llm.endpoint` to `http://127.0.0.1:8081/v1` and
  `llm.provider` to `llamacpp`. One `llama-server` serves one model, so all
  three roles use it; to run several models, use Ollama.
- Anything OpenAI-compatible (vLLM, SGLang, LM Studio) works the same way.

## Commands

```bash
nala model status            # backend, GPU, each role, routing, thinking, context, STT, TTS
nala model list              # what the server has, and which role each model is
nala model main <name>      # set a role's model
nala model fast <name>
nala model speed <name>
nala model mode auto|main|fast|speed
nala model thinking off|on|auto
nala model routing on|off
nala model unload            # free the GPU
nala benchmark [main|fast|speed]
nala latency                 # where the last request's time went
```

When Nala is not running, `nala model main|fast|speed|mode|thinking <value>`
writes the setting so it applies on start.

## Settings reference

Nala keeps its settings in `~/.config/nala/assistant.json` (the project's
existing format, edited from Preferences or `nala model ...`), not a TOML file.

| Concept | Setting | Default |
| --- | --- | --- |
| provider | `llm.provider` | `auto` |
| endpoint | `llm.endpoint` | `http://127.0.0.1:11434/v1` |
| main / fast / speed model | `llm.mainModel` / `llm.fastModel` / `llm.speedModel` | empty (preferred list) / `gpt-oss:20b` / `qwen3:30b-a3b` |
| default mode | `llm.mode` | `auto` |
| auto routing | `llm.autoRouting` | `true` |
| per-task roles | `llm.route.conversation` `.tools` `.coding` `.summary` `.classify` | `auto` |
| fallback order | `llm.fallback.main` `.fast` `.speed` | built in |
| context length | `llm.contextTokens` | `16384` |
| recent turns | `llm.contextTurns` | `20` |
| conversation summary | `llm.conversationSummary` | `true` |
| tool output limit | `llm.maxToolOutputChars` | `12000` |
| temperature | `llm.temperature` | `0.6` |
| streaming | `llm.streaming` | `true` |
| thinking | `llm.thinking` | `auto` |
| keep alive | `llm.keepAlive` | `30m` |
| GPU management | `llm.manageVram` | `true` |
| any local model as last resort | `llm.useAnyLocalModel` | `true` |
| vision model for screen memory | `llm.visionModel` | empty (the main model) |

## Measured on the development machine

RTX 4090 (24 GB), Ollama 0.34.3, `nala benchmark` with thinking off. The
27B here is a locally built `qwen3.8-neo-coder:Q4_K_M` (27.3B, 17.2 GB) and the
fast model is `gemma4-coder` (11.9B, 7 GB); the `gpt-oss:20b` and
`qwen3:30b-a3b` models were not installed on this machine, so their numbers are
not claimed here. Run `nala benchmark` on your own hardware.

| Model | Request | First token | Tokens/s | Total | GPU peak |
| --- | --- | --- | --- | --- | --- |
| 27B (main) | command | 228 ms | 42.1 | 1749 ms | 19.8 GB |
| 27B (main) | conversation | 217 ms | 44.2 | 557 ms | 19.7 GB |
| 27B (main) | reasoning | 222 ms | 43.9 | 451 ms | 19.8 GB |
| 27B (main) | coding | 210 ms | 40.4 | 2811 ms | 19.7 GB |
| 27B (main) | tool call | (no text) | 40.1 | 1394 ms | 19.8 GB |
| gemma4 (fast) | command | 42 ms | 86.4 | 552 ms | 10.5 GB |
| gemma4 (fast) | conversation | 44 ms | 90.5 | 243 ms | 10.5 GB |
| gemma4 (fast) | reasoning | 50 ms | 89.3 | 163 ms | 10.5 GB |
| gemma4 (fast) | coding | 48 ms | 87.3 | 1137 ms | 10.5 GB |
| gemma4 (fast) | tool call | (no text) | 89.2 | 484 ms | 10.5 GB |

These are speed measurements, not a ranking of quality. A faster answer from a
smaller model is not a better one; use them to choose a latency profile. The two
models together (18 GB + 10 GB) do not fit a 24 GB card at once, which is why
Nala unloads one before loading the other; switching costs a model load
(13 s for the smaller here).
