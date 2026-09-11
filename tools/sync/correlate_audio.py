#!/usr/bin/env python3
"""Mide el desfase real entre dos capturas del mismo programa, por correlacion de audio.

    correlate_audio.py PAR.wav                      # 2 canales: ch0=main, ch1=backup
    correlate_audio.py MAIN.wav BACKUP.wav [T_MAIN T_BACKUP]

La forma recomendada es la primera: un unico WAV estereo generado por capture_pair.sh, donde los
dos audios ya vienen alineados en el mismo eje temporal porque los captura un solo ffmpeg con
amerge. Asi no hay que reconciliar dos relojes ni fiarse de timestamps de contenedor.

La segunda forma (dos ficheros) queda para capturas hechas por separado. T_MAIN/T_BACKUP son los
instantes absolutos (epoch, segundos) de inicio de cada una; sin ellos se asume arranque
simultaneo, lo que anade el desfase de arranque de los dos procesos a la medida.

Por que audio y no PTS: comparar marcas de tiempo entre dos codificadores independientes no
funciona (cada uno arranca su epoch por su cuenta; y con wallclock-timestamps la marca pasa a ser
hora de LLEGADA, que es ~ahora para ambos por construccion). El contenido, en cambio, si se puede
alinear: si el instante de programa T llega por Main en T+latA y por Backup en T+latB, las dos
envolventes de audio sobre el eje de hora de llegada estan desplazadas exactamente latA-latB, que
es el desfase que se ve en el mosaico. La marca de tiempo solo aporta el eje x comun.
"""
import math
import struct
import sys
import wave

BLOCK_S             = 0.04   # 40 ms = 1 frame a 25 fps, la resolucion a la que se puede corregir
DB_FLOOR            = -90.0
MAX_LAG_S           = 5.0
PEAK_GUARD_BLOCKS   = 3      # entorno del pico a ignorar al buscar el segundo mejor
MIN_OVERLAP_BLOCKS  = 25     # menos de 1 s de solape no es una medida
MIN_ENVELOPE_SD_DB  = 2.0    # por debajo de esto la envolvente es plana: silencio o tono constante
MIN_PEAK            = 0.5
MIN_PEAK_RATIO      = 1.3    # cuanto tiene que destacar el pico sobre el segundo mejor


