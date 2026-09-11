#!/usr/bin/env bash
# Captura simultanea del audio de dos senales (Main/Backup del mismo programa) conservando un
# eje de tiempo comun, para medir su desfase real con correlate_audio.py.
#
#   capture_pair.sh udp://239.192.21.74:7111 udp://239.192.28.74:8112 [segundos] [directorio]
#
# La clave es -use_wallclock_as_timestamps 1 junto con -copyts: sin ellos cada fichero queda
# rebasado a cero por separado y se pierde la relacion temporal entre ambas capturas, que es
# justo lo que se quiere medir. Matroska si conserva el timestamp absoluto, asi que despues se
# recupera con ffprobe y la alineacion no depende de que los dos procesos arranquen en el mismo
# milisegundo.
set -euo pipefail

URL_A="${1:?uso: capture_pair.sh <url_main> <url_backup> [segundos] [directorio]}"
URL_B="${2:?uso: capture_pair.sh <url_main> <url_backup> [segundos] [directorio]}"
SECS="${3:-60}"
DIR="${4:-/tmp/sync}"

mkdir -p "$DIR"
cd "$DIR"

echo "Capturando ${SECS}s de audio de ambas senales..."
echo "  main   $URL_A"
echo "  backup $URL_B"

# -t antes de -i (limita la LECTURA): con -copyts, un -t de salida se compara contra timestamps
# absolutos de epoch y no corta cuando se espera.
ffmpeg -hide_banner -loglevel warning -use_wallclock_as_timestamps 1 -t "$SECS" \
    -i "${URL_A}?fifo_size=4194304&overrun_nonfatal=1" \
    -map 0:a:0 -c:a pcm_s16le -ar 48000 -ac 1 -copyts -y main.mka &
PID_A=$!
ffmpeg -hide_banner -loglevel warning -use_wallclock_as_timestamps 1 -t "$SECS" \
    -i "${URL_B}?fifo_size=4194304&overrun_nonfatal=1" \
    -map 0:a:0 -c:a pcm_s16le -ar 48000 -ac 1 -copyts -y bckp.mka &
PID_B=$!
wait "$PID_A" "$PID_B"

first_pts() {
    ffprobe -v error -select_streams a:0 -show_entries packet=pts_time \
        -read_intervals "%+#1" -of csv=p=0 "$1" | head -1
}

T_A="$(first_pts main.mka)"
T_B="$(first_pts bckp.mka)"

ffmpeg -v error -i main.mka -c:a pcm_s16le -y main.wav
ffmpeg -v error -i bckp.mka -c:a pcm_s16le -y bckp.wav

echo
echo "start_time absoluto (epoch): main=$T_A backup=$T_B"

# Si el muxer rebaso los timestamps a ~0, el eje comun se pierde y solo queda asumir arranque
# simultaneo - sigue valiendo para decidir viabilidad (la nitidez del pico no se ve afectada),
# pero el valor absoluto del desfase se lleva el error de arranque de los dos procesos.
if [ "$(printf '%.0f' "${T_A:-0}")" -lt 1000000 ]; then
    echo
    echo "AVISO: los timestamps salieron rebasados a cero, no absolutos."
    echo "Analiza sin start_time (se asumira arranque simultaneo, +-decimas de segundo):"
    echo "   python3 $(dirname "$0")/correlate_audio.py $DIR/main.wav $DIR/bckp.wav"
else
    echo
    echo "Analiza con:"
    echo "   python3 $(dirname "$0")/correlate_audio.py $DIR/main.wav $DIR/bckp.wav $T_A $T_B"
fi
