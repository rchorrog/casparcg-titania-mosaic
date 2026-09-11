# Cómo probar el parche de sincronización

Este documento existe porque el parche llegó **sin construir y sin probar** (entrada 3 de
`BUILD.md`). No es una lista de buenas intenciones: son los comandos, los números concretos y el
criterio de aprobado o suspenso de cada prueba.

**El orden importa.** La prueba 1 es la de contención y va primero: si con los flags apagados el
comportamiento no es el de hoy, nada de lo demás merece medirse todavía.

Y una regla que se salta sola si no se escribe: **la prueba 2 tiene que suspender con los flags
apagados.** Una prueba que no puede fallar no demuestra nada. Si al cortar una rama el offset entre
feeds *no* se descoloca con el parche desactivado, entonces el defecto que esto dice arreglar no
está ocurriendo en ese equipo, y hay que averiguar por qué antes de encender ningún flag.

Los comandos de aquí están probados salvo donde se diga lo contrario. Lo que **no** se ha podido
probar es el parche en sí: donde se escribió esto no hay toolchain de Linux.

---

## 0. Construir y desplegar

Receta de siempre (`BUILD.md`, fase 4C de `setup-desde-cero.sh`):

```bash
git fetch origin
git checkout fix/srt-input-recovery-and-rate-governor
mkdir -p build && cd build
cmake ../src -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_CEF=OFF   # + resto de flags de la receta
cmake --build . --parallel "$(nproc)"
cmake --install . --prefix ../staging
```

**Guardar el binario actual antes de sustituirlo.** Es la vía de vuelta y hace falta en la prueba 1:

```bash
cp ~/mosaic/casparcg/bin/casparcg ~/mosaic/casparcg/bin/casparcg.pre-sync
```

Volver atrás es copiar ese fichero encima y reiniciar el servicio. No hay migración de estado ni
cambio de configuración que deshacer: con los flags apagados el binario nuevo se comporta como el
viejo, que es justo lo que comprueba la prueba 1.

---

## 1. Leer los contadores

Todo lo que hay que medir se publica por OSC, sin reiniciar y sin tocar la configuración. La ruta
de un productor es:

```
/channel/<canal>/stage/layer/<capa>/foreground/sync/<campo>
```

Suscribirse desde AMCP (puerto 5250, el de `casparcg.config`):

```bash
printf 'OSC SUBSCRIBE 6260\r\n' | nc localhost 5250
```

El receptor está en **`tools/sync/watch_sync.py`**, en este mismo repo. Es autónomo — solo stdlib
de Python 3 — porque tiene que correr en el propio mosaic sin instalar nada:

```bash
python3 tools/sync/watch_sync.py 6260
```

Va como fichero y no como bloque de código pegado aquí por una razón concreta: CasparCG manda
paquetes **`#bundle`**, no mensajes OSC sueltos (`src/protocol/osc/client.cpp:123-139`), así que un
receptor ingenuo escrito de memoria no imprime absolutamente nada y parece que no hay datos. El de
este repo tiene el parseo de bundles probado contra bundles sintéticos y contra un socket UDP real.

Salida, una línea por capa y por segundo:

```
16:23:07 capa   1-10  modo=governed ts=wallclock slip=    -3 rep=  1204 drop=  1207 disc=  1 reconn=  0 buf=4/4.1 ppm=12
```

### Los dos campos que responden a la investigación de MAIN/BACKUP

`file/time` — el campo que ya existía y que `osc-bridge.js` ya consume — **no es el PTS de la
señal**. Sale de la salida del grafo de filtros, y `fps=fps=...` (que se añade a *todos* los grafos
de vídeo, `av_producer.cpp:410`) sustituye la marca de tiempo del stream por un contador propio
(`vf_fps.c:305`). Los dos coinciden mientras no se descarte nada y se separan en silencio en cuanto
se descarta algo — que es justo el fallo que se busca.

Por eso este build publica además:

| campo | qué es |
| :--- | :--- |
| `sync/source-time` | el reloj **de la señal**, tomado en el decodificador antes del grafo, normalizado igual que `file/time` para poder compararlos |
| `sync/graph-slip-frames` | la diferencia entre los dos, en frames |

Para qué sirve cada uno:

* **`graph-slip-frames` distinto de cero y creciendo** significa que el grafo está descartando o
  duplicando respecto al origen. Es la baldosa congelada con el buffer sano, hecha número.
