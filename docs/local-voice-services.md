# Local voice services

Since 1.5, `auto` prefers the separate [X2 streaming service](tts-streaming.md), then Fish. The Qwen HTTP adapter below is legacy compatibility and useful for comparisons; avoid enabling it alongside X2 on limited VRAM.

The two optional adapters use official model implementations locally on CUDA:

| Service | Model | Loopback port | Output |
| --- | --- | --- | --- |
| Qwen | Qwen3-TTS-12Hz-0.6B-CustomVoice | 8880 | 24 kHz WAV / signed 16-bit PCM |
| Fish fallback | Fish Speech 1.5 | 8080 | 44.1 kHz WAV |

They serialize generation within each model and generate a complete sentence
before returning audio. `stream: true` requests work, but these adapters do not
implement incremental inference. Model loading happens once per service startup.
Both bind only to 127.0.0.1 and need no API key. Qwen's original `alloy` setting
maps to Ryan; other CustomVoice speakers can be selected by their model names.

Use `tts.engine = qwen` for this legacy adapter; `auto` now tries X2 followed by Fish. Explicit
`qwen` or `fish` selects only that engine and disables the other fallback.

## Installation on NixOS

Run from this repository with Python 3.12 and `uv` available. Install the two
environments separately: their NumPy/Transformers constraints differ. Training,
ASR, Gradio and PortAudio dependencies from Fish's full development environment
are unnecessary for this adapter.

```bash
voice_dir="$HOME/.local/share/nala/voice"
mkdir -p "$voice_dir"
nix build nixpkgs#python312 --out-link "$voice_dir/python-runtime"
uv venv --python "$voice_dir/python-runtime/bin/python" "$voice_dir/qwen-env"
uv venv --python "$voice_dir/python-runtime/bin/python" "$voice_dir/fish-env"
uv pip install --python "$voice_dir/qwen-env/bin/python" -r scripts/voice-qwen-requirements.txt
uv pip install --python "$voice_dir/fish-env/bin/python" -r scripts/voice-fish-requirements.txt
git clone --branch v1.5.1 --depth 1 https://github.com/fishaudio/fish-speech.git "$voice_dir/fish-speech"
cp scripts/{qwen,fish}_tts_server.py "$voice_dir/"
nix build nixpkgs#sox --out-link "$voice_dir/sox"
gcc_runtime="$(nix eval --raw nixpkgs#stdenv.cc.cc.lib.outPath)"
nix-store --add-root "$voice_dir/gcc-runtime" --indirect --realise "$gcc_runtime"
```

Create `runtime.env` in that directory. Its values must be absolute paths;
EnvironmentFile does not expand shell variables. Include Nix's GCC library,
NVIDIA driver, and SoX executable paths:

```text
LD_LIBRARY_PATH=/run/opengl-driver/lib:/run/current-system/sw/lib:/absolute/voice/gcc-runtime/lib
PATH=/absolute/voice/sox/bin:/run/current-system/sw/bin
NALA_QWEN_MODEL=/absolute/huggingface/snapshot/path
```

Before enabling offline startup, download the official models with
`huggingface_hub.snapshot_download` using the Qwen environment. Set
`NALA_QWEN_MODEL` to the **returned local snapshot path**; this avoids a
Transformers metadata request during offline tokenizer loading.

```python
from huggingface_hub import snapshot_download

print(snapshot_download("Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice"))
snapshot_download(
    "fishaudio/fish-speech-1.5",
    local_dir="/absolute/voice/fish-speech/checkpoints/fish-speech-1.5",
    allow_patterns=["*.json", "*.pth", "*.tiktoken"],
)
```

Install the unit templates, then enable them to start at login and restart on
failure. The restart rate is bounded so configuration failures cannot loop
indefinitely.

```bash
mkdir -p "$HOME/.config/systemd/user"
cp packaging/systemd/nala-{qwen,fish}-tts.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable --now nala-qwen-tts nala-fish-tts
curl -f http://127.0.0.1:8880/v1/models
curl -f http://127.0.0.1:8080/v1/health
nala tts status
```

Inspect startup failures with `journalctl --user -u nala-qwen-tts -u
nala-fish-tts`. Check actual synthesis as well as health: an empty or broken
response is not a working voice. The optional test exercises Qwen PCM through
Nala's client, then a disconnected primary with real Fish WAV fallback, without
playing audio or changing production endpoints:

```bash
NALA_TEST_VOICES=1 build/nala-model-tests liveLocalVoiceChain
```

On the development RTX 4090 both real services and this chain test passed.
Direct short-sentence requests took approximately 2.7 s for Qwen and 2.1 s for
Fish, including first-generation work. These are measurements of complete
sentence generation, not streaming time-to-first-audio claims. Larger Ollama
models may compete for GPU memory; these smaller speech models leave more room
than Fish S2-Pro, whose official installation targets a 24 GB GPU by itself.

Model implementations and weights have separate licenses; consult the
[Qwen3-TTS repository](https://github.com/QwenLM/Qwen3-TTS) and
[Fish Speech 1.5 model card](https://huggingface.co/fishaudio/fish-speech-1.5).
