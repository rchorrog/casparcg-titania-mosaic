#!/usr/bin/env bash
# Captura el audio de dos senales (Main/Backup del mismo programa) en un unico WAV estereo, con
# ambas en el MISMO eje temporal, para medir su desfase real con correlate_audio.py.
#
#   capture_pair.sh udp://239.192.21.74:7111 udp://239.192.28.74:8112 [segundos] [directorio]
#
# Un solo proceso ffmpeg con las dos entradas y amerge: canal 0 = main, canal 1 = backup. Asi el
# alineamiento entre ambas lo resuelve el propio grafo de filtros y no hay que reconciliar dos
# relojes despues.
#
# Por que NO se capturan dos ficheros por separado con timestamps de reloj de pared (primer
# intento, 2026-09-11): con -use_wallclock_as_timestamps los paquetes de audio de un TS llegan a
# rafagas y sus marcas se pisan entre si; el muxer las recorta ("Non-monotonic DTS ... changing
# to") con saltos de ~70 ms, justo el orden de magnitud que queremos medir. La referencia absoluta
# quedaba inservible.
set -euo pipefail

URL_A="${1:?uso: capture_pair.sh <url_main> <url_backup> [segundos] [directorio]}"
URL_B="${2:?uso: capture_pair.sh <url_main> <url_backup> [segundos] [directorio]}"
SECS="${3:-60}"
DIR="${4:-/tmp/sync}"

mkdir -p "$DIR"
cd "$DIR"

echo "Capturando ${SECS}s de audio de ambas senales en un unico WAV..."
echo "  ch0 (main)   $URL_A"
echo "  ch1 (backup) $URL_B"
echo "  salida       $DIR/pair.wav   (log de ffmpeg en $DIR/capture.log)"

# -t antes de cada -i limita la LECTURA de esa entrada.
# pan=mono|c0=c0 coge el canal izquierdo: sirve igual para fuentes mono y estereo, y para
# correlacionar envolventes no aporta nada mezclar los dos canales.
# Los errores "non-existing PPS / decode_slice_header / no frame" que aparezcan en capture.log son
# del VIDEO del stream (perdida de paquetes en origen), no afectan a esta medida.
ffmpeg -hide_banner -loglevel error \
    -use_wallclock_as_timestamps 1 -t "$SECS" -i "${URL_A}?fifo_size=4194304&overrun_nonfatal=1" \
    -use_wallclock_as_timestamps 1 -t "$SECS" -i "${URL_B}?fifo_size=4194304&overrun_nonfatal=1" \
    -filter_complex "[0:a]pan=mono|c0=c0,aresample=48000[a];[1:a]pan=mono|c0=c0,aresample=48000[b];[a][b]amerge=inputs=2[m]" \
    -map "[m]" -c:a pcm_s16le -y pair.wav 2> capture.log || {
        echo
        echo "ffmpeg fallo. Ultimas lineas de $DIR/capture.log:"
        tail -20 capture.log
        exit 1
    }

echo
ls -la pair.wav
echo
echo "Analiza con:"
echo "   python3 $(cd "$(dirname "$0")" && pwd)/correlate_audio.py $DIR/pair.wav"
