# Troubleshooting

Start with `nala doctor`. It lists what is installed and running and what is
missing, and each line says how to fix it. It works whether or not the
companion is already running (as do `nala model status`, `nala stt status`,
`nala tts status`, and `nala memory status|pause|resume|clear screen`). Then dig
into whichever line failed.

Required checks print `ok` or `FAIL`; optional ones print `--` when missing
(voices, extra model roles, desktop helpers). For scripts, `nala doctor` exits
`0` when every required line is ok, and `1` when any required line is `FAIL`
(timeouts and "not running" for other commands also exit `1`).

## The answer takes many seconds

`nala model status` shows, for each loaded model, how much of it is on the GPU
and the context it was loaded with.

- **"only 78% on the GPU"** — part of the model runs from system RAM. The
  usual cause is an oversized context: a model whose Modelfile says
  `num_ctx 131072` needs a huge KV cache. Nala asks Ollama for
  `llm.contextTokens` (16384) through its native API (`llm.ollamaNative` must be
  on). If it still spills, lower `llm.contextTokens`, use a smaller quantisation,
  or unload other models (`nala model unload`). Measured: 27 GB / 22% CPU /
  52 s to first token at 131072, versus 18 GB / 100% GPU / 449 ms at 16384.
- **The first request after a while is slow** — the model was unloaded and is
  loading. `llm.keepAlive` (default `30m`) keeps it warm; `-1` is forever.
  `nala latency` shows the split.
- **Thinking** — `nala model thinking off` or `auto`. Reasoning models can spend
  seconds thinking before the first word; `auto` only does so for hard requests
  on the main model.
- **Two big models loaded** — a 27B and a 20B do not fit a 24 GB card together.
  Nala unloads one before loading the other (`llm.manageVram`), which costs a
  load each time you switch roles. Use `nala model mode fast` to stay on one.

## "I can't reach my brain"

`nala model status` says whether the server answers. Ollama:
`systemctl status ollama`, `ollama list`. A different server:
`llm.endpoint` and `llm.provider`. If a model runs out of memory, Nala retries
once on the next local model (`journalctl` is not needed; see
`~/.local/state/nala/assistant.log`, category `llm`).

## A model is "not installed"

`nala model list` shows what the server has. Set the role to a real name:
`nala model fast <tag>`. Names are matched loosely (`qwen3-30b-a3b` finds
`qwen3:30b-a3b`). Nala never downloads a model itself; `ollama pull <tag>`.

## No voice

`nala tts status` shows whether Qwen3-TTS and Fish Speech answer. Neither is
started by Nala; run an OpenAI-compatible Qwen3-TTS server on
`tts.qwen.endpoint` (default `http://127.0.0.1:8880`), or Fish Speech on
`tts.endpoint`. With neither, replies appear in the bubble. After a TTS or
speaker failure she skips the voice for the TtsChain cooldown (default 30 s),
then retries automatically when the server is back; change any `tts.*` setting
to retry immediately. Trailing slashes on those endpoints are fine. If the
voice is choppy or pitched wrong, `tts.qwen.sampleRate` is wrong for your
server.

## She does not hear me

`stt.activation` must match what you set up (`push`, `wake`, `always`).
`nala stt status` shows the recogniser. `whisper-server` down falls back to
`whisper-cli`, which needs a model in `~/.local/share/nala/whisper/`.
For a wake word, see [WAKEWORD.md](WAKEWORD.md); `nala --ptt` tests everything
after the wake word without it.

## Clicking and typing do nothing

`ydotool` needs its daemon and permission on `/dev/uinput`. On NixOS see
[nixos.md](nixos.md): `programs.ydotool.enable` and your user in the `ydotool`
group; log out and in after adding the group.

## Volume / media commands say nothing happened

They need PipeWire's `wpctl` and `playerctl`; `nala doctor` shows both. "Nothing
is playing" is `playerctl` finding no MPRIS player.

## The reasoning text is being read aloud

It should never be. If a model wraps reasoning in unusual tags, report the
model and tag; Nala strips `<think>…</think>` and the `thinking` field of
Ollama's API.

## Everything about the wake word, memory and privacy

[WAKEWORD.md](WAKEWORD.md), [MEMORY.md](MEMORY.md), [PRIVACY.md](PRIVACY.md).
