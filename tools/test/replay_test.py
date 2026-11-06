#!/usr/bin/env python3
"""Prueba funcional del modulo replay contra un servidor CasparCG real.

Solo usa la biblioteca estandar de Python 3.8+, para que corra igual en Windows, en Linux y
dentro del contenedor Docker sin instalar nada.

Arranca el servidor con una configuracion propia (tres canales, puerto AMCP 5290 por defecto
para no pisar un servidor de produccion en 5250), graba, reproduce, captura fotogramas con el
consumer IMAGE y comprueba lo que se ve, no solo lo que el servidor dice.

Uso:
    python3 tools/test/replay_test.py --server <ruta a casparcg o casparcg.exe>
    python3 tools/test/replay_test.py --host 127.0.0.1 --port 5250 --media /ruta/media  # servidor ya arrancado

Codigo de salida 0 solo si todas las comprobaciones pasan. Una comprobacion que no llega a
ejecutarse cuenta como fallo, nunca como "saltada".
"""

import argparse
import glob
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import xml.etree.ElementTree as ET
import zlib

# Dos colores con R y B intercambiados: un fallo de orden de canales los confunde, y una
# busqueda que no busca nada devuelve el mismo color en los dos extremos.
COLOR_A = (192, 128, 64)
COLOR_B = (64, 128, 192)
GREY = (128, 128, 128)

# Perdida medida de VMX SQ sobre un color plano: (193,128,66) desde (192,128,64).
TOL_8BIT = 6
# El camino de 10 bits (vmx-p216) es casi sin perdida sobre un color plano: medido 0 LSB en
# gris; sin el reescalado de replay_codec.cpp para el shader de 2.5.0, 2 LSB de dominante.
TOL_10BIT = 1

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
        <channel><video-mode>720p5000</video-mode><consumers/></channel>
        <channel><video-mode>720p5000</video-mode><color-depth>16</color-depth><consumers/></channel>
        <channel><video-mode>1080i5000</video-mode><consumers/></channel>
    </channels>
    <controllers>
        <tcp><port>{port}</port><protocol>AMCP</protocol></tcp>
    </controllers>
