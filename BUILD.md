# Origen y build de este árbol

Este repositorio es el árbol fuente real de CasparCG que se compila y despliega en producción
(mosaic1, mosaic2, mosaic4, mosaic5). Documentado aquí porque el árbol se generó originalmente
descargando un `.zip` (sin histórico git de CasparCG) y aplicando parches propios a mano — sin
este fichero, el origen y el porqué de cada cambio no se puede reconstruir solo mirando el árbol.

## Base

- **CasparCG oficial `v2.5.0-stable`**, descargado como fuente sin histórico git desde
  `https://github.com/CasparCG/server/archive/refs/tags/v2.5.0-stable.zip` (por eso este repo
  empieza en un commit "vacío" sin relación con el histórico oficial de CasparCG).
- Confirmado por AMCP (`VERSION` → `2.5.0 N/A Stable`) que coincide con las constantes de
  versión (`CONFIG_VERSION_MAJOR/MINOR/TAG`) del tag `v2.5.0-stable` en el repo `server`.

## Parches aplicados, en orden

1. **`patches/osc-audio-per-layer-on-v2.5.0-stable.patch`** (commit `2d29225`) — expone el pico
   de audio por capa vía OSC (`/channel/N/mixer/layer/L/audio/peak/CH`), necesario para el
   vúmetro por capa. 7 ficheros: `src/core/mixer/audio/audio_mixer.cpp/h`,
   `src/core/mixer/mixer.cpp/h`, `src/core/producer/stage.cpp/h`, `src/core/video_channel.cpp`.
   No toca `av_input.cpp`/`av_producer.cpp` — el `.deb` oficial (Fase 4B de
   `setup-desde-cero.sh`) no trae este desglose por capa.
2. **Fix SIGILL en `src/modules/html/html.cpp`** (commit `74d19ca`) — mitiga un crash recurrente
   de CasparCG (`OnMemoryDump` en `malloc_dump_provider.cc`, dentro de `libcef.so`): el sistema
   de tracing de memoria interno de Chromium ("memory-infra"), disparado por un timer periódico,
   no por ninguna acción de Titania/CasparCG. Causa raíz localizada con `gdb`/coredump real
   (backtrace: `OnMemoryDump()` en `malloc_dump_provider.cc:465`), no una suposición. Mitigación:
   en `OnBeforeCommandLineProcessing`, tras `remote-allow-origins`, se añaden
   `--disable-features=MemoryInfra`, `--disable-component-update` y
   `--disable-background-networking`. Verificado en runtime (vía `/proc/<pid>/cmdline` de los
   subprocesos CEF reales) y estable sin recurrencia desde el 2026-07-31.

