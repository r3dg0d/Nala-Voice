# Incremental local speech

Nala's default `auto` chain is X2Streaming-TTS → Fish Speech → text. The C++
client uses the patched upstream native WebSocket gateway; it is not a renamed
OpenAI speech client. The separate Python service owns CUDA inference and state.
Nala stays a Qt application, with Qt WebSockets and D-Bus added to its build.

## Commit, synthesize, play

LLM deltas pass through the existing reasoning filter and `SentenceStream`.
The commitment controller holds incomplete words, URLs, abbreviations, numeric
suffixes and open code/Markdown constructs. It can commit clauses, safe word
boundaries after a deadline, or conservative sentences. It does not call an LLM.

Each utterance has one UUID token-input session. Start declares CustomVoice, English,
24 kHz mono `pcm_s16le`; text packets have monotonic sequence numbers, and end
closes text input. Native binary PCM is decoded only after its format is declared.
Int16 sample fragments carry across messages. Session events are matched by ID.
Normal completion can reuse the socket, but a new turn has fresh acoustic state.
Cancellation sends cancel, retires the connection and rejects late callbacks.

Upstream extensions maintain the Code2Wav KV/conv/transconv state and a causal
Talker hidden-state bridge across healthy segments. This is acoustic inheritance,
not just keeping the transport open or retaining raw Talker KV. Unhealthy
segments clear inheritance rather than propagating corrupt context. Nala uses
bounded single-user resources: batch 1, four sessions, one concurrent segment,
256 input / 512 sequence tokens. Input is limited to 64,000 characters and queued
output to 60 seconds; larger sessions fail explicitly instead of dropping audio.

The Fish fallback buffers future committed text until the LLM finishes when X2
fails **before PCM**. A failure after PCM never replays the utterance. Explicit
`qwen` keeps the legacy `/v1/audio/speech` client and its settings unchanged;
`fish` selects Fish alone, `none` selects intentional silence. HTTP endpoints,
model IDs and legacy voice names are not copied into an incompatible X2 namespace.

## Runtime and model

