"""Loopback speech adapter for the official Qwen3-TTS CustomVoice model.

The reference implementation generates a complete sentence before returning
audio. PCM is supported, but this adapter does not claim incremental inference.
"""

import io
import os
import threading
from contextlib import asynccontextmanager

import numpy as np
import soundfile as sf
import torch
from fastapi import FastAPI, HTTPException
from fastapi.responses import Response
from pydantic import BaseModel, Field
from qwen_tts import Qwen3TTSModel

MODEL = os.environ.get("NALA_QWEN_MODEL", "Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice")
lock = threading.Lock()
tts = None


@asynccontextmanager
async def lifespan(app):
    global tts
    torch.set_num_threads(4)
    tts = Qwen3TTSModel.from_pretrained(
        MODEL, device_map="cuda:0", dtype=torch.bfloat16,
        attn_implementation="sdpa",
    )
    yield


app = FastAPI(lifespan=lifespan)


class Speech(BaseModel):
    input: str = Field(min_length=1, max_length=4000)
    model: str = "tts-1"
    voice: str = "Ryan"
    response_format: str = "wav"
    stream: bool = False
    instruct: str = Field(default="", max_length=1000)


@app.get("/v1/models")
def models():
    return {"object": "list", "data": [{"id": "tts-1", "object": "model"}]}


@app.get("/v1/health")
def health():
    return {"status": "ok"}


@app.post("/v1/audio/speech")
def speech(req: Speech):
    if req.model not in ("tts-1", MODEL):
        raise HTTPException(400, "Unknown speech model")
    if req.response_format not in ("pcm", "wav"):
        raise HTTPException(400, "Use pcm or wav")
    # Preserve compatibility with Nala's original OpenAI voice default.
    speaker = "Ryan" if req.voice.lower() == "alloy" else req.voice
    voices = tts.get_supported_speakers()
    speaker = next((v for v in voices if v.lower() == speaker.lower()), None)
    if speaker is None:
        raise HTTPException(400, "Unknown CustomVoice speaker")
    # A single model must not receive overlapping generation requests.
    with lock, torch.inference_mode():
        wavs, rate = tts.generate_custom_voice(
            text=req.input, language="Auto", speaker=speaker,
            instruct=req.instruct, max_new_tokens=2048,
            do_sample=False,
        )
    if req.response_format == "pcm":
        if rate != 24000:
            raise HTTPException(500, "Model sample rate is incompatible with PCM")
        pcm = (np.clip(wavs[0], -1, 1) * 32767).astype("<i2").tobytes()
        return Response(pcm, media_type="application/octet-stream")
    buf = io.BytesIO()
    sf.write(buf, wavs[0], rate, format="WAV", subtype="PCM_16")
    return Response(buf.getvalue(), media_type="audio/wav")