</configuration>
"""


# ── AMCP ─────────────────────────────────────────────────────────────────────────────────────


class Amcp:
    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def _line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("el servidor cerro la conexion")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode("utf-8", "replace")

    def cmd(self, text):
        """Devuelve (codigo, [lineas de datos])."""
        self.last = text
        self.sock.sendall((text + "\r\n").encode("utf-8"))
        head = self._line()
        try:
            code = int(head.split(" ", 1)[0])
        except ValueError:
            raise RuntimeError("respuesta inesperada a %r: %r" % (text, head))
        data = []
        if code == 201:
            data.append(self._line())
        elif code == 200:
            while True:
                line = self._line()
                if line == "":
                    break
                data.append(line)
        return code, data, head

    def ok(self, text):
        code, data, head = self.cmd(text)
        if code >= 400:
            raise RuntimeError("%s -> %s" % (text, head))
        return data

    def info(self, channel):
        """INFO <canal> aplanado a {ruta/con/barras: texto}."""
        code, data, head = self.cmd("INFO %d" % channel)
        if code != 201 and code != 200:
            raise RuntimeError("INFO %d -> %s" % (channel, head))
        # El XML usa '\n' por dentro y termina en '\r\n', asi que llega como una sola "linea".
        # El parche de pico de audio por capa publica audio/peak/0, 1...; INFO solo renombra los
        # segmentos numericos intermedios, asi que el ultimo sale como <0>, que no es XML valido.
        text = re.sub(r"<(/?)(\d)", r"<\1_\2", "\n".join(data))
        root = ET.fromstring(text)
        out = {}

        def walk(node, path):
            kids = list(node)
            if not kids:
                out[path] = (node.text or "").strip()
            for k in kids:
                walk(k, path + "/" + k.tag if path else k.tag)

        walk(root, "")
        return out


def layer_key(info, layer, leaf):
    for k, v in info.items():
        if ("/layer_%d/" % layer) in k and k.endswith("foreground/" + leaf):
            return v
    return None


def port_key(info, leaf, name=None):
    """Valor `leaf` del consumer cuyo file/path termina en `name` (o del primero, sin nombre)."""
    ports = {}
    for k, v in info.items():
        if "/port/" in k:
            head, _, rest = k.partition("/port/")
            port, _, sub = rest.partition("/")
            ports.setdefault(port, {})[sub] = v
    for port, keys in sorted(ports.items()):
        path = keys.get("file/path", "").replace("\\", "/").rstrip("/")
        if name is None or path.endswith("/" + name):
            if leaf in keys:
                return keys[leaf]
    return None


# ── PNG (solo lo que escribe el consumer IMAGE: 8 bits, RGB o RGBA, sin entrelazado) ────────


def read_png(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s no es un PNG" % path)
    pos, idat, w = 8, b"", None
    while pos < len(blob):
        n, kind = struct.unpack(">I4s", blob[pos : pos + 8])
        body = blob[pos + 8 : pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError("PNG no soportado: depth=%d tipo=%d" % (depth, ctype))
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def mean_rgb(path):
    """Media de un recuadro central (evita bordes y el posible escalado)."""
    w, h, bpp, rows = read_png(path)
    x0, x1, y0, y1 = w // 4, 3 * w // 4, h // 4, 3 * h // 4
    acc = [0, 0, 0]
    n = 0
    for y in range(y0, y1, 4):
        r = rows[y]
        for x in range(x0, x1, 4):
            i = x * bpp
            acc[0] += r[i]
            acc[1] += r[i + 1]
            acc[2] += r[i + 2]
            n += 1
    return tuple(a / n for a in acc)


# ── Prueba ───────────────────────────────────────────────────────────────────────────────────


class Run:
    def __init__(self, amcp, media):
        self.a = amcp
        self.media = media
        self.results = []

    def check(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""), flush=True)
        return ok

    def capture(self, channel, name):
        path = os.path.join(self.media, name + ".png")
        if os.path.exists(path):
            os.remove(path)
        self.a.ok('ADD %d IMAGE "%s"' % (channel, name))
        deadline = time.time() + 10
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(0.3)  # el consumer puede estar terminando de escribir
                try:
                    return mean_rgb(path)
                except Exception:
                    pass
            time.sleep(0.1)
        raise RuntimeError("el consumer IMAGE no escribio %s" % path)

    def shot(self, channel, name):
        """Como capture(), pero devuelve la ruta del PNG (para leer la marca)."""
        path = os.path.join(self.media, name + ".png")
        if os.path.exists(path):
            os.remove(path)
        self.a.ok('ADD %d IMAGE "%s"' % (channel, name))
        deadline = time.time() + 10
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(0.3)
                try:
                    read_png(path)
                    return path
                except Exception:
                    pass
            time.sleep(0.1)
        raise RuntimeError("el consumer IMAGE no escribio %s" % path)

    def wait_layer(self, channel, layer, leaf, pred, timeout=10.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = layer_key(self.a.info(channel), layer, leaf)
            if last is not None and pred(last):
                return last
            time.sleep(0.1)
        return last


def dist(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def fmt(c):
    return "(%s)" % ", ".join("%.1f" % v for v in c)


def color_cmd(c):
    return "#FF%02X%02X%02X" % c


def segments(media, name):
    return sorted(glob.glob(os.path.join(media, name, name + ".*.rseg")))


# ── Clip de marcas: cada frame lleva su numero en binario ───────────────────────────────────
#
# Nueve barras verticales, blanca (Y=235) o negra (Y=16): el frame k pinta el bit i en la barra
# i. 512 valores distintos, legibles tras VMX, tras el escalado a cualquier canal y sin depender
# de ninguna matriz de color (solo luma). Se escribe en Y4M desde Python para no depender de un
# ffmpeg en la maquina que prepara la prueba; el servidor lo lee con su propio FFmpeg.
MARK_BITS = 9
MARK_FRAMES = 1 << MARK_BITS
MARK_BAR = 32
MARK_H = 16


def write_marker_clip(path, fps=50):
    w = MARK_BAR * MARK_BITS
    chroma = bytes([128]) * (w * MARK_H)
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F%d:1 Ip A1:1 C444\n" % (w, MARK_H, fps))
        for k in range(MARK_FRAMES):
            row = b"".join(bytes([235 if (k >> i) & 1 else 16]) * MARK_BAR for i in range(MARK_BITS))
            f.write(b"FRAME\n")
            f.write(row * MARK_H)
            f.write(chroma)
            f.write(chroma)


def bars_to_marker(values):
    """Valores de las nueve barras (0-255) -> numero, o None si alguna barra es ambigua (mezcla
    de dos frames, o una imagen que no es el clip)."""
    k = 0
    for i, v in enumerate(values):
        if v > 170:
            k |= 1 << i
        elif v >= 85:
            return None
    return k


def read_marker(path):
    w, h, bpp, rows = read_png(path)
    row = rows[h // 2]
    return bars_to_marker([row[int((i + 0.5) * w / MARK_BITS) * bpp] for i in range(MARK_BITS)])


def file_markers(ffmpeg, path, width, height):
    """Marca de cada frame de una grabacion del consumer FILE: la fila central reducida a nueve
    pixeles (uno por barra), en gris."""
    p = subprocess.run([ffmpeg, "-v", "error", "-i", path, "-vf",
                        "crop=%d:2:0:%d,scale=%d:1:flags=area,format=gray" % (width, height // 2 - 1, MARK_BITS),
                        "-f", "rawvideo", "-"], capture_output=True)
    raw = p.stdout
    return [bars_to_marker(raw[i : i + MARK_BITS]) for i in range(0, len(raw) - MARK_BITS + 1, MARK_BITS)]


def steps_of(markers):
    """Pasos entre frames consecutivos, en registros (modulo el clip), ignorando frames ilegibles."""
    out = []
    prev = None
    for m in markers:
        if m is None:
            prev = None
            continue
        if prev is not None:
            d = (m - prev) % MARK_FRAMES
            out.append(d - MARK_FRAMES if d > MARK_FRAMES // 2 else d)
        prev = m
    return out


def slope(a, channel, layer, secs=2.0, leaf="file/frame"):
    """Registros por segundo de `leaf`, por minimos cuadrados sobre muestras de INFO. file/frame
    cuenta desde el registro mas antiguo que queda: con la retencion borrando, usar
    replay/frame_number, que es absoluto."""
    pts = []
    end = time.time() + secs
    while time.time() < end:
        f = layer_key(a.info(channel), layer, leaf)
        if f is not None:
            pts.append((time.time(), float(f)))
        time.sleep(0.1)
    if len(pts) < 5:
        return None
    n = len(pts)
    mt = sum(t for t, _ in pts) / n
    mf = sum(f for _, f in pts) / n
    den = sum((t - mt) ** 2 for t, _ in pts)
    return sum((t - mt) * (f - mf) for t, f in pts) / den if den else None


def run_tests(r, ffprobe):
    a = r.a
    media = r.media

    # Canal 1: fuente de 8 bits. Canal 3: fuente de 16 bits. Canal 2: reproduccion (8 bits).
    #
    # Un canal SIN consumers no mezcla en 2.5.0 (video_channel.cpp, `has_consumers`), y el
    # consumer IMAGE es de un solo frame: sin un consumer permanente, cada captura recibe el frame
    # que quedo en la tuberia del mezclador la ultima vez que hubo uno -- la imagen de la busqueda
    # ANTERIOR. Medido: sin esto, cada captura en pausa mostraba el color del comando previo.
    a.ok('ADD 1 REPLAY "keep1?segment=2&max_duration=4"')
    a.ok('ADD 2 REPLAY "keep2?segment=2&max_duration=4"')
    a.ok("PLAY 1-10 %s" % color_cmd(COLOR_A))
    a.ok("PLAY 3-10 %s" % color_cmd(GREY))
    time.sleep(1.0)

    # ── 1. Grabacion de 8 bits, dos colores en el aire, segmentos de 2 s y retencion de 6 s ──
    a.ok('ADD 1 REPLAY "t8?segment=2&max_duration=6"')
    t0 = time.time()
    time.sleep(3.0)
    a.ok("PLAY 1-10 %s" % color_cmd(COLOR_B))
    time.sleep(2.0)

    man = os.path.join(media, "t8", "t8.rman.json")
    r.check("la grabacion crea su manifiesto", os.path.exists(man), man)
    try:
        codec = json.load(open(man)).get("codec") if os.path.exists(man) else None
    except Exception as e:
        codec = "ilegible: %s" % e
    r.check("canal de 8 bits graba vmx-bgra8", codec == "vmx-bgra8", "codec=%s" % codec)
    segs = segments(media, "t8")
    r.check("la grabacion escribe segmentos .rseg", len(segs) >= 2, "%d segmentos" % len(segs))

    info1 = a.info(1)
    r.check("el consumer publica replay/state=recording", port_key(info1, "replay/state", "t8") == "recording",
            "replay/state=%s" % port_key(info1, "replay/state", "t8"))

    # ── 2. Reproduccion de una grabacion que sigue creciendo: una busqueda mueve la imagen ──
    a.ok('PLAY 2-10 "t8" SEEK 0 SPEED 0')
    length = r.wait_layer(2, 10, "file/length", lambda v: float(v) > 100)
    r.check("la grabacion en curso se puede reproducir", length is not None and float(length) > 100,
            "file/length=%s" % length)
    time.sleep(0.5)
    cap_a = r.capture(2, "cap_a")
    r.check("SEEK 0 muestra el primer color grabado", dist(cap_a, COLOR_A) <= TOL_8BIT,
            "%s frente a %s" % (fmt(cap_a), COLOR_A))

    n = int(float(layer_key(a.info(2), 10, "file/length")))
    code, _, head = a.cmd("CALL 2-10 SEEK %d" % (n - 5))
    r.check("CALL SEEK responde OK", code < 400, head)
    time.sleep(0.5)
    frame = layer_key(a.info(2), 10, "file/frame")
    r.check("file/frame refleja la busqueda", frame is not None and int(float(frame)) == n - 5,
            "file/frame=%s, esperado %d" % (frame, n - 5))
    cap_b = r.capture(2, "cap_b")
    r.check("SEEK al final muestra el segundo color", dist(cap_b, COLOR_B) <= TOL_8BIT,
            "%s frente a %s" % (fmt(cap_b), COLOR_B))

    # ── 3. Rechazos: un valor que no se entiende nunca contesta OK ──
    for verb in ("BANANA", "SPEED banana", "SEEK banana"):
        code, _, head = a.cmd("CALL 2-10 %s" % verb)
        r.check("CALL %s se rechaza (4xx)" % verb, 400 <= code < 500, head)
    code, _, head = a.cmd('PLAY 2-11 "t8" SPEED x')
    r.check("PLAY ... SPEED x se rechaza", code >= 400, head)

    # ── 4. LIVE: sigue la cabeza de escritura a distancia fija ──
    a.ok('PLAY 2-10 "t8" LIVE 3')
    time.sleep(1.0)
    gaps = []
    for _ in range(6):
        i = a.info(2)
        ln = layer_key(i, 10, "file/length")
        fr = layer_key(i, 10, "file/frame")
        if ln is not None and fr is not None:
            gaps.append(int(float(ln)) - 1 - int(float(fr)))
        time.sleep(0.4)
    # Muestreado por INFO, no en el mismo tick que el escritor: se admite +-1.
    r.check("LIVE 3 se mantiene a 3 registros de la cabeza", len(gaps) >= 4 and all(2 <= g <= 4 for g in gaps),
            "distancias %s" % gaps)
    live = layer_key(a.info(2), 10, "replay/live")
    r.check("replay/live se publica en modo LIVE", live in ("true", "1"), "replay/live=%s" % live)

    # ── 5. EXPORT a ProRes + PCM ──
    a.ok('PLAY 2-10 "t8" SEEK 0 SPEED 0')
    time.sleep(0.5)
    out = os.path.join(media, "t8_export.mov")
    code, data, head = a.cmd('CALL 2-10 EXPORT "t8_export.mov" 0 250')
    r.check("CALL EXPORT responde OK", code < 400, "%s %s" % (head, data))
    # Mientras corre, un segundo EXPORT se rechaza (antes: "201 CALL OK" con el texto
    # "400 EXPORT BUSY" dentro, que un cliente lee como exito).
    code2, _, head2 = a.cmd('CALL 2-10 EXPORT "t8_busy.mov" 0 50')
    busy_state = layer_key(a.info(2), 10, "export/state")
    # 250 registros de ProRes tardan segundos; el segundo EXPORT llega milisegundos despues.
    r.check("un segundo EXPORT mientras corre se rechaza (4xx)", 400 <= code2 < 500,
            "%s (export/state=%s)" % (head2, busy_state))
    state = r.wait_layer(2, 10, "export/state", lambda v: v in ("done", "failed"), timeout=60)
    err = layer_key(a.info(2), 10, "export/error")
    r.check("EXPORT termina en done", state == "done", "export/state=%s error=%s" % (state, err))
    r.check("EXPORT escribe el fichero", os.path.exists(out) and os.path.getsize(out) > 0, out)
    if ffprobe and os.path.exists(out):
        p = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-show_entries",
                            "stream=codec_name,nb_read_frames,r_frame_rate", "-of", "json", out],
                           capture_output=True, text=True)
        try:
            streams = json.loads(p.stdout)["streams"]
        except Exception:
            streams = []
        v = [s for s in streams if s.get("codec_name") == "prores"]
        au = [s for s in streams if s.get("codec_name", "").startswith("pcm_")]
        r.check("EXPORT: pista ProRes con los 250 registros",
                bool(v) and int(v[0].get("nb_read_frames", 0)) == 250 and v[0].get("r_frame_rate") == "50/1",
                json.dumps(v))
        r.check("EXPORT: pista de audio PCM", bool(au), json.dumps(au))
    else:
        print("[info] ffprobe no disponible: no se inspecciona el contenido del export", flush=True)

    # ── 6. Retencion: borra por delante, y la linea de tiempo no supera lo que hay en disco ──
    wait = 16.0 - (time.time() - t0)
    if wait > 0:
        time.sleep(wait)
    counts = []
    for _ in range(5):
        counts.append(len(segments(media, "t8")))
        time.sleep(1.0)
    i1 = a.info(1)
    written = port_key(i1, "replay/frames_written", "t8")
    # 6 s de retencion + un segmento de 2 s en curso + uno de margen: como mucho 5 segmentos.
    r.check("la retencion borra segmentos antiguos", max(counts) <= 5 and written and int(float(written)) > 5 * 100,
            "segmentos %s, frames_written=%s" % (counts, written))
    a.ok('PLAY 2-10 "t8" SEEK 0 SPEED 0')
    time.sleep(1.0)
    ln = layer_key(a.info(2), 10, "file/length")
    # Lo que puede haber en disco: (retencion + 2 segmentos) * 50 fps.
    r.check("la linea de tiempo no supera lo que hay en disco", ln is not None and float(ln) <= (6 + 4) * 50,
            "file/length=%s, maximo %d" % (ln, (6 + 4) * 50))

    # ── 7. Canal de 16 bits: graba vmx-p216 y se reproduce sin dominante de color ──
    a.ok('ADD 3 REPLAY "t16?segment=2&max_duration=6"')
    time.sleep(2.5)
    man16 = os.path.join(media, "t16", "t16.rman.json")
    try:
        codec16 = json.load(open(man16)).get("codec") if os.path.exists(man16) else None
    except Exception as e:
        codec16 = "ilegible: %s" % e
    r.check("canal de 16 bits graba vmx-p216", codec16 == "vmx-p216", "codec=%s" % codec16)
    a.ok("PLAY 1-10 %s" % color_cmd(GREY))
    time.sleep(0.5)
    ref = r.capture(1, "cap_ref_grey")
    a.ok('PLAY 2-10 "t16" SEEK 10 SPEED 0')
    time.sleep(1.0)
    g = r.capture(2, "cap_t16_grey")
    spread = max(g) - min(g)
    r.check("10 bits: un gris se reproduce como el mismo gris", dist(g, ref) <= TOL_10BIT,
            "%s frente a la fuente %s" % (fmt(g), fmt(ref)))
    r.check("10 bits: un gris sigue siendo neutro (sin dominante)", spread <= 1.0,
            "diferencia entre canales %.2f" % spread)
    a.ok('REMOVE 3 REPLAY "t16"')

    # ── 8. Dos grabaciones con nombre distinto conviven; REMOVE quita solo la suya ──
    a.ok('ADD 1 REPLAY "t8b?segment=2&max_duration=6"')
    time.sleep(1.5)
    a.ok('REMOVE 1 REPLAY "t8b"')
    gone = "?"
    deadline = time.time() + 5
    while time.time() < deadline:  # el consumer sale del estado en el siguiente tick, no al responder
        gone = port_key(a.info(1), "replay/state", "t8b")
        if gone is None:
            break
        time.sleep(0.1)
    w1 = port_key(a.info(1), "replay/frames_written", "t8")
    time.sleep(1.0)
    w2 = port_key(a.info(1), "replay/frames_written", "t8")
    r.check("REMOVE quita la grabacion que nombra", gone is None, "t8b replay/state=%s" % gone)
    r.check("REMOVE de otra grabacion deja la primera grabando",
            w1 is not None and w2 is not None and float(w2) > float(w1), "frames_written %s -> %s" % (w1, w2))
    a.ok('REMOVE 1 REPLAY "t8"')
    a.ok('REMOVE 1 REPLAY "keep1"')
    a.ok('REMOVE 2 REPLAY "keep2"')


def run_transport_tests(r, ffmpeg):
    """Transporte leido de la IMAGEN: seek exacto, velocidades frame a frame, IN/OUT/LOOP, los
    extremos sin LOOP, un canal entrelazado, la retencion bajo un lector y la regrabacion con el
    mismo nombre. Cada frame grabado lleva su numero (clip de marcas), asi que el registro que se
    ve se lee de la imagen, no de lo que el servidor publica."""
    a = r.a
    media = r.media
    if not ffmpeg:
        # Sin ffmpeg no se pueden leer las grabaciones del consumer FILE: es un fallo, no un salto.
        r.check("transporte: ffmpeg disponible para leer las grabaciones frame a frame", False,
                "instala ffmpeg o pasa --ffmpeg")
        return
    write_marker_clip(os.path.join(media, "marks.y4m"))

    # Consumers permanentes: sin ellos un canal no mezcla en 2.5.0 y las capturas llegan viejas.
    a.ok('ADD 1 REPLAY "keep1t?segment=2&max_duration=4"')
    a.ok('ADD 2 REPLAY "keep2t?segment=2&max_duration=4"')
    a.ok('ADD 4 REPLAY "keep4t?segment=2&max_duration=4"')

    # ── 9. Grabacion de un clip con marcas: ~7 s, menos de 512 registros (sin ambiguedad) ──
    a.ok('PLAY 1-10 "marks.y4m" LOOP')
    time.sleep(1.0)
    s1 = read_marker(r.shot(1, "src_a"))
    s2 = read_marker(r.shot(1, "src_b"))
    r.check("el clip de marcas se reproduce en el canal fuente (marca legible y avanzando)",
            s1 is not None and s2 is not None and s1 != s2, "marcas %s, %s" % (s1, s2))
    a.ok('ADD 1 REPLAY "tm?segment=2&max_duration=60"')
    time.sleep(7.0)
    a.ok('REMOVE 1 REPLAY "tm"')
    time.sleep(1.0)

    a.ok('PLAY 2-10 "tm" SPEED 0')
    time.sleep(0.8)
    n = int(float(layer_key(a.info(2), 10, "file/length")))
    m0 = read_marker(r.shot(2, "tm_play0"))
    r.check("PLAY ... SPEED 0 sin SEEK muestra el registro 0 (marca legible)", m0 is not None and 300 <= n < 512,
            "marca=%s, file/length=%d" % (m0, n))
    if m0 is None:
        return

    def rec_of(m):
        return None if m is None else (m - m0) % MARK_FRAMES

    # Seek exacto en pausa, los dos extremos incluidos: el registro N lleva la marca m0+N. Esto
    # comprueba a la vez que la grabacion no salto ni repitio frames (un hueco desplaza todo lo
    # que viene detras) y que SEEK cae en el registro pedido.
    got = []
    for k in (1, 2, 137, n // 2, n - 2, n - 1):
        a.ok("CALL 2-10 SEEK %d" % k)
        time.sleep(0.5)
        got.append((k, rec_of(read_marker(r.shot(2, "tm_seek_%d" % k)))))
    r.check("SEEK N en pausa muestra el registro N, extremos incluidos (grabacion sin huecos)",
            all(k == v for k, v in got), "pedido -> mostrado: %s" % got)

    # ── 10. Velocidades, frame a frame: se graba la salida y se lee la marca de cada frame ──
    def run_at(speed, start, tag, secs=2.5):
        a.ok("CALL 2-10 SPEED 0")
        a.ok("CALL 2-10 SEEK %d" % start)
        time.sleep(0.4)
        out = os.path.join(media, tag + ".mkv")
        if os.path.exists(out):
            os.remove(out)
        # Sin audio: solo se leen las marcas, y con FFmpeg 8 el audio por defecto del contenedor
        # rechaza la disposicion de 16 canales del canal.
        a.ok('ADD 2 FILE "%s.mkv" -codec:v libx264 -preset:v ultrafast -crf:v 10 -an' % tag)
        time.sleep(0.3)
        a.ok("CALL 2-10 SPEED %s" % speed)
        rate = slope(a, 2, 10, secs)
        a.ok('REMOVE 2 FILE "%s.mkv"' % tag)
        time.sleep(1.5)
        steps = steps_of(file_markers(ffmpeg, out, 1280, 720))
        return rate, steps

    for speed, start, want_steps, want_rate in (
            ("1", 30, {1}, 50.0),
            ("2", 30, {2}, 100.0),
            ("-1", n - 40, {-1}, -50.0),
            ("0.5", 30, {0, 1}, 25.0)):
        rate, steps = run_at(speed, start, "sp_%s" % speed.replace("-", "m").replace(".", "_"))
        # Fuera la pausa de antes y de despues (pasos 0 en los extremos); dentro, un 0 a velocidad 1
        # es un frame repetido y cuenta como fallo.
        while steps and steps[0] == 0:
            steps.pop(0)
        while steps and steps[-1] == 0:
            steps.pop()
        core = steps[5:-5] if len(steps) > 20 else steps
        exact = bool(core) and all(s in want_steps for s in core)
        if speed == "0.5":
            ones = sum(1 for s in core if s == 1)
            exact = exact and bool(core) and abs(ones / len(core) - 0.5) <= 0.1
        r.check("SPEED %s: cada frame avanza %s registro(s), leido de la imagen" % (speed, sorted(want_steps)),
                exact, "%d pasos, distintos: %s" % (len(core), sorted(set(core))[:8]))
        r.check("SPEED %s: file/frame avanza a %.0f registros/s" % (speed, want_rate),
                rate is not None and abs(rate - want_rate) <= abs(want_rate) * 0.1,
                "medido %s" % (None if rate is None else "%.1f" % rate))

    # ── 11. IN/OUT con LOOP: se queda en el rango y da la vuelta ──
    a.ok('PLAY 2-10 "tm" LOOP IN 100 OUT 150 SEEK 100 SPEED 1')
    frames = []
    end = time.time() + 3.0
    while time.time() < end:
        f = layer_key(a.info(2), 10, "file/frame")
        if f is not None:
            frames.append(int(float(f)))
        time.sleep(0.05)
    wraps = sum(1 for x, y in zip(frames, frames[1:]) if y < x)
    r.check("LOOP IN 100 OUT 150: file/frame no sale de [100, 150) y da la vuelta",
            frames and min(frames) >= 100 and max(frames) <= 149 and wraps >= 2,
            "min %s max %s, vueltas %d" % (min(frames or [0]), max(frames or [0]), wraps))
    m = rec_of(read_marker(r.shot(2, "tm_loop")))
    r.check("LOOP: la imagen es un registro del rango", m is not None and 100 <= m <= 149, "registro %s" % m)

    # ── 12. Sin LOOP: hacia delante se para en OUT-1, hacia atras en IN, sin negro ──
    a.ok('PLAY 2-10 "tm" IN 100 OUT 150 SEEK 140 SPEED 1')
    time.sleep(1.5)
    i2 = a.info(2)
    fr, at_end = layer_key(i2, 10, "file/frame"), layer_key(i2, 10, "replay/at_end")
    m1 = rec_of(read_marker(r.shot(2, "tm_end_a")))
    time.sleep(0.5)
    m2 = rec_of(read_marker(r.shot(2, "tm_end_b")))
    r.check("sin LOOP se para en OUT-1 (registro 149) y lo mantiene en pantalla",
            fr is not None and int(float(fr)) == 149 and m1 == 149 and m2 == 149,
            "file/frame=%s, imagen %s y %s" % (fr, m1, m2))
    r.check("replay/at_end se publica al llegar al final", at_end in ("true", "1"), "replay/at_end=%s" % at_end)

    a.ok('PLAY 2-10 "tm" IN 100 OUT 150 SEEK 110 SPEED -1')
    time.sleep(1.0)
    fr = layer_key(a.info(2), 10, "file/frame")
    m = rec_of(read_marker(r.shot(2, "tm_rev_end")))
    r.check("hacia atras sin LOOP se para en IN (registro 100)",
            fr is not None and int(float(fr)) == 100 and m == 100, "file/frame=%s, imagen %s" % (fr, m))

    # ── 13. Avance rapido dentro de una grabacion que crece: vuelve a velocidad 1 al alcanzarla ──
    a.ok('ADD 1 REPLAY "tg?segment=2&max_duration=60"')
    time.sleep(3.0)
    a.ok('PLAY 2-10 "tg" SEEK 0 SPEED 4')
    time.sleep(3.0)
    rate = slope(a, 2, 10, 2.0)
    i2 = a.info(2)
    ln, fr = layer_key(i2, 10, "file/length"), layer_key(i2, 10, "file/frame")
    gap = None if ln is None or fr is None else int(float(ln)) - 1 - int(float(fr))
    r.check("SPEED 4 al alcanzar el final de una grabacion en curso sigue a velocidad 1, pegado a ella",
            rate is not None and abs(rate - 50) <= 5 and gap is not None and 0 <= gap <= 5,
            "ritmo %s registros/s, distancia a la cabeza %s" % (None if rate is None else "%.1f" % rate, gap))
    a.ok('REMOVE 1 REPLAY "tg"')

    # ── 14. Canal entrelazado (1080i50): un registro por campo, y se reproduce limpio ──
    a.ok('PLAY 4-10 "marks.y4m" LOOP')
    time.sleep(1.0)
    a.ok('ADD 4 REPLAY "ti?segment=2&max_duration=60"')
    time.sleep(3.0)
    w1 = port_key(a.info(4), "replay/frames_written", "ti")
    t1 = time.time()
    time.sleep(3.0)
    w2 = port_key(a.info(4), "replay/frames_written", "ti")
    per_s = None if w1 is None or w2 is None else (float(w2) - float(w1)) / (time.time() - t1)
    r.check("1080i50 graba un registro por campo, ~50/s (como en CasparVP)",
            per_s is not None and 45 <= per_s <= 55, "%s registros/s" % (None if per_s is None else "%.1f" % per_s))
    a.ok('REMOVE 4 REPLAY "ti"')
    time.sleep(1.0)
    a.ok('PLAY 4-20 "ti" SEEK 20 SPEED 0')
    time.sleep(1.0)
    mi = read_marker(r.shot(4, "ti_paused"))
    r.check("1080i50: un registro en pausa se ve limpio (marca legible, sin mezcla de dos frames)", mi is not None,
            "marca %s" % mi)
    a.ok("CALL 4-20 SPEED 1")
    rate = slope(a, 4, 20, 2.0)
    r.check("1080i50: SPEED 1 reproduce ~50 registros/s, uno por campo",
            rate is not None and abs(rate - 50) <= 5, "medido %s" % (None if rate is None else "%.1f" % rate))
    a.ok("STOP 4-20")
    a.ok("STOP 4-10")

    # ── 15. Retencion bajo un lector parado en el registro 0 ──
    a.ok('ADD 1 REPLAY "tr?segment=1&max_duration=3"')
    time.sleep(2.5)
    a.ok('PLAY 2-10 "tr" SEEK 0 SPEED 0')
    time.sleep(7.0)  # el segmento del registro 0 se borra mientras el lector lo tiene
    alive = True
    try:
        a.cmd("VERSION")
    except Exception:
        alive = False
    a.ok("CALL 2-10 SPEED 1")
    rate = slope(a, 2, 10, 2.0, leaf="replay/frame_number") if alive else None
    mr = read_marker(r.shot(2, "tr_after")) if alive else None
    r.check("la retencion borra bajo un lector parado y la reproduccion sigue (servidor vivo, imagen legible)",
            alive and rate is not None and rate > 40 and mr is not None,
            "vivo=%s, ritmo %s, marca %s" % (alive, None if rate is None else "%.1f" % rate, mr))
    a.ok('REMOVE 1 REPLAY "tr"')

    # ── 16. Regrabar con un nombre existente no toca la grabacion anterior ──
    before = [(p, os.path.getsize(p)) for p in segments(media, "tm")]
    a.ok('ADD 1 REPLAY "tm?segment=2&max_duration=60"')
    time.sleep(2.5)
    a.ok('REMOVE 1 REPLAY "tm"')
    time.sleep(1.0)
    after = [(p, os.path.getsize(p)) for p in segments(media, "tm")]
    renamed = [d for d in glob.glob(os.path.join(media, "tm_*")) if os.path.isdir(d)]
    r.check("regrabar con el mismo nombre deja la primera intacta y graba la segunda aparte",
            before and before == after and len(renamed) == 1,
            "%d segmentos antes y %d despues, carpetas nuevas %s" % (len(before), len(after),
                                                                     [os.path.basename(d) for d in renamed]))

    a.ok("STOP 2-10")
    a.ok("STOP 1-10")
    a.ok('REMOVE 1 REPLAY "keep1t"')
    a.ok('REMOVE 2 REPLAY "keep2t"')
    a.ok('REMOVE 4 REPLAY "keep4t"')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", help="ejecutable de CasparCG a arrancar (si se omite, se usa uno ya arrancado)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5290)
    ap.add_argument("--media", help="carpeta de media del servidor ya arrancado")
    ap.add_argument("--workdir", help="carpeta para config, media y logs (por defecto, temporal)")
    ap.add_argument("--keep", action="store_true", help="no borrar la carpeta de trabajo")
    ap.add_argument("--ffprobe", default=shutil.which("ffprobe"))
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    args = ap.parse_args()

    proc = None
    work = None
    if args.server:
        work = os.path.abspath(args.workdir or tempfile.mkdtemp(prefix="replay_test_"))
        dirs = {k: os.path.join(work, k) for k in ("media", "log", "data", "template")}
        for d in dirs.values():
            os.makedirs(d, exist_ok=True)
        cfg = os.path.join(work, "casparcg.config")
        with open(cfg, "w", encoding="utf-8") as f:
            f.write(CONFIG.format(port=args.port, **{k: v.replace("\\", "/") for k, v in dirs.items()}))
        server = os.path.abspath(args.server)
        log = open(os.path.join(work, "server-stdout.log"), "wb")
        proc = subprocess.Popen([server, cfg], cwd=os.path.dirname(server), stdin=subprocess.PIPE,
                                stdout=log, stderr=subprocess.STDOUT)
        media = dirs["media"]
        print("[info] servidor %s, carpeta de trabajo %s" % (server, work), flush=True)
    else:
        if not args.media:
            ap.error("--media es obligatorio sin --server")
        media = os.path.abspath(args.media)

    amcp = None
    deadline = time.time() + 120
    while time.time() < deadline:
        if proc and proc.poll() is not None:
            break
        try:
            amcp = Amcp(args.host, args.port)
            amcp.cmd("VERSION")
            break
        except OSError:
            amcp = None
            time.sleep(0.5)

    r = Run(amcp, media) if amcp else None
    failed_to_run = None
    if not amcp:
        failed_to_run = "el servidor no respondio en %s:%d" % (args.host, args.port)
    else:
        try:
            run_tests(r, args.ffprobe)
            run_transport_tests(r, args.ffmpeg)
        except Exception as e:  # una excepcion es un fallo, nunca una prueba saltada
            traceback.print_exc()
            failed_to_run = "%s: %s (ultimo comando: %s)" % (type(e).__name__, e, getattr(amcp, "last", "?"))

    if proc:
        try:
            if amcp:
                amcp.cmd("KILL")
        except Exception:
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()

    results = r.results if r else []
    if failed_to_run:
        results.append(("la prueba llega hasta el final", False, failed_to_run))
        print("[FAIL] la prueba llega hasta el final -- %s" % failed_to_run, flush=True)
    passed = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d comprobaciones pasan" % (passed, len(results)), flush=True)
    if work and not args.keep and passed == len(results):
        shutil.rmtree(work, ignore_errors=True)
    elif work:
        print("[info] carpeta de trabajo conservada: %s" % work, flush=True)
    return 0 if results and passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