The preferred checkpoint is the official streaming-trained
[x-square-robot/X2Streaming-TTS-1.7B](https://huggingface.co/x-square-robot/X2Streaming-TTS-1.7B),
revision `33858c61c73797136edd2e9bd427e2677c9b2601`, with built-in
`robot_service_v1`. It is a Mandarin-first service-assistant voice; English is
secondary and has lower naturalness. A Ryan-capable standard Qwen checkpoint is
a different model, not another voice in this checkpoint's speaker table.

Pinned policy: `e96f066150c2b2353854e56a790262a80a2fff3d`.
Pinned engine: `0745e4a8613f0780cc57475452ee775a9abac2dd` plus the two verified
factory/lifecycle patches. The original submodule remains unchanged; patches
are applied to a separate worktree. No upstream implementation or model weights
are vendored into Nala. English uses ordinary orthography; Chinese special-span
normalization is not applied to Latin words. Capacity adaptation/inheritance stay
active. Read [research notes](streaming-research.md) for the rationale.

The adapter listens only on `127.0.0.1:50052`, disables text capture/access logs,
and silently prewarms the actual English voice prefix before declaring ready.
Upstream guarded delivery remains enabled to retain retractable synthesis ahead
of the estimated playhead. A dead engine thread causes service failure/restart. Routes:

- `/v1/ws`: native speech protocol; `/v1/capabilities`: supported speakers.
- `/readyz`: readiness and engine-thread liveness.
- `/nala/diagnostics`: model/GPU/warm state, startup milliseconds and Torch memory.
  Torch memory excludes TensorRT weights; use NVIDIA's full-process allocation
  when budgeting VRAM.

## NixOS setup

The ML environment is separate from `nix develop` for the Qt application.
The following assumes a configured NVIDIA driver and Docker with NVIDIA CDI
(`docker run --device nvidia.com/gpu=0 ...`). It does not change the daemon,
install system packages or use apt/dnf/pacman.

From the repository, root Python and GCC in the voice directory:

```bash
voice_dir="$HOME/.local/share/nala/voice"
mkdir -p "$voice_dir"
nix build nixpkgs#python312 --out-link "$voice_dir/python-runtime"
nix build nixpkgs#gcc.cc.lib --out-link "$voice_dir/gcc-runtime"
nix build nixpkgs#sox --out-link "$voice_dir/sox"
export LD_LIBRARY_PATH="/run/opengl-driver/lib:/run/current-system/sw/lib:$voice_dir/gcc-runtime/lib"
export PATH="$voice_dir/sox/bin:$PATH"
"$voice_dir/python-runtime/bin/python3.12" scripts/setup_x2.py prepare
"$voice_dir/python-runtime/bin/python3.12" scripts/setup_x2.py models
"$voice_dir/python-runtime/bin/python3.12" scripts/setup_x2.py export
"$voice_dir/python-runtime/bin/python3.12" scripts/setup_x2.py build
```

Use `--voice-root` to prepare a separate root when existing checkouts/models have
other revisions. Preparation checks pins and patch integrity and refuses to
replace incompatible directories. About 4.5 GB of model files are downloaded;
ONNX/external weights/engine/export intermediates require additional disk space.
Export/build can approach the 32 GB host RAM limit. Stop the existing X2 service
before export/build, run the phases sequentially, and unload Nala's idle vision
model first. Fish can be restarted after the build if additional host RAM is needed. Do not stop unrelated applications.
The builder uses a pinned TensorRT 10.16.1 image, bounded workspace/batch profiles,
and separately validates the saved engine with the exact deployment runtime.
Optional `build_x2_engine.py --cp-precision fp32` trades more resources for predictor
precision; checkpoint quality still needs testing, independent of precision.

Create `runtime.env` with **absolute** paths (systemd does not expand variables):

```text
LD_LIBRARY_PATH=/run/opengl-driver/lib:/run/current-system/sw/lib:/absolute/voice/gcc-runtime/lib
PATH=/absolute/voice/sox/bin:/run/current-system/sw/bin
```

An existing legacy/Fish `runtime.env` can be reused. Then:

```bash
"$voice_dir/python-runtime/bin/python3.12" scripts/setup_x2.py install
systemctl --user status nala-x2-tts.service
curl --fail http://127.0.0.1:50052/nala/diagnostics
nala tts status
nala benchmark tts
```

Fish stays on port 8080; see [local-voice-services.md](local-voice-services.md).
Keep legacy Qwen on 8880 available for explicit compatibility/testing, but avoid
keeping both primary models resident. Installed units restart on failure and
start at login; model startup is offline after the download/export phases.

## Settings and measurement

`tts.engine`: `auto`, `x2streaming`, `qwen`, `fish`, `none`.
`tts.x2.endpoint`: `ws://127.0.0.1:50052/v1/ws` (validated WebSocket URL).
`tts.x2.voice`: `robot_service_v1`; `sessionReuse=true`; `prewarm=true` opens the
transport. The service's silent engine warmup is separate from transport warmup.
`tts.x2.commit.minChars`, `maxChars`, `maxDelayMs` and `mode` tune commitment;
`mode` is `clauses` or `sentences`. Defaults are 24 minimum characters, 220 maximum characters and 350 ms deadline.
Small prefixes need checkpoint-specific quality
checks; a short transport TTFB alone is not proof of complete or natural speech.

`nala latency` reports commit/session/output submission/completion stages and
output underruns. First PCM submission is a practical proxy, not physical audible
onset; device latency is additional. The silent `nala benchmark tts` measures
paced synthetic text, first commit, first PCM, commit-to-PCM, completion, generated
audio duration and delivery RTF. It cannot measure physical sound or underruns.
First request is not called a cold model measurement; startup is reported by the
service's `load_ms`. `nala-tts-bench --audio-dir /private/path` saves only its owned
synthetic fixtures for transcription/listening. `--legacy` measures the old Qwen
endpoint; that endpoint gets full text immediately, so compare backend TTFB,
not its input schedule against the paced streaming schedule.

The runtime is experimental. English quality, number reading and seamless segment
prosody require local validation. The six GUI fixtures and voice timing cases are
small engineering benchmarks, not general reliability scores. Additional engines
such as MOSS/CosyVoice are not bundled; the incremental backend boundary and chain
allow future integration without adding their model dependencies to Qt.

## Local validation, 2026-09-30

RTX 4090 / 24 GB, driver 595.104.02, BF16 streaming-trained 1.7B engine,
batch 1, 256/512 profiles. Warm confirmation: 39 ms commit-to-PCM; conversation:
48 ms (413 ms from paced input start); long response: 45 ms (412 ms from input
start, 12.081 s delivery completion, 26.320 s generated audio, RTF 0.459).
Legacy Qwen 0.6B full-text HTTP conversation: 2805 ms backend first PCM; long:
14751 ms. Models/voices and input schedules differ, so this is a local backend
comparison, not an equal-model quality benchmark.

Cold service restart to ready: 5.326 s; to first PCM: 5.364 s. Model loading and
silent warmup accounted for 4.479 s; ready-to-PCM was 38 ms. No output device was
used. Reproduce with the isolated environment's Python:
`python benchmarks/tts_cold.py --restart` (explicitly interrupts active X2 speech).

Full-process allocation is about 5 GiB for X2 and 2 GiB for Fish, separate from
Whisper, Ollama and display allocations. The service's Torch-only counters are
smaller. Keep idle models unloaded when building the engine; export/build host
memory is substantially higher than runtime memory.

Owned synthetic speech was transcribed by local Whisper: conversation,
punctuation, numbers and technical text matched; long speech covered the full
response with a pronunciation stumble near a segment boundary. English has a
different accent and lower naturalness than Mandarin. Healthy inherited-state
snapshots were reported on the long fixture, but seamless prosody is not proven
by a transport metric. See [raw benchmark results](../benchmarks/results/).
