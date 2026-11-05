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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", help="ejecutable de CasparCG a arrancar (si se omite, se usa uno ya arrancado)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5290)
    ap.add_argument("--media", help="carpeta de media del servidor ya arrancado")
    ap.add_argument("--workdir", help="carpeta para config, media y logs (por defecto, temporal)")
    ap.add_argument("--keep", action="store_true", help="no borrar la carpeta de trabajo")
    ap.add_argument("--ffprobe", default=shutil.which("ffprobe"))
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
