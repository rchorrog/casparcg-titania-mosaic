#!/usr/bin/env python3
"""Receptor WebRTC (WHEP) de prueba: se suscribe a un flujo y mide lo que llega DE VERDAD.

No es parte del servidor. Sirve para verificar una salida WebRTC de punta a punta: cuantos
frames de video llegan, de que tamanyo, de que color es el centro de la imagen, y si el audio
suena. Necesita aiortc, aiohttp y numpy (pip install aiortc aiohttp numpy); no es solo biblioteca
estandar. Sin numpy, el analisis de cada frame falla y se informa en "error".

    python3 whep_probe.py http://127.0.0.1:8889/mosaico/whep --seconds 6

Imprime una linea JSON. Codigo de salida 0 si llegaron video y audio.
"""

import argparse
import asyncio
import json
import math
import sys

import aiohttp
from aiortc import RTCPeerConnection, RTCSessionDescription


async def probe(url, seconds):
    pc = RTCPeerConnection()
    pc.addTransceiver("video", direction="recvonly")
    pc.addTransceiver("audio", direction="recvonly")
    res = {"video_frames": 0, "audio_frames": 0, "size": None, "center_rgb": None,
           "audio_peak_dbfs": -100.0, "first_video_s": None, "error": None}
    loop = asyncio.get_event_loop()
    t0 = loop.time()
    done = asyncio.Event()

    async def eat_video(track):
        while not done.is_set():
            try:
                f = await asyncio.wait_for(track.recv(), 2)
            except Exception:
                continue
            res["video_frames"] += 1
            if res["first_video_s"] is None:
                res["first_video_s"] = round(loop.time() - t0, 3)
            if res["video_frames"] % 10 == 0:
                try:
                    img = f.to_ndarray(format="rgb24")
                except Exception as e:  # el analisis no puede cortar el recuento
                    res["error"] = "video analysis: %r" % e
                    continue
                h, w, _ = img.shape
                res["size"] = [w, h]
                c = img[h // 4: 3 * h // 4: 4, w // 4: 3 * w // 4: 4].reshape(-1, 3).mean(axis=0)
                res["center_rgb"] = [round(float(v), 1) for v in c]

    async def eat_audio(track):
        while not done.is_set():
            try:
                f = await asyncio.wait_for(track.recv(), 2)
            except Exception:
                continue
            res["audio_frames"] += 1
            try:
                a = f.to_ndarray().astype("float64")
            except Exception as e:
                res["error"] = "audio analysis: %r" % e
                continue
            full = 32768.0 if f.format.name.startswith("s16") else 1.0
            peak = float(abs(a).max()) / full if a.size else 0.0
            if peak > 0:
                res["audio_peak_dbfs"] = max(res["audio_peak_dbfs"], round(20 * math.log10(peak), 1))

    tasks = []

    @pc.on("track")
    def on_track(track):
        tasks.append(asyncio.ensure_future(eat_video(track) if track.kind == "video" else eat_audio(track)))

    await pc.setLocalDescription(await pc.createOffer())
    async with aiohttp.ClientSession() as s:
        async with s.post(url, data=pc.localDescription.sdp, headers={"Content-Type": "application/sdp"}) as r:
            body = await r.text()
            if r.status not in (200, 201):
                res["error"] = "WHEP %d: %s" % (r.status, body.strip()[:200])
                await pc.close()
                return res
    await pc.setRemoteDescription(RTCSessionDescription(sdp=body, type="answer"))
    await asyncio.sleep(seconds)
    done.set()
    await asyncio.gather(*tasks, return_exceptions=True)
    await pc.close()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--seconds", type=float, default=6.0)
    a = ap.parse_args()
    res = asyncio.run(probe(a.url, a.seconds))
    print(json.dumps(res))
    return 0 if res["video_frames"] > 0 and res["audio_frames"] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
