#!/usr/bin/env python3
"""Loopback-only native gateway for the pinned, patched X2 streaming engine.

The model and state inheritance belong to upstream. This adapter only selects
bounded single-user resources, privacy-safe logging and the listening address.
"""
import asyncio
import logging
import os
import signal
import sys
import time
from pathlib import Path


class EnglishTextNormalizer:
    def normalize(self, text):
        # English CustomVoice consumes ordinary orthography. X2 still holds
        # ambiguous numeric/unit suffixes; no Chinese normalization is applied.
        return text


async def main():
    root = Path(os.environ["NALA_X2_ENGINE_ROOT"]).resolve()
    x2 = Path(os.environ["NALA_X2_POLICY_ROOT"]).resolve()
    sys.path[:0] = [str(root), str(root / "client/src"), str(x2 / "src")]
    from aiohttp import web
    from engine.config import EngineConfig, load_model_manifest
    from engine.gateway.websocket_server import WebSocketGateway, add_health_routes
    from engine.server import TTSEngine, HealthState
    from engine.core.types import SessionConfig, InputMode, GroupPolicy, AudioConfig, AudioEncoding
    from engine.core import observability as obs
    from x2streaming_tts import X2StreamingPolicy
    from x2streaming_tts.config import X2StreamingConfig
    from x2streaming_tts.adapters.qwen3tts_streaming import build_policy_factories

    obs.configure("daily", "daily", text_capture="disabled")
    logging.basicConfig(level=logging.WARNING)
    logging.getLogger().setLevel(logging.WARNING)
    cfg = EngineConfig()
    cfg.scheduler.max_batch_size = 1
    cfg.scheduler.max_seq_len = 512
    cfg.server.max_sessions = 4
    cfg.server.warmup_rounds = 1
    cfg.server.prewarm_speakers = []
    cfg.server.guarded_delivery_default = True
    cfg.spliter.max_concurrent_segments = 1
    cfg.paths.engine_dir = str(root / "workspace/exported/custom-1.7b")
    cfg.paths.weights_dir = str(Path(cfg.paths.engine_dir) / "weights")
    cfg.paths.tokenizer_dir = str(root / "workspace/models/Qwen3-TTS-12Hz-1.7B-CustomVoice")
    arch = load_model_manifest(cfg.paths.engine_dir, cfg, tokenizer_dir=cfg.paths.tokenizer_dir)
    # Upstream special_text classifies Latin words as special spans. English
    # commitment is handled by Nala; keep capacity adaptation and inheritance.
    policy = X2StreamingPolicy(X2StreamingConfig(boundary_rules=()), text_normalizer=EnglishTextNormalizer())
    engine = TTSEngine(config=cfg, model_arch=arch, engine_dir=cfg.paths.engine_dir,
                       weights_dir=cfg.paths.weights_dir, tokenizer_dir=cfg.paths.tokenizer_dir,
                       device_id=0, max_batch_size=1, max_sessions=4, max_seq_len=512,
                       extensions=build_policy_factories(policy).to_upstream())
    started = time.monotonic()
    await engine.start()
    # Prefix caches include language. Upstream's Chinese default warmup does
    # not warm the English streaming-voice prefix used by Nala. Never play this audio.
    warmed = asyncio.Event()
    warm_pcm = False

    async def discard_audio(_session, data):
        nonlocal warm_pcm
        warm_pcm = warm_pcm or bool(data)

    async def warm_done(_session, _metrics):
        warmed.set()

    warm_id = "__nala_english_prewarm__"
    try:
        await engine.start_session(
            warm_id,
            config=SessionConfig(task_type="custom_voice", language="english",
                                 speaker="robot_service_v1", input_mode=InputMode.TOKEN,
                                 group_policy=GroupPolicy.NONE,
                                 audio=AudioConfig(encoding=AudioEncoding.PCM_S16LE)),
            on_audio=discard_audio, on_done=warm_done)
        await engine.push_text_input(warm_id, "Ready.")
        await engine.mark_input_complete(warm_id)
        await asyncio.wait_for(warmed.wait(), timeout=30)
        if not warm_pcm:
            raise RuntimeError("English prewarm produced no PCM")
    except Exception:
        await engine.cancel(warm_id)
        await engine.stop()
        raise
    load_ms = (time.monotonic() - started) * 1000
    health = HealthState(engine)
    health.mark_ready()
    gateway = WebSocketGateway(engine)
    app = web.Application(client_max_size=256 * 1024)
    app.router.add_get("/v1/ws", gateway.handle_websocket)
    app.router.add_get("/v1/capabilities", gateway.handle_capabilities)
    add_health_routes(app, health)

    async def diagnostics(_request):
        import torch
        _, code = health.payload_and_status("/readyz")
        return web.json_response({"ready": code == 200, "model_loaded": True,
                                  "gpu_ready": torch.cuda.is_available(), "warm": code == 200,
                                  "load_ms": round(load_ms, 1), "x2_state_inheritance": True,
                                  "allocated_mib": round(torch.cuda.memory_allocated() / 1048576),
                                  "reserved_mib": round(torch.cuda.memory_reserved() / 1048576)}, status=code)

    app.router.add_get("/nala/diagnostics", diagnostics)
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    await web.TCPSite(runner, "127.0.0.1", int(os.environ.get("NALA_X2_PORT", "50052"))).start()
    stopped = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stopped.set)
    try:
        while not stopped.is_set():
            try:
                await asyncio.wait_for(stopped.wait(), timeout=5)
            except asyncio.TimeoutError:
                if not engine.engine_thread_alive():
                    raise RuntimeError("X2 engine thread stopped; restarting service")
    finally:
        await runner.cleanup()
        await engine.stop()


if __name__ == "__main__":
    asyncio.run(main())
