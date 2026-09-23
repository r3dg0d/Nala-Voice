# Setting up the local AI

Each backend is optional. `nala doctor` shows what Nala can reach.

## Speech recognition — whisper.cpp

Nala sends 16 kHz mono speech to whisper.cpp in one of two ways
(`Preferences → Assistant → Recogniser`, setting `stt.mode`):

- **whisper-server** (fastest; the model stays loaded):

  ```bash
  whisper-server -m ~/.local/share/nala/whisper/ggml-base.en.bin --port 8178
  ```

  Nala expects it at `http://127.0.0.1:8178` (`stt.serverUrl`). Port 8080 is
  whisper-server's own default, but it is also Fish Speech's, so Nala defaults
  to 8178.

- **whisper-cli** (nothing running in the background; loads the model per
  utterance). Nala uses `stt.model`, or the first `ggml-*.bin` in
  `~/.local/share/nala/whisper/`.

- **auto** (default) uses the server when it answers and falls back to the
  cli, for the utterance in hand and afterwards.

Models: `whisper-cpp-download-ggml-model base.en <dir>` (Nix) or
`models/download-ggml-model.sh` in a whisper.cpp checkout. `base.en` is a good
default; `tiny.en` is faster and worse; multilingual models need
`stt.language` set to a code or `auto`.

Nala primes whisper with her name and the commands that matter. Measured on
this project's test clips with `tiny.en`: "Hey Nala, pause screen memory" went
from "Hey Artler, pause screen memory" (routed to the model) to the exact
phrase, and "Open Nala settings" from "Open all settings" to the exact phrase.

### Listening modes (`stt.activation`)

