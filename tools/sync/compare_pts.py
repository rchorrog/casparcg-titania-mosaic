#!/usr/bin/env python3
"""Compara el PTS de dos señales que llevan el mismo contenido, emparejando paquetes por huella.

    compare_pts.py MAIN.csv BACKUP.csv

Los CSV los produce ffprobe con:

    ffprobe -v error -select_streams v:0 -show_entries packet=pts_time,size \\
        -of csv=p=0 -read_intervals "%+30" "udp://239.192.21.74:7111" > main_pkts.csv

Para que comparar PTS entre dos streams signifique algo hace falta saber que ambos numeran desde
el mismo cero. Esto lo averigua sin suponerlo: los tamanos de los paquetes de video varian mucho
(I/P/B), asi que la secuencia de tamanos es una huella practicamente unica del contenido. Si se
encuentra la misma secuencia en las dos capturas, esos paquetes son el MISMO instante de programa,
y entonces la diferencia de sus PTS es directamente el desfase de epoch entre las dos senales:

  * diferencia ~0 y constante -> comparten epoch: comparar PTS entre ellas es valido, y el
    desfase real de contenido se mide restando PTS.
  * diferencia constante pero != 0 -> no comparten cero, pero el offset es medible una vez y
    aplicable despues.
  * sin coincidencia de huella -> no llevan el mismo contenido (o hay demasiada perdida de
    paquetes), y la comparacion por PTS no procede.
"""
import sys
from collections import Counter

NEEDLE   = 60    # paquetes consecutivos usados como huella
MIN_UNIQ = 20    # tamanos distintos minimos en la huella: menos que eso no identifica nada


def load(path):
    """CSV de ffprobe -> [(pts_time, size)], descartando lineas sin PTS."""
    out = []
    with open(path) as fh:
        for line in fh:
            parts = line.strip().split(',')
            if len(parts) < 2:
                continue
            try:
                out.append((float(parts[0]), int(parts[1])))
            except ValueError:
                continue   # 'N/A' u otras lineas sin timestamp utilizable
    return out


def find_offset(a, b):
    """Busca la huella de tamanos de 'a' dentro de 'b'. Devuelve (i_a, i_b) o None."""
    sizes_b = [s for _, s in b]

    # Se prueban varias posiciones de partida: si justo ahi hubo perdida de paquetes en una de
    # las dos capturas, la huella no aparece y hay que intentarlo mas adelante.
    for start in range(0, max(1, len(a) - NEEDLE), NEEDLE // 2):
        needle = [s for _, s in a[start:start + NEEDLE]]
        if len(needle) < NEEDLE:
            break
        if len(set(needle)) < MIN_UNIQ:
            continue   # tramo demasiado uniforme para identificar nada

        for j in range(len(sizes_b) - NEEDLE + 1):
            if sizes_b[j:j + NEEDLE] == needle:
                return start, j
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    a, b = load(sys.argv[1]), load(sys.argv[2])
    print("main:   %5d paquetes, pts de %.3f a %.3f" % (len(a), a[0][0], a[-1][0]) if a else "main: vacio")
    print("backup: %5d paquetes, pts de %.3f a %.3f" % (len(b), b[0][0], b[-1][0]) if b else "backup: vacio")
    if not a or not b:
        sys.exit("Alguna captura esta vacia.")

    hit = find_offset(a, b)
    if hit is None:
        print("\nSIN COINCIDENCIA: la secuencia de tamanos de una no aparece en la otra.")
        print("O no llevan el mismo contenido, o hay demasiada perdida de paquetes.")
        print("Comparar PTS entre estas dos senales no tiene sentido; habria que ir por contenido")
        print("(correlacion de audio).")
        return

    i, j = hit
    print("\nHuella encontrada: paquete %d de main == paquete %d de backup" % (i, j))

    # Con el emparejamiento fijado, la diferencia de PTS deberia ser la misma en todos los
    # paquetes emparejados. Si no lo es, los relojes corren a ritmos distintos.
    diffs = []
    for k in range(NEEDLE):
        if i + k < len(a) and j + k < len(b):
            diffs.append(b[j + k][0] - a[i + k][0])

    lo, hi = min(diffs), max(diffs)
    avg    = sum(diffs) / len(diffs)
    print("diferencia de PTS para el mismo contenido: %.6f s  (min %.6f / max %.6f)" % (avg, lo, hi))

    print()
    if abs(avg) < 0.001:
        print("MISMO EPOCH: los PTS coinciden para el mismo contenido.")
        print("Comparar PTS entre estas dos senales ES valido, y la diferencia de PTS en vivo mide")
        print("directamente el desfase de contenido.")
    elif hi - lo < 0.010:
        print("EPOCH DISTINTO PERO ESTABLE: difieren en %.3f s de forma constante." % avg)
        print("Comparar PTS sirve si se resta ese offset, que hay que medir una vez por par")
        print("(y volver a medir si algun codificador se reinicia).")
    else:
        print("EPOCH INESTABLE: la diferencia varia %.3f s a lo largo de la huella." % (hi - lo))
        print("Los dos relojes no corren igual; el PTS no sirve para comparar estas dos senales.")


if __name__ == '__main__':
    main()
