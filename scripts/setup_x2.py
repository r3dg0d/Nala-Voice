#!/usr/bin/env python3
"""Provision a pinned X2 checkout and isolated Python environment.

Run with Nix Python 3.12 and CUDA library paths; see docs/tts-streaming.md.
Model downloads/export/build are separate explicit phases. No sudo/daemon edits.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

POLICY = "e96f066150c2b2353854e56a790262a80a2fff3d"
ENGINE = "0745e4a8613f0780cc57475452ee775a9abac2dd"
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("phase", choices=("prepare", "models", "export", "build", "install"))
p.add_argument("--voice-root", type=Path, default=Path.home()/".local/share/nala/voice")
a = p.parse_args()
root = a.voice_root.expanduser().resolve()
root.mkdir(parents=True, exist_ok=True, mode=0o700)
policy = root/"x2streaming"
engine = root/"x2-engine"
venv = root/"x2-env"
repo = Path(__file__).resolve().parent.parent

def run(args, cwd=None):
    subprocess.run([str(v) for v in args], cwd=cwd, check=True)

if a.phase == "prepare":
    if sys.version_info[:2] != (3,12):
        p.error("Use the documented Nix Python 3.12 environment")
    if not policy.exists():
        run(["git", "clone", "https://github.com/X-Square-Robot/X2Streaming-TTS.git", policy])
        run(["git", "checkout", "--detach", POLICY], policy)
        run(["git", "submodule", "update", "--init", "--recursive"], policy)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=policy, text=True).strip()
    if actual != POLICY:
        p.error("Existing policy checkout differs from the verified pin; preserve it and choose another voice-root")
    run([sys.executable, "scripts/verify_upstream.py"], policy)
    run([sys.executable, "scripts/verify_patches.py"], policy)
    upstream = policy/"third_party/Qwen3TTS-Streaming"
    if not engine.exists():
        run(["git", "worktree", "add", "--detach", engine, ENGINE], upstream)
        for patch in sorted((policy/"patches/upstream"/ENGINE).glob("*.patch")):
            run(["git", "apply", patch], engine)
        nested = engine/"third_party/Qwen3-TTS"
        if nested.is_dir() and not any(nested.iterdir()): nested.rmdir()
        nested.symlink_to(upstream/"third_party/Qwen3-TTS", target_is_directory=True)
    else:
        actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=engine, text=True).strip()
        if actual != ENGINE: p.error("Existing engine differs from verified pin")
        for patch in sorted((policy/"patches/upstream"/ENGINE).glob("*.patch")):
            run(["git", "apply", "--reverse", "--check", patch], engine)
    if not venv.exists(): run([sys.executable, "-m", "venv", venv])
    run([venv/"bin/pip", "install", "-r", repo/"scripts/voice-x2-requirements.txt"])
elif a.phase == "models":
    code = """from huggingface_hub import snapshot_download
from pathlib import Path
import sys
engine=Path(sys.argv[1]); checkpoint=engine.parent/'x2-checkpoint'
snapshot_download('x-square-robot/X2Streaming-TTS-1.7B', revision='33858c61c73797136edd2e9bd427e2677c9b2601', local_dir=checkpoint)
models=engine/'workspace/models'
models.mkdir(parents=True, exist_ok=True)
for name, target in [('Qwen3-TTS-12Hz-1.7B-CustomVoice',checkpoint), ('Qwen3-TTS-Tokenizer-12Hz',checkpoint/'speech_tokenizer')]:
 dest=models/name
 if dest.exists():
  if dest.resolve()!=target.resolve(): raise RuntimeError('Preserve existing model directory and use another voice-root: '+str(dest))
 else: dest.symlink_to(target, target_is_directory=True)
"""
    run([venv/"bin/python", "-c", code, engine])
elif a.phase == "export":
    run([venv/"bin/python", "scripts/export/export_all.py", "--variant", "custom-1.7b",
         "--skip-tokenizer", "--skip-verification", "--device", "cuda:0"], engine)
elif a.phase == "build":
    run([venv/"bin/python", repo/"scripts/build_x2_engine.py", "--root", engine])
else:
    if not (engine/"workspace/exported/custom-1.7b/talker_code2wav_fused.engine").exists():
        p.error("Export/build the engine first")
    if root != (Path.home()/".local/share/nala/voice").resolve():
        p.error("Packaged unit uses the default voice-root; write a matching unit for custom roots")
    if not (root / "runtime.env").exists():
        p.error("Create runtime.env with absolute Nix/CUDA library paths first")
    shutil.copy2(repo/"scripts/x2_tts_server.py", root/"x2_tts_server.py")
    units = Path.home()/".config/systemd/user"
    units.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo/"packaging/systemd/nala-x2-tts.service", units/"nala-x2-tts.service")
    run(["systemctl", "--user", "daemon-reload"])
    run(["systemctl", "--user", "enable", "--now", "nala-x2-tts.service"])
