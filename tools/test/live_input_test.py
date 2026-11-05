#!/usr/bin/env python3
"""Prueba de entradas en directo (SRT/UDP) y de robustez de AMCP contra un servidor real.

Necesita `ffmpeg` en el PATH con libx264, aac y srt (el de Ubuntu 24.04 vale). Arranca el
servidor con su propia configuracion (puerto AMCP 5291) y un emisor ffmpeg local.

Escenarios, cada uno escrito para poder FALLAR con el defecto que vigila:

1. Paquetes corruptos. MPEG-TS por UDP a traves de un relay que, durante 3 s, machaca bytes de
   la carga de los paquetes TS. Despues, el video tiene que volver a avanzar (sync/source-time)
   y el audio tiene que volver a sonar (pico de la capa). Antes del arreglo, un error de
   decodificacion terminaba el hilo del decodificador para siempre.
2. Reconexion SRT. Se mata el emisor y vuelve con otra resolucion y otra frecuencia de muestreo.
   La capa tiene que reconectar (sync/reconnects) y volver a dar imagen y sonido.
2b. Cambio de formato dentro del flujo (UDP, sin reconexion): el emisor vuelve con otra
   resolucion; la capa tiene que seguir dando imagen y sonido.
2c. Salida STREAM: -g, -tune, -vcodec... llegan al codificador (keyframe cada 10 frames con -g 10).
3. Una linea AMCP cuyo CRLF llega partido entre dos lecturas TCP no puede ejecutarse dos veces.
4. INFO en bucle desde varios clientes mientras el canal reproduce: el servidor sigue vivo.

Uso:
    python3 tools/test/live_input_test.py --server build/shell/casparcg [--only corrupt,srt,...]
"""

import argparse
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_test import Amcp, layer_key  # noqa: E402

CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<configuration>
    <paths>
        <media-path>{media}/</media-path>
        <log-path disable="false">{log}/</log-path>
        <data-path>{data}/</data-path>
        <template-path>{template}/</template-path>
    </paths>
    <log-level>info</log-level>
    <channels>
        <channel><video-mode>720p5000</video-mode><consumers/></channel>
    </channels>
    <controllers>
        <tcp><port>{port}</port><protocol>AMCP</protocol></tcp>
    </controllers>