* **`source-time` sí es comparable entre capas** para un par MAIN/BACKUP que salga del mismo
  codificador, cosa que `file/time` **no** es: cada producer normaliza contra su propio
  `input_->start_time`, así que dos capas pueden marcar el mismo `file/time` con contenidos de
  instantes distintos. Para comparar un par, comparar `source-time`.

**El número que decide casi todo es `net-slip-frames`** — repeticiones menos descartes acumulados.
Es el trinquete hecho número: si sube y no vuelve a bajar, esa capa se está quedando atrás de forma
permanente. Los demás contadores explican *por qué*.

---

## 2. Prueba 1 — contención (obligatoria, y va primera)

**Qué se afirma:** con los flags apagados, el binario nuevo se comporta exactamente como el viejo.

```bash
# casparcg.config: no añadir el bloque <sync>, o dejarlo entero en false
systemctl restart casparcg
python3 tools/sync/watch_sync.py 6260
```

**Aprobado si**, con las 20 entradas en marcha durante ≥ 30 min:

| contador | valor esperado |
| :--- | :--- |
| `mode` | `passive` en todas las capas |
| `timestamps` | `source` en todas las capas |
| `drops` | **0**, sin excepción — con el gobernador apagado no puede descartar nada |
| `repeats` | puede subir; es el comportamiento de hoy, no un fallo |
| `reconnects` | 0 si la red está sana |

Y a ojo: el mosaico se ve como siempre, los vúmetros se mueven, los subtítulos entran.

**Cualquier `drops > 0` aquí es un defecto del parche**, no del equipo: significa que el gobernador
está actuando con el flag apagado. Parar y reportar.

---

## 3. Prueba 2 — reproducir el fallo (la que tiene que suspender primero)

**Qué se afirma:** hoy, un corte en una entrada descoloca ese feed de forma permanente.

Hacen falta dos entradas comparables entre sí. **Un solo codificador y dos transportes**, no dos
codificadores: dos procesos `ffmpeg` independientes arrancan con milisegundos de diferencia y esa
diferencia se confundiría con lo que se quiere medir. Con el muxer `tee` las dos ramas llevan los
mismos frames con los mismos timestamps, así que **cualquier divergencia observada viene del camino
de recepción y de nada más**. Comprobado: las dos salidas del `tee` salen byte a byte idénticas.

```bash
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf
IP=<ip-del-mosaic>

ffmpeg -re -f lavfi -i "testsrc2=size=640x360:rate=25" \
  -vf "drawtext=fontfile=$FONT:text='%{pts\:hms}':fontsize=48:fontcolor=white:box=1:boxcolor=black:x=20:y=20" \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 25 -b:v 2M \
  -map 0:v \
  -f tee "[f=mpegts]srt://$IP:9001?mode=caller|[f=mpegts]srt://$IP:9002?mode=caller"
```

El `-map 0:v` no es opcional: sin él, `tee` falla con *"Output file does not contain any stream"*.

Si el `ffmpeg` del equipo no trae `drawtext` (necesita libfreetype), vale cualquier fuente con
movimiento rápido y reconocible; lo único que hace falta es poder decir a ojo si las dos baldosas
van al mismo frame.

Cargar las dos ramas en dos capas del mosaico. Al venir del mismo codificador los dos relojes
deberían marcar **el mismo valor**; si hay desfase inicial se anota como punto de partida — es fase
de llegada, no un error.

Ahora el corte. Tiene que afectar a **una sola** rama, así que matar el `ffmpeg` no sirve: tiraría
las dos. Se corta el tráfico de un puerto en el receptor:

```bash
# en el mosaic: tirar SOLO la rama 9001 durante 2 s
sudo iptables -I INPUT -p udp --dport 9001 -j DROP
sleep 2
sudo iptables -D INPUT -p udp --dport 9001 -j DROP
```

Así la rama 9002 queda intacta y hace de referencia: se mide la diferencia entre las dos, no el
comportamiento absoluto de ninguna.

**Con los flags apagados se espera que suspenda:**

- el offset entre los dos relojes **cambia y se queda cambiado**;
- `net-slip-frames` de esa capa sube y no baja;
- `reconnects` sube a 1 — esto sí es el parche trabajando, y funciona con el gobernador apagado
  porque la reconexión no está detrás del flag.

> **Si el offset vuelve solo a su valor original con los flags apagados, parar aquí.** El defecto no
> se está reproduciendo en ese equipo, y encender el gobernador estaría corrigiendo algo que no
> ocurre. Averiguar por qué antes de seguir.

Ahora encender:

