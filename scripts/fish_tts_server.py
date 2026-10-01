"""Loopback Fish Speech 1.5 adapter using the official inference engine.

Only speech inference is imported; the upstream ASR/WebUI/training servers
and their extra dependencies are unnecessary for Nala's fallback voice.
"""

import io
import os
import sys
import threading
from contextlib import asynccontextmanager
from pathlib import Path

import numpy as np
import soundfile as sf
import torch
from loguru import logger
from fastapi import FastAPI, HTTPException
from fastapi.responses import Response
from fish_speech.inference_engine import TTSInferenceEngine
from fish_speech.models.text2semantic.inference import launch_thread_safe_queue
from fish_speech.models.vqgan.inference import load_model
from fish_speech.utils.schema import ServeTTSRequest

# Upstream INFO logging includes generated text. Keep conversation content
# out of persistent service journals.
logger.remove()
logger.add(sys.stderr, level="WARNING")
checkpoint = Path(os.environ["NALA_FISH_CHECKPOINT"])
lock = threading.Lock()
engine = None


@asynccontextmanager
async def lifespan(app):
    global engine
    torch.set_num_threads(4)
    queue = launch_thread_safe_queue(
        checkpoint_path=checkpoint, device="cuda", precision=torch.bfloat16,
        compile=False,
    )
    decoder = load_model(
        config_name="firefly_gan_vq",
        checkpoint_path=checkpoint / "firefly-gan-vq-fsq-8x1024-21hz-generator.pth",
        device="cuda",
    )
    engine = TTSInferenceEngine(queue, decoder, torch.bfloat16, False)
    yield


app = FastAPI(lifespan=lifespan)


@app.get("/v1/health")
def health():
    return {"status": "ok"}


@app.post("/v1/tts")
def speech(req: ServeTTSRequest):
    if not req.text.strip() or len(req.text) > 4000 or req.format != "wav":
        raise HTTPException(400, "Supply 1..4000 characters and WAV format")
    # Return a complete WAV: Nala accepts this for streaming requests too.
    # This ensures inference errors return an HTTP error before any WAV header.
    req.streaming = False
    req.max_new_tokens = min(req.max_new_tokens or 2048, 2048)
    with lock:
        for result in engine.inference(req):
            if result.code == "error":
                raise HTTPException(500, "Fish Speech generation failed")
            if result.code == "final":
                rate, waveform = result.audio
                buf = io.BytesIO()
                sf.write(buf, np.clip(waveform, -1, 1), rate,
                         format="WAV", subtype="PCM_16")
                return Response(buf.getvalue(), media_type="audio/wav")
    raise HTTPException(500, "Fish Speech produced no audio")