</configuration>
"""

UDP_SEND, UDP_RECV, SRT_PORT = 23000, 23001, 23010
# El seno de lavfi es 1/8 de fondo de escala: unos -18 dBFS de pico. Silencio se publica -100.
AUDIO_ALIVE_DB = -40.0


def sender_cmd(url, size="1280x720", rate=48000):
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re",
            "-f", "lavfi", "-i", "testsrc2=size=%s:rate=50" % size,
            "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=%d" % rate,
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "25",
            "-c:a", "aac", "-b:a", "128k", "-f", "mpegts", url]


class CorruptingRelay(threading.Thread):
    """UDP 127.0.0.1:src -> 127.0.0.1:dst; con `corrupt` activo, machaca la carga de los TS."""

    def __init__(self, src, dst):
        super().__init__(daemon=True)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("127.0.0.1", src))
        self.rx.settimeout(0.2)
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.dst = ("127.0.0.1", dst)
        self.corrupt = False
        self.stop = False
        self.corrupted = 0
        self.rng = random.Random(1234)

    def run(self):
        while not self.stop:
            try:
                data = bytearray(self.rx.recv(65536))
            except socket.timeout:
                continue
            if self.corrupt:
                # Por cada paquete TS de 188 bytes, 1 de cada 3 recibe 24 bytes aleatorios en la
                # carga. El byte de sincronia y la cabecera de 4 bytes se respetan: lo que se
                # quiere es carga corrupta que llegue al decodificador, no un demuxer perdido.
                for off in range(0, len(data) - 187, 188):
                    if self.rng.random() < 0.33:
                        for _ in range(24):
                            data[off + 4 + self.rng.randrange(184)] = self.rng.randrange(256)
                        self.corrupted += 1
            self.tx.sendto(bytes(data), self.dst)


def peak_db(info, layer=10):
    vals = []
    needle = "mixer/layer/layer_%d/audio/peak/" % layer
    for k, v in info.items():
        if needle in k:
            try:
                vals.append(float(v))
            except ValueError:
                pass
    return max(vals) if vals else -100.0


def source_time(info, layer=10):
    v = layer_key(info, layer, "sync/source-time")
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


class Run:
    def __init__(self, a):
        self.a = a
        self.results = []

    def check(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""), flush=True)

    def alive(self, label, layer=10):
        """Mide 2 s: el video avanza y el audio suena."""
        i0 = self.a.info(1)
        t0 = source_time(i0, layer)
        time.sleep(2.0)
        i1 = self.a.info(1)
        t1 = source_time(i1, layer)
        p = max(peak_db(i0, layer), peak_db(i1, layer))
        # Solo el AVANCE: tras un reinicio del emisor sin reconexion, source-time se mide contra
        # el origen anterior y puede ser negativo sin que nada vaya mal.
        moved = t0 is not None and t1 is not None and (t1 - t0) >= 1.5
        self.check("%s: el video avanza" % label, moved, "sync/source-time %s -> %s" % (t0, t1))
        self.check("%s: el audio suena" % label, p > AUDIO_ALIVE_DB, "pico %.1f dBFS" % p)
        return moved and p > AUDIO_ALIVE_DB


def scenario_corrupt(r):
    relay = CorruptingRelay(UDP_SEND, UDP_RECV)
    relay.start()
    snd = subprocess.Popen(sender_cmd("udp://127.0.0.1:%d?pkt_size=1316" % UDP_SEND))
    try:
        time.sleep(1.0)
        r.a.ok('PLAY 1-10 "udp://127.0.0.1:%d"' % UDP_RECV)
        time.sleep(6.0)
        r.alive("UDP antes de la corrupcion")
        relay.corrupt = True
        time.sleep(3.0)
        relay.corrupt = False
        print("[info] paquetes TS corrompidos: %d" % relay.corrupted, flush=True)
        time.sleep(5.0)
        r.alive("UDP despues de la corrupcion")
    finally:
        r.a.cmd("CLEAR 1-10")
        snd.kill()
        snd.wait()
        relay.stop = True


def scenario_srt(r):
    url = "srt://127.0.0.1:%d?mode=listener" % SRT_PORT
    snd = subprocess.Popen(sender_cmd(url))
    try:
        time.sleep(1.0)
        r.a.ok('PLAY 1-10 "srt://127.0.0.1:%d?mode=caller"' % SRT_PORT)
        time.sleep(6.0)
        r.alive("SRT antes de cortar")
        before = layer_key(r.a.info(1), 10, "sync/reconnects")
        snd.kill()
        snd.wait()
        time.sleep(3.0)
        # Vuelve con OTRA resolucion y OTRA frecuencia de muestreo: un decodificador que
        # sobreviviera a la reconexion seguiria configurado para el flujo anterior.
        snd = subprocess.Popen(sender_cmd(url, size="640x360", rate=44100))
        deadline = time.time() + 30
        rec = before
        while time.time() < deadline:
            rec = layer_key(r.a.info(1), 10, "sync/reconnects")
            if rec is not None and before is not None and float(rec) > float(before):
                break
            time.sleep(0.5)
        r.check("SRT: la capa reconecta", rec is not None and before is not None and float(rec) > float(before),
                "sync/reconnects %s -> %s" % (before, rec))
        time.sleep(6.0)
        r.alive("SRT despues de reconectar")
    finally:
        r.a.cmd("CLEAR 1-10")
        snd.kill()
        snd.wait()


def scenario_udp_switch(r):
    """El emisor UDP se reinicia con otra resolucion y otra frecuencia de muestreo.

    UDP no tiene conexion, asi que no hay reconexion: el cambio llega DENTRO del flujo. El grafo
    de filtros, configurado para el formato anterior, rechaza el primer frame nuevo; antes eso
    terminaba el hilo del producer.
    """
    url = "udp://127.0.0.1:%d?pkt_size=1316" % UDP_RECV
    snd = subprocess.Popen(sender_cmd(url))
    try:
        time.sleep(1.0)
        r.a.ok('PLAY 1-10 "udp://127.0.0.1:%d"' % UDP_RECV)
        time.sleep(6.0)
        r.alive("UDP antes del cambio de formato")
        snd.kill()
        snd.wait()
        snd = subprocess.Popen(sender_cmd(url, size="640x360", rate=44100))
        time.sleep(8.0)
        r.alive("UDP despues del cambio de formato (sin reconexion)")
    finally:
        r.a.cmd("CLEAR 1-10")
        snd.kill()
        snd.wait()


def probe_stream_output(r, label, args, workdir):
    """ADD 1 STREAM con `args`, recibido 4 s por un ffmpeg local; devuelve (codec, intervalos GOP)."""
    port = 24000
    out = os.path.join(workdir, "stream_%d.ts" % int(time.time() * 1000))
    rcv = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                            "-i", "udp://127.0.0.1:%d?timeout=15000000" % port, "-t", "4", "-c", "copy", out])
    try:
        time.sleep(1.0)
        code, _, head = r.a.cmd("ADD 1 STREAM udp://127.0.0.1:%d %s" % (port, args))
        if code >= 400:
            r.check("%s: ADD STREAM" % label, False, head)
            return None, []
        rcv.wait(timeout=30)
    finally:
        r.a.cmd("REMOVE 1 STREAM udp://127.0.0.1:%d" % port)
        if rcv.poll() is None:
            rcv.kill()
    if not os.path.exists(out):
        return None, []
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=codec_name:frame=key_frame", "-of", "csv=p=0", out], capture_output=True, text=True)
    lines = [ln.strip().rstrip(",") for ln in p.stdout.splitlines() if ln.strip()]
    codec = next((ln for ln in lines if not ln.isdigit()), None)
    flags = [ln for ln in lines if ln.isdigit()]
    keys = [i for i, ln in enumerate(flags) if ln == "1"]
    return codec, [b - a for a, b in zip(keys, keys[1:])]


def scenario_stream_options(r, workdir):
    """Las opciones de codificador sin sufijo (-g, -tune, -crf...) tienen que llegar al codificador.

    `-g 10` es medible desde fuera: un keyframe cada 10 frames. Sin el arreglo iba al muxer y se
    quedaba en el GOP por defecto de libx264, y `-vcodec` ni siquiera elegia el codec.
    """
    # -an en todas: el canal lleva 16 pistas de audio y el mp2 por defecto de mpegts las rechaza,
    # lo que hacia fallar las tres filas por igual -- tambien el control.
    r.a.ok("PLAY 1-10 #FF808080")
    try:
        for label, args in (
            # CONTROL: la grafia con sufijo, que ya funcionaba. Si esta falla, el fallo es del
            # arnes (el receptor, ffprobe), no del servidor.
            ("STREAM control, todo con sufijo (-codec:v, -g:v)",
             "-codec:v libx264 -tune:v zerolatency -preset:v ultrafast -g:v 10 -an -format mpegts"),
            ("STREAM con la grafia de la documentacion (-vcodec, -format)",
             "-vcodec libx264 -tune zerolatency -preset ultrafast -g 10 -an -format mpegts"),
            ("STREAM con la grafia de la CLI de ffmpeg (-c:v, -f)",
             "-c:v libx264 -tune zerolatency -preset ultrafast -g 10 -an -f mpegts"),
        ):
            codec, gops = probe_stream_output(r, label, args, workdir)
            r.check("%s: codec h264" % label, codec == "h264", "codec %s" % codec)
            r.check("%s: -g 10 llega al codificador" % label, len(gops) >= 3 and all(g == 10 for g in gops),
                    "intervalos entre keyframes %s" % gops[:8])
    finally:
        r.a.cmd("CLEAR 1-10")


def scenario_split_crlf(r, host, port):
    """'VERSION\\r\\nVERSION\\r' y luego '\\n': deben salir exactamente dos respuestas."""
    s = socket.create_connection((host, port), timeout=3)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"VERSION\r\nVERSION\r")
    time.sleep(0.5)
    s.sendall(b"\n")
    time.sleep(1.0)
    s.settimeout(0.5)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    replies = [ln for ln in data.split(b"\r\n") if ln[:3].isdigit()]
    r.check("AMCP: un CRLF partido entre lecturas no ejecuta la linea dos veces", len(replies) == 2,
            "%d respuestas: %r" % (len(replies), replies))


def scenario_info_hammer(r, host, port, proc):
    snd = subprocess.Popen(sender_cmd("udp://127.0.0.1:%d?pkt_size=1316" % UDP_RECV))
    stop = threading.Event()
    counts = [0] * 4
    errors = []

    def worker(n):
        try:
            c = Amcp(host, port)
            while not stop.is_set():
                code, _, _ = c.cmd("INFO 1")
                if code >= 400:
                    errors.append(code)
                counts[n] += 1
        except Exception as e:  # el servidor cayo o cerro la conexion
            errors.append(repr(e))

    try:
        time.sleep(1.0)
        r.a.ok('PLAY 1-10 "udp://127.0.0.1:%d"' % UDP_RECV)
        threads = [threading.Thread(target=worker, args=(n,), daemon=True) for n in range(4)]
        for t in threads:
            t.start()
        time.sleep(20.0)
        stop.set()
        for t in threads:
            t.join(5)
        r.check("INFO en bucle (4 clientes, 20 s): sin errores y el servidor sigue vivo",
                not errors and (proc is None or proc.poll() is None),
                "%d INFO, errores %s" % (sum(counts), errors[:3]))
    finally:
        r.a.cmd("CLEAR 1-10")
        snd.kill()
        snd.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", required=True)
    ap.add_argument("--port", type=int, default=5291)
    ap.add_argument("--workdir")
    ap.add_argument("--only", default="crlf,corrupt,srt,switch,streamopts,info")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        print("[FAIL] ffmpeg no esta en el PATH")
        return 1

    work = os.path.abspath(args.workdir or tempfile.mkdtemp(prefix="live_test_"))
    dirs = {k: os.path.join(work, k) for k in ("media", "log", "data", "template")}
    for d in dirs.values():
        os.makedirs(d, exist_ok=True)
    cfg = os.path.join(work, "casparcg.config")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write(CONFIG.format(port=args.port, **{k: v.replace("\\", "/") for k, v in dirs.items()}))
    server = os.path.abspath(args.server)
    proc = subprocess.Popen([server, cfg], cwd=os.path.dirname(server), stdin=subprocess.PIPE,
                            stdout=open(os.path.join(work, "server-stdout.log"), "wb"), stderr=subprocess.STDOUT)
    a = None
    for _ in range(240):
        if proc.poll() is not None:
            break
        try:
            a = Amcp("127.0.0.1", args.port, timeout=30)
            a.cmd("VERSION")
            break
        except OSError:
            a = None
            time.sleep(0.5)

    r = Run(a)
    only = set(args.only.split(","))
    try:
        if not a:
            raise RuntimeError("el servidor no respondio")
        # Un canal sin consumers no mezcla en 2.5.0 (video_channel.cpp, `has_consumers`), y sin
        # mezcla no hay picos de audio por capa: medido, -100 en todas las pistas. Un consumer
        # permanente y barato hace que el audio sea medible.
        a.ok('ADD 1 REPLAY "live_test_keepalive?segment=2&max_duration=4"')
        if "crlf" in only:
            scenario_split_crlf(r, "127.0.0.1", args.port)
        if "corrupt" in only:
            scenario_corrupt(r)
        if "srt" in only:
            scenario_srt(r)
        if "switch" in only:
            scenario_udp_switch(r)
        if "streamopts" in only:
            scenario_stream_options(r, work)
        if "info" in only:
            scenario_info_hammer(r, "127.0.0.1", args.port, proc)
    except Exception as e:
        traceback.print_exc()
        r.check("la prueba llega hasta el final", False, "%s: %s" % (type(e).__name__, e))

    r.check("el servidor sigue vivo al final", proc.poll() is None, "codigo %s" % proc.poll())
    try:
        if a:
            a.cmd("KILL")
    except Exception:
        pass
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()

    passed = sum(1 for _, ok, _ in r.results if ok)
    print("\n%d/%d comprobaciones pasan (carpeta de trabajo: %s)" % (passed, len(r.results), work), flush=True)
    return 0 if r.results and passed == len(r.results) else 1


if __name__ == "__main__":
    sys.exit(main())