```xml
<sync>
    <enabled>true</enabled>
    <target-frames>4</target-frames>
    <deadband-frames>2</deadband-frames>
    <min-interval-frames>25</min-interval-frames>
</sync>
```

Reiniciar y repetir el corte. **Aprobado si** el offset vuelve a su valor inicial en unos pocos
segundos, `drops` sube en la capa afectada y `net-slip-frames` regresa cerca de donde estaba.

---

## 4. Prueba 3 — el trinquete, diez veces

Repetir el corte de la prueba 2 **diez veces**, con un par de minutos entre cada uno.

| flags | `net-slip-frames` tras 10 cortes |
| :--- | :--- |
| apagados | sube de forma monótona, sin recuperarse |
| encendidos | vuelve cerca del valor de partida cada vez; sin acumulación |

Esto es el trinquete reproducido y luego eliminado. Es lo que de verdad justifica el cambio: la
prueba 2 sola podría ser suerte.

---

## 5. Prueba 4 — `wallclock-timestamps`

```xml
<sync>
    <enabled>true</enabled>
    <wallclock-timestamps>true</wallclock-timestamps>
    ...
</sync>
```

Comprobar **primero** que aparece `ts=wallclock` en el volcado. Si sigue diciendo `source`, la
opción no se está aplicando y el resto de la prueba no significa nada.

Repetir la prueba 3 con esto encendido y comparar contra la tabla anterior. **No hay un aprobado
absoluto aquí, es una comparación**: dice si con esto basta o si hace falta algo más en el lado de
los timestamps. En concreto, es lo que decide si merece la pena implementar el arrastre del PTS de
origen a través del grafo de filtros — la opción "B" discutida en el PR, que no se ha implementado
precisamente a la espera de este número.

Vigilar también el efecto secundario esperado: al medir llegada y no captura, un feed con peor
camino de red se lee como permanentemente más tardío. Eso es la opción funcionando, no un fallo,
pero conviene verlo y anotarlo.

---

## 6. Prueba 5 — audio y vúmetros

En este despliegue **el audio no se escucha: solo se ve, a través de los vúmetros**. Eso simplifica
esta prueba y elimina la única pega que el gobernador tenía pendiente de aprobación — descartar un
frame descarta el audio que viaja con él, y un chasquido que nadie oye no es un problema.

Lo que sigue importando es que la medida sea correcta. Con audio real en al menos dos capas,
provocar una corrección (prueba 2 con los flags encendidos) y comprobar:

- los vúmetros por capa siguen moviéndose y **siguen correspondiendo a su capa** — que no se crucen
  entre baldosas es lo que de verdad hay que mirar;
- responden a cambios de nivel en el origen sin quedarse pegados;
- no se quedan a cero en la capa corregida después de la corrección.

Un descarte le quita al vúmetro los samples de un frame. A la cadencia esperada de correcciones
(como mucho una cada `min-interval-frames`, o sea una cada segundo en el peor caso) eso es
invisible en una barra de nivel. Si un vúmetro **sí** acusa el descarte de forma visible, es un
hallazgo: significa que el gobernador está corrigiendo mucho más de lo previsto, y el número que lo
confirma es `drops`.

---

## 7. Prueba 6 — soak

Veinte entradas, con la salida redirigida a fichero, **mínimo 72 horas**. No menos, y la razón es
concreta: el segundo wrap de PTS llega hacia las ~53 h y es el fallo que no aparece antes. Un soak
de 24 h no lo puede ver.

```bash
python3 tools/sync/watch_sync.py 6260 > /var/log/sync-soak.log 2>&1 &
```

**Aprobado si:**

| contador | criterio |
| :--- | :--- |
| `net-slip-frames` | oscila alrededor de un valor; **no crece de forma sostenida** |
| `discontinuities` | 0 o unos pocos; un pico hacia las ~53 h es lo esperado y es el parche actuando |
| `reconnects` | bajo y atribuible a incidencias de red reales |
| `drops` / `repeats` | crecen despacio y de forma comparable entre sí |
| memoria del proceso | estable |

Cualquier contador que suba de forma sostenida y monótona es la siguiente pregunta, no un aprobado.

---

## 8. Qué reportar

Con que sea copiable vale. Por prueba: flags usados, duración, y el bloque de contadores al
principio y al final. Para las pruebas 2 y 3, el offset entre relojes antes y después de cada corte.

Y si algo suspende, **el binario anterior está en `casparcg.pre-sync`** — copiar encima y reiniciar.
Con los flags apagados el binario nuevo también sirve de vuelta atrás, que es precisamente lo que la
prueba 1 deja demostrado.