3. **`patches/ffmpeg-srt-recovery-and-rate-governor-on-v2.5.0-stable.patch`** (commits `a42e4a0`,
   `de65a45`, …) — sincronización de las entradas SRT del mosaico. 3 ficheros:
   `src/modules/ffmpeg/producer/av_input.cpp/h`, `src/modules/ffmpeg/producer/av_producer.cpp`,
   más el bloque comentado de `src/shell/casparcg.config`. **Nada de esto está construido ni
   probado** — ver el aviso al final de la entrada, que no es una formalidad.

   - **El hilo de lectura moría para siempre con un solo error.** El `try` envolvía todo el
     `while (true)` y el `catch (...)` quedaba fuera del bucle, así que el primer fallo de
     `av_read_frame` — cualquiera, también uno transitorio — salía del bucle y terminaba el hilo.
     Nadie lo supervisa: esa entrada quedaba muerta durante toda la vida del proceso, sin
     paquetes, sin EOF y sin más errores. El `rw_timeout` es de 60 s, o sea que un minuto de
     silencio de un encoder retiraba ese tile hasta reiniciar el servidor. Ahora el `try` está
     **dentro** del bucle y una entrada en vivo reabre con backoff exponencial hasta 5 s. Vivo se
     decide por protocolo (`srt`, `udp`, `rtp`, `rtsp`, `rtmp`, `http`, `tcp`): un fichero
     conserva el comportamiento anterior exacto, porque ahí un error de lectura sí es real.
   - **Un salto de timeline congelaba el tile sin dejar rastro.** `fps=fps=N/D:start_time=...` se
     añade siempre al grafo de vídeo, y `vf_fps` siembra su contador una sola vez
     (`frame->pts = s->next_pts++`, `vf_fps.c:305`); nunca lo resiembra. Así que ante una
     reconexión SRT, un reinicio de encoder o una discontinuidad de PCR, compara timestamps
     nuevos contra un ancla vieja y descarta todos los frames o duplica uno, durante horas. El
     buffer se ve sano y nadie marca `underflow`: no hay síntoma que buscar en el log. La
     detección va en el hilo de decodificación, justo tras `best_effort_timestamp`, que es el
     último punto donde el timestamp original aún existe; un salto de más de 5 s en cualquier
     sentido reconstruye el grafo por la misma vía que ya usa un seek.
   - **El wrap de PTS entra en esa lista, pero solo a partir del segundo — y esto está medido.**
     `libavformat` corrige el desbordamiento en la ruta de lectura (`wrap_timestamp()` sobre
     `pkt->pts`/`pkt->dts`, `demux.c:559-560`; `correct_ts_overflow` a 1 por defecto;
     `pts_wrap_bits = 33` en `mpegts.c:928`), pero **solo uno**: `update_wrap_reference` sale
     antes si la referencia ya está puesta (`demux.c:487`), o sea que se fija una vez y no se
     revisa, tal como dice la descripción de la opción ("correct **single** timestamp
     overflows"). Medido con FFmpeg 8.1.1 sobre TS sintéticos: un flujo de 5 s que cruza **un**
     wrap da 1 salto hacia atrás en crudo y **0** con la corrección puesta; uno de 53 h que cruza
     **dos** da 2 saltos en crudo y **siguen siendo 2** con la corrección puesta. Los residuales
     son de ≈ −95.383 s, cuatro órdenes de magnitud por encima del umbral de 5 s. En un mosaico
     24/7: el primer wrap (~26,5 h) es inofensivo, y desde las ~53 h cada wrap llega al
     decodificador. (Fichero sintético, no SRT en vivo — la aritmética no depende del transporte,
     pero eso último no está probado.)
   - **Un gobernador de ritmo, apagado por defecto.** `next_frame` solo podía repetir cuando se
     quedaba sin frames — y sin avanzar nada, así que el feed quedaba un frame más atrasado de
     forma permanente con cada episodio. El caso contrario es peor: una fuente algo rápida no
     tenía dónde poner el sobrante y contrapresionaba al demuxer hasta que el socket perdía
     paquetes. Con `<sync><enabled>true</enabled></sync>` puede descartar, guiándose por una EWMA
     de ocupación del buffer (~4 s) y corrigiendo como mucho cada `min-interval-frames`.
     **Apagado es idéntico bit a bit al comportamiento actual**, y los contadores `sync/*`
     (repeticiones, descartes, discontinuidades, reconexiones, deriva neta, offset en ppm,
     profundidad de buffer) se publican en ambos casos por `INFO` y OSC, para poder medir un
     equipo antes de encender nada. `sync/net-slip-frames` es el número que importa: repeticiones
     menos descartes, y debería quedarse cerca de cero.

     Sobre el audio: descartar un frame descarta el audio que viaja con él. En este despliegue el
     audio **no se escucha, solo se mide** con los vúmetros, así que el chasquido es irrelevante y
     lo único que hay que preservar es que la medida siga siendo correcta y siga correspondiendo a
     su capa. En un contexto donde el audio se emitiera, esto no valdría.
   - **Un epoch común para feeds que no lo tienen** (`<wallclock-timestamps>`, apagado por
     defecto, solo entradas en vivo). Veinte encoders sin PTP producen veinte epochs de PTS sin
     relación entre sí, así que un timestamp no puede decir que dos feeds se capturaron en el
     mismo instante — solo que un feed es consistente consigo mismo.
     `use_wallclock_as_timestamps` sustituye el PTS por el reloj de esta máquina cuando llega el
     paquete (`demux.c:565`): un solo epoch para todos y, de paso, el wrap deja de importar. El
     coste conviene decirlo claro — mide **llegada**, no captura, así que el jitter de red pasa a
     ser jitter de tiempo y un feed con peor camino se lee como permanentemente más tardío. Para
     un muro de monitorización es buen cambio; donde importe el tiempo de captura absoluto, es la
     herramienta equivocada. `sync/timestamps` publica cuál de los dos está activo, porque cambia
     el significado de todos los demás contadores.

   > **Sin construir y sin probar.** Este árbol es de Linux y aquí solo se ha podido verificar
   > sintácticamente (`cl /Zs` contra las cabeceras reales de FFmpeg, Boost y TBB, con la
   > comprobación validada introduciendo un error a propósito y confirmando que lo detecta). No
   > se ha compilado el árbol completo ni se ha ejecutado. Falta, como mínimo: el build; que con
   > los flags apagados el comportamiento sea el de hoy; provocar un corte real de un emisor y
   > ver si el offset entre feeds vuelve a su sitio; medir con `wallclock-timestamps` encendido
   > frente a apagado; y comprobar que los vúmetros por capa sobreviven a una corrección.

   Nota aparte: el commit `d74e0a5` (`use-gl=disabled` en el gpu-process de CEF), que es la punta
   de la rama de la que parte esto, no figura en esta lista.

## Receta de build (misma que `build-and-deploy.sh` / Fase 4C de `setup-desde-cero.sh`)

```bash
mkdir -p build && cd build
cmake ../src \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SYSTEM_CEF=OFF \
  # ... resto de flags, ver server/tools/linux/instalacion/setup-desde-cero.sh fase4c_build_osc_patch()
cmake --build . --parallel "$(nproc)"
cmake --install . --prefix ../staging
```

Tras el build: copiar `staging/bin/casparcg`, `staging/lib/*.so*` y los recursos de CEF
(`icudtl.dat`, `*.pak`, `v8_context_snapshot.bin`, `libcef.so`, `chrome-sandbox`, `locales/`...)
al directorio final, y `patchelf --set-rpath '$ORIGIN'` sobre el binario para que sea
autocontenido. Detalle completo en `server/tools/linux/instalacion/setup-desde-cero.sh`
(función `fase4c_build_osc_patch`).

## Despliegue real

- **mosaic1, mosaic2, mosaic4, mosaic5**: unificados en `~/mosaic/casparcg/bin/casparcg`
  (`ExecStart=` de `casparcg.service` apunta ahí, no a `/usr/bin/casparcg-server-2.5`) — este
  árbol, con los 2 parches de arriba aplicados. Confirmado por `ExecStart=` real en mosaic2 y
  mosaic4 (2026-08-26).
- **mosaic3**: queda fuera a propósito — sigue en una línea distinta ("fork propio", rama
  `development_gpu` del repo `server`, versión `2.6.0 Dev`), no en este árbol. Migrarlo o no
  quedó pendiente de decisión desde antes de vacaciones (ver memoria
  `mosaic4-casparcg-sigill-crash.md`) — no resuelto todavía.