def read_envelope(path, channel=0):
    """WAV PCM 16 bits -> envolvente en dBFS del canal indicado, un valor por bloque de 40 ms.

    En dB a proposito: es la misma magnitud que publica CasparCG por OSC
    (mixer/layer/L/audio/peak_mono), asi que lo que se valide aqui vale tal cual para la version
    que lea de OSC. Ademas la correlacion se porta mejor sobre dB que sobre RMS lineal, cuyo rango
    dinamico esta dominado por los picos.
    """
    with wave.open(path, 'rb') as w:
        if w.getsampwidth() != 2:
            sys.exit("%s: se esperaba PCM de 16 bits, tiene %d bytes/muestra"
                     % (path, w.getsampwidth()))
        rate     = w.getframerate()
        channels = w.getnchannels()
        if channel >= channels:
            sys.exit("%s: se pidio el canal %d pero el fichero tiene %d" % (path, channel, channels))
        block    = int(round(rate * BLOCK_S))
        env      = []
        while True:
            raw = w.readframes(block)
            if len(raw) < block * channels * 2:
                break
            samples = struct.unpack('<%dh' % (len(raw) // 2), raw)
            if channels > 1:
                samples = samples[channel::channels]
            acc = 0
            for s in samples:
                acc += s * s
            rms = math.sqrt(acc / len(samples))
            env.append(20.0 * math.log10(rms / 32768.0) if rms > 0 else DB_FLOOR)
    return rate, env


def stddev(series):
    n    = len(series)
    mean = sum(series) / n
    return math.sqrt(sum((x - mean) ** 2 for x in series) / n), mean


def normalize(series):
    """Media cero y varianza uno: hace la medida inmune a diferencias de ganancia, codec y nivel
    entre los dos caminos, que es la objecion obvia a comparar dos codificaciones distintas."""
    sd, mean = stddev(series)
    if sd < 1e-9:
        return None
    return [(x - mean) / sd for x in series]


def correlate(a, b, max_lag):
    """corr[L] = media de a[i]*b[i+L] sobre el solape, con a y b ya normalizados."""
    out = []
    for lag in range(-max_lag, max_lag + 1):
        lo = max(0, -lag)
        hi = min(len(a), len(b) - lag)
        if hi - lo < MIN_OVERLAP_BLOCKS:
            continue
        acc = 0.0
        for i in range(lo, hi):
            acc += a[i] * b[i + lag]
        out.append((lag, acc / (hi - lo)))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    if len(sys.argv) == 2:
        # Un unico WAV estereo de capture_pair.sh: ch0=main, ch1=backup, ya en el mismo eje.
        path_a = path_b = sys.argv[1]
        t_a = t_b = 0.0
        _, env_a = read_envelope(path_a, 0)
        _, env_b = read_envelope(path_b, 1)
        label_a, label_b = path_a + " ch0", path_b + " ch1"
        common_axis = True
    else:
        path_a, path_b = sys.argv[1], sys.argv[2]
        t_a = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
        t_b = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
        _, env_a = read_envelope(path_a)
        _, env_b = read_envelope(path_b)
        label_a, label_b = path_a, path_b
        common_axis = False

    sd_a, _ = stddev(env_a)
    sd_b, _ = stddev(env_b)

    print("MAIN   %-28s %5d bloques (%.1f s)  sd=%.1f dB" % (label_a, len(env_a), len(env_a) * BLOCK_S, sd_a))
    print("BACKUP %-28s %5d bloques (%.1f s)  sd=%.1f dB" % (label_b, len(env_b), len(env_b) * BLOCK_S, sd_b))
    if common_axis:
        print("eje temporal: comun por construccion (un solo ffmpeg con amerge)")
    elif t_a or t_b:
        print("start_time: main=%.3f backup=%.3f  (diferencia %+.3f s)" % (t_a, t_b, t_b - t_a))
    else:
        print("start_time: no facilitado - se asume arranque simultaneo (+-decimas de segundo)")

    norm_a, norm_b = normalize(env_a), normalize(env_b)
    if norm_a is None or norm_b is None:
        sys.exit("\nINCONCLUSO: una de las envolventes es constante, no hay nada que correlacionar.")

    corr = correlate(norm_a, norm_b, int(round(MAX_LAG_S / BLOCK_S)))
    if not corr:
        sys.exit("\nINCONCLUSO: solape insuficiente entre las dos capturas.")

    peak_lag, peak_val = max(corr, key=lambda p: p[1])
    runner = max([v for l, v in corr if abs(l - peak_lag) > PEAK_GUARD_BLOCKS] or [0.0])
    ratio  = peak_val / runner if runner > 1e-9 else float('inf')

    offset_s = (t_b - t_a) + peak_lag * BLOCK_S

    print("\ncorrelacion alrededor del pico:")
    for lag, val in corr:
        if abs(lag - peak_lag) <= 5:
            print("   lag %+4d bloques  %+.3f%s" % (lag, val, "   <-- pico" if lag == peak_lag else ""))

    print("\npico=%.3f   segundo mejor=%.3f   relacion=%.2f" % (peak_val, runner, ratio))
    print("DESFASE: backup va %+.3f s respecto a main  (%+.1f frames a 25 fps)"
          % (offset_s, offset_s / BLOCK_S))
    print("   (positivo = backup llega despues, o sea muestra contenido mas viejo: para alinear,")
    print("    habria que pausar MAIN ese tiempo)")

    print()
    if sd_a < MIN_ENVELOPE_SD_DB or sd_b < MIN_ENVELOPE_SD_DB:
        print("INCONCLUSO: envolvente casi plana (sd < %.1f dB) - silencio o tono constante." % MIN_ENVELOPE_SD_DB)
        print("Repetir la captura con el programa emitiendo contenido normal.")
    elif peak_val >= MIN_PEAK and peak_val >= MIN_PEAK_RATIO * max(runner, 0.0):
        print("VIABLE: el pico es nitido y destaca sobre el resto - el desfase medido es de fiar.")
        print("Siguiente paso: reproducir esta misma medida con los picos que ya llegan por OSC.")
    else:
        print("NO VIABLE POR AUDIO: el pico no destaca lo suficiente (hace falta >=%.2f y >=%.1fx el segundo)."
              % (MIN_PEAK, MIN_PEAK_RATIO))
        print("Posibles causas: las dos senales no llevan el mismo programa de audio, o el")
        print("procesado de cada camino difiere demasiado. Parar aqui y replantear.")


if __name__ == '__main__':
    main()