- **wake**: a small local wake-word detector listens for her phrase ("Hey
  Nala") and nothing else is transcribed until it hears it; see
  [WAKEWORD.md](WAKEWORD.md). Until a phrase is trained, this mode falls back
  to transcribing every utterance and looking for the phrase in the text.
- **push** (default until set up): `nala listen` opens the microphone for one
  utterance. Bind it to a key, e.g. `bind = SUPER, N, exec, nala listen` in
  Hyprland (Wayland does not let applications grab global keys). Works in
  every mode. *Click her to talk* (`ui.clickToTalk`) does the same.
- **always**: every utterance is a request. Beware of television.

After she answers, `wake.followUpSec` (10 s by default) lets you carry on
without her name: "What's on my GPU?" … "And how much is free?".

Voice-activity detection is energy-based with an adaptive noise floor
(`stt.vadThresholdDb`, `stt.silenceMs`).

`stt.prompt` is empty by default, meaning "generate it from her identity":
her name, her wake phrases and the key commands. Renaming her updates it.
After a detected wake the router also forgives a misheard wake phrase ("Hit
Nala, resume screen memory" still resumes it).

## The model

### Qwen3.8-Flash-Next: the recommended brain

Nala is set up around [Qwen3.8-Flash-Next](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)
(open weights, qwen-community-1.0 licence): a mixture-of-experts model with
125B parameters of which 6B are active per token, plus 51B of n-gram
embeddings, reading text, images and video, with a 262k context. With
`llm.model` empty, Nala picks the first of `llm.preferred` the server offers
— Flash-Next first — matching loosely, so `hf.co/unsloth/Qwen3.8-Flash-Next-GGUF:UD-IQ1_S`
or `Qwen/Qwen3.8-Flash-Next` are both found; otherwise the best other Qwen,
otherwise whatever is listed first.

Serving it (from its model card):

```bash
vllm serve Qwen/Qwen3.8-Flash-Next                   # or SGLang
llama serve -hf unsloth/Qwen3.8-Flash-Next-GGUF:UD-IQ1_S
ollama run hf.co/unsloth/Qwen3.8-Flash-Next-GGUF:UD-IQ1_S
```

**What it needs.** It is large. The smallest published quantisation
(unsloth `UD-IQ1_S`) is **72.5 GB**; Q4 is 111 GB. A 24 GB GPU cannot hold
it, and the whole machine needs enough memory for GPU plus system RAM to
exceed the file. MoE offloading keeps the per-token work to the 6B active
parameters, so a 24 GB GPU with roughly 96 GB of system RAM should be the
practical floor -- an estimate from the file sizes, not a measurement. On the
development machine here (RTX 4090, 32 GB RAM) it does not fit at all, and it
was **not** run; Nala there used the next preference, a local Qwen 27B.

**Thinking.** Flash-Next reasons before it answers by default, which costs
seconds before she speaks. `llm.thinking` is `off` by default, and Nala sends
the switch each server understands: `chat_template_kwargs:
{"enable_thinking": false}` for vLLM, SGLang and llama.cpp (Qwen's template
switch), `reasoning_effort: "none"` for Ollama. Measured on Ollama 0.33 with a
local thinking Qwen for a one-word answer: 24.6 s with thinking (first call,
model loading), 0.5 s with `reasoning_effort: "none"`; Ollama ignores
`chat_template_kwargs` and its own `think: false` on this endpoint. If a
server rejects the parameter, Nala retries once without it and stops sending
it. Anything left in `<think>…</think>` is stripped before she speaks.

**Qwen3.8-Omni-Flash** is Qwen's omni-modal agent model, but it is published
only as a hosted API (Alibaba Model Studio / QwenCloud, no open weights), so
it cannot run locally and is not a default. Pointing `llm.endpoint` at an
OpenAI-compatible hosted endpoint with an API key works in principle, but
sends transcripts, and screenshots when vision is on, off the machine; it has
not been tested.

### Any OpenAI-compatible server

`llm.endpoint` (default `http://127.0.0.1:11434/v1`, Ollama). Works with
Ollama, llama.cpp's `llama-server`, vLLM, SGLang, LM Studio and similar.
`llm.apiKey` is sent as a Bearer token when set, is stored in a 0600 file, and
is never logged or exported.

- **Capabilities.** On Ollama, Nala asks `/api/show` what the model can do
  (`vision`, `tools`, `thinking`); elsewhere it guesses from the name (`vl`,
  `omni`, `vision`, `flash-next`…). `llm.vision` = `auto` follows that; `on`
  and `off` override it. Vision enables the screenshot tool, memory
  descriptions and the vision privacy check; images go as base64 `image_url`
  data URIs.
- **Tool calling** (`llm.toolCalling`): tested end to end with Ollama 0.33 and
  a local Qwen: the model called `memory_search` on its own and answered from
  the result. Tool names are sent with underscores (`apps_launch`) because some
  servers reject dots.
- **Errors** are translated: an out-of-GPU-memory failure says so, rather than
  quoting CUDA.

What goes to the model: only requests the fast command router could not
handle ("open Discord", "pause screen memory", "open settings" never reach
it), the last few exchanges, and tool results. Screen-memory frames go to it
only with `memory.describe` or `privacy.visionFilter` on.

### GPU and memory

| component | where it runs | notes |
| --- | --- | --- |
| wake word | CPU, one thread | measured ≈1 % of one i9-14900K core |
| whisper.cpp | GPU if the build has it; `stt.gpu` off adds `-ng` | `tiny.en`/`base.en` answer in ≈0.3–0.4 s on CPU |
| the model | GPU (the server's choice) | `llm.unloadIdleMin` asks Ollama to free it after idle minutes (`keep_alive: 0`) |
| Fish Speech | GPU when available | its own server |

**On a 24 GB card (RTX 4090)**: run a model that fits alongside everything
else — a ≈27–32B Qwen at Q4 (≈17–20 GB) leaves room for whisper and Fish
Speech; keep whisper on the CPU (`stt.gpu` off) if Fish Speech needs the
memory; set `llm.unloadIdleMin` if you use the GPU for other work (image
generation, games) between conversations. `nala doctor` shows GPU memory in
use, and she can answer "what's using my GPU?" through the `system_gpu` tool.

## Voice — Fish Speech

Nala talks to Fish Speech's API server (`tools/api_server.py` in the
fish-speech repository):

```
POST {tts.endpoint}/v1/tts   JSON {text, format: "wav", streaming, normalize,
                                   latency: "balanced", reference_id?}
GET  {tts.endpoint}/v1/health
```

Default endpoint `http://127.0.0.1:8080`. `tts.referenceId` picks a voice
you have added to the server; `tts.stylePrefix` is prepended to every line,
for Fish's expressive markers such as `(cheerful)`.

Replies are split into sentences, each synthesised and streamed to the
speaker as the WAV arrives (a streaming header of unknown length is handled).
"Stop", a tap on her, or `nala stop` aborts the request and silences the
speaker at once.

If Fish Speech is not running, `nala doctor` says so, and she answers in the
speech bubble only — nothing fails. The request format was checked against
the fish-speech source (`ServeTTSRequest`, `/v1/tts`); this repository's
tests exercise the streaming parser with the formats it emits, and
`liveFishSpeech` in `tests/assistant_tests.cpp` runs against a real server
when `NALA_TEST_FISH` is set. It has not been run against a live Fish Speech
install in this environment.

## Checking a setup

```bash
nala doctor
NALA_TEST_LLM=http://127.0.0.1:11434/v1 build/nala-assistant-tests liveModelCallsTools
nala wakeword test
NALA_TEST_WAV=clip.wav NALA_TEST_WHISPER_SERVER=http://127.0.0.1:8178 build/nala-assistant-tests liveWhisper
```
