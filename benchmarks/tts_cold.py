#!/usr/bin/env python3
"""Silent cold-service timing. Explicit --restart interrupts active X2 speech."""
import argparse
import asyncio
import json
import subprocess
import time
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--restart", action="store_true", required=True)
a = p.parse_args()

async def main():
    from aiohttp import ClientSession, ClientTimeout, ClientError, WSMsgType
    subprocess.run(["systemctl", "--user", "stop", "nala-x2-tts.service"], check=True)
    started = time.monotonic()
    subprocess.run(["systemctl", "--user", "start", "nala-x2-tts.service"], check=True)
    async with ClientSession(timeout=ClientTimeout(total=45)) as client:
        ready = None
        while time.monotonic() - started < 45:
            try:
                async with client.get("http://127.0.0.1:50052/readyz", timeout=ClientTimeout(total=1)) as response:
                    if response.status == 200:
                        ready = time.monotonic()
                        break
            except (ClientError, asyncio.TimeoutError):
                pass
            await asyncio.sleep(.1)
        if ready is None:
            raise RuntimeError("X2 did not become ready within 45 seconds")
        async with client.get("http://127.0.0.1:50052/nala/diagnostics") as response:
            diagnostics = await response.json()
        async with client.ws_connect("http://127.0.0.1:50052/v1/ws", max_msg_size=1048576) as socket:
            await socket.send_json({"type":"start", "session_id":str(uuid.uuid4()), "config":{
                "task_type":"custom_voice", "language":"english", "speaker":"robot_service_v1",
                "input_mode":"token", "group_policy":"none",
                "audio":{"encoding":"pcm_s16le", "sample_rate":24000, "channels":1}}})
            await socket.send_json({"type":"text", "seq_no":1, "text":"Ready."})
            await socket.send_json({"type":"end"})
            while True:
                message = await socket.receive(timeout=30)
                if message.type == WSMsgType.BINARY and message.data:
                    first = time.monotonic()
                    print(json.dumps({"cold_to_ready_ms":round((ready-started)*1000,1),
                                      "cold_to_first_pcm_ms":round((first-started)*1000,1),
                                      "ready_to_first_pcm_ms":round((first-ready)*1000,1),
                                      "model_load_and_prewarm_ms":diagnostics["load_ms"],
                                      "output_device":False}))
                    return
                if message.type in (WSMsgType.ERROR, WSMsgType.CLOSED, WSMsgType.CLOSE):
                    raise RuntimeError("Speech connection ended before PCM")
                if message.type == WSMsgType.TEXT:
                    event=json.loads(message.data).get("event",{})
                    if event.get("type") in ("error", "done"):
                        raise RuntimeError("Speech ended before PCM")

asyncio.run(main())
