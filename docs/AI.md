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

Nala primes whisper with `stt.prompt` ("Hey Nala. Pause screen memory. …").
Measured on this project's test clips with `tiny.en`, this changed the
transcription of "Hey Nala, pause screen memory" from "Hey Artler, pause
screen memory" (routed to the model) to the exact phrase (routed to the
privacy switch). Keep her name and the commands that matter in it.

### Listening modes (`stt.activation`)

- **push** (default): `nala listen` opens the microphone for one utterance.
  Bind it to a key, e.g. `bind = SUPER, N, exec, nala listen` in Hyprland.
  Wayland does not let applications grab global keys themselves.
- **wake**: the microphone stays open; utterances not starting (or ending)
  with a wake phrase are ignored. This is transcript-based wake detection —
  every utterance is transcribed — not a dedicated low-power wake-word model.
- **always**: every utterance is a request. Beware of television.

Voice-activity detection is energy-based with an adaptive noise floor
(`stt.vadThresholdDb`, `stt.silenceMs`). While she is speaking with an open
microphone she ignores everything except "stop"-type commands, because
without echo cancellation she would otherwise hear herself.

## The model — any OpenAI-compatible server

`llm.endpoint` (default `http://127.0.0.1:11434/v1`, Ollama) and `llm.model`
(empty = the first model the server lists). Works with Ollama, llama.cpp's
`llama-server`, vLLM, LM Studio and similar. `llm.apiKey` is sent as a Bearer
token when set, is stored in a 0600 file, and is never logged.

- **Tool calling** (`llm.toolCalling`): the server and model must support
  OpenAI-style `tools`. Tested end to end with Ollama 0.33 and a Qwen model:
  the model called `memory_search` on its own and answered from the result.
  Tool names are sent with underscores (`apps_launch`) because some servers
  reject dots.
- **Vision** (`llm.vision`): turn on only for a model that accepts images.
  Enables the screenshot tool, memory descriptions and the vision privacy
  check. Images are sent as base64 `image_url` data URIs.
- Reasoning models that emit `<think>…</think>` have it stripped before
  anything is spoken.

Any model can be used; a multimodal Qwen (for example `qwen2.5vl` in Ollama)
suits the vision features. Nothing in the code depends on a particular one.

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
NALA_TEST_WAV=clip.wav NALA_TEST_WHISPER_SERVER=http://127.0.0.1:8178 build/nala-assistant-tests liveWhisper
```
