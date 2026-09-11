#!/usr/bin/env bash
# Captura el audio de dos senales (Main/Backup del mismo programa) para medir su desfase con
# correlate_audio.py.
#
#   capture_pair.sh udp://239.192.21.74:7111 udp://239.192.28.74:8112 [segundos] [directorio]
#
# Dos ffmpeg en paralelo, cada uno a un WAV mono: sin filtros, sin contenedor, sin timestamps.
# Deliberadamente simple, porque los dos intentos anteriores fallaron por apoyarse en mecanismos
# que no hacian lo que parecia (2026-09-11, mosaic2):
#
#   1. Dos capturas con -use_wallclock_as_timestamps + -copyts sobre Matroska, para reconstruir el
#      eje absoluto desde el contenedor: los paquetes de audio de un TS llegan a rafagas y sus
#      marcas de llegada se pisan, el muxer las recorta ("Non-monotonic DTS ... changing to") con
#      saltos de ~70 ms - el mismo orden de magnitud que se quiere medir.
#   2. Un solo ffmpeg con amerge para tener eje comun por construccion: fallo la negociacion de
#      formatos, y ademas amerge no alinea por PTS - encadena muestras segun llegan, asi que la
#      garantia de eje comun que se le suponia no existe.
#
# Lo que queda: las dos capturas arrancan a la vez pero cada WAV empieza en su propio primer
# paquete, asi que la medida lleva un desfase de captura desconocido (tipicamente < 1 s, por la
# diferencia de sondeo entre ambas). NO afecta al veredicto - la nitidez del pico de correlacion,
# que es lo que decide si la via sirve, es indiferente a un desplazamiento constante. El valor
# absoluto exacto saldra despues por OSC, donde las dos series vienen del mismo tick de canal y no
# hay desfase de captura ninguno.
set -euo pipefail

URL_A="${1:?uso: capture_pair.sh <url_main> <url_backup> [segundos] [directorio]}"
URL_B="${2:?uso: capture_pair.sh <url_main> <url_backup> [segundos] [directorio]}"
SECS="${3:-60}"
DIR="${4:-/tmp/sync}"

mkdir -p "$DIR"
cd "$DIR"

echo "Capturando ${SECS}s de audio de ambas senales..."
echo "  main   $URL_A   -> $DIR/main.wav"
echo "  backup $URL_B   -> $DIR/bckp.wav"
echo "  (log de ffmpeg en $DIR/capture-main.log y capture-bckp.log)"

# Sondeo corto en ambas para que arranquen lo mas a la vez posible: el desfase de captura es
# justo la diferencia entre lo que tarde cada una en sondear.
# Los errores "non-existing PPS / decode_slice_header / no frame" de los logs son del VIDEO del
# stream (perdida de paquetes en origen) y no afectan a esta medida.
grab() {
    ffmpeg -hide_banner -loglevel error \
        -analyzeduration 1000000 -probesize 1000000 \
        -t "$SECS" -i "${1}?fifo_size=4194304&overrun_nonfatal=1" \
        -map 0:a:0 -c:a pcm_s16le -ar 48000 -ac 1 -y "$2" 2> "$3"
}

grab "$URL_A" main.wav capture-main.log &
PID_A=$!
grab "$URL_B" bckp.wav capture-bckp.log &
PID_B=$!

FAIL=0
wait "$PID_A" || FAIL=1
wait "$PID_B" || FAIL=1

if [ "$FAIL" -ne 0 ] || [ ! -s main.wav ] || [ ! -s bckp.wav ]; then
    echo
    echo "ffmpeg fallo. Ultimas lineas de los logs:"
    echo "--- main ---";   tail -15 capture-main.log
    echo "--- backup ---"; tail -15 capture-bckp.log
    exit 1
fi

echo
ls -la main.wav bckp.wav
echo
echo "Analiza con:"
echo "   python3 $(cd "$(dirname "$0")" && pwd)/correlate_audio.py $DIR/main.wav $DIR/bckp.wav"
