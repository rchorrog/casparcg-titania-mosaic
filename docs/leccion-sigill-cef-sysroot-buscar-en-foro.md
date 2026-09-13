# Lección aprendida: crash de CEF sin símbolos → buscar en el foro antes que nada

Fecha: 2026-09-13. Contexto: investigación de reinicios frecuentes de CasparCG en mosaic2/mosaic5
(`SIGILL` recurrente, ver `BUILD.md` y la memoria de proyecto `mosaic4-casparcg-sigill-crash.md`).

## El problema al que nos enfrentábamos

- `SIGILL` del proceso principal de CasparCG, backtrace dentro de `libcef.so`, siempre en el
  mismo offset (`libcef.so + 0x77270aa`), confirmado en capturas de coredump reales de fechas muy
  distintas (más de dos semanas de diferencia) y en hosts distintos (mosaic2, mosaic5) — o sea,
  no era ruido ni algo introducido por nuestros propios cambios.
- **CEF no publica símbolos de debug para Linux** (comprobado contra el índice oficial de builds:
  `debug_symbols`/`release_symbols` solo existen para `macosarm64`). Sin símbolos, un backtrace de
  `libcef.so` es solo una lista de offsets — no hay forma de saber a qué función corresponde sin
  compilar CEF desde cero con información de debug (horas de build, no razonable solo para
  diagnosticar un crash).
- Con ese callejón sin salida, las opciones que se estaban barajando eran todas caras y a ciegas:
  compilar CEF desde cero con símbolos, cambiar de versión de CasparCG entera, o bajar de versión
  de CEF sin saber si el bug persiste en versiones más antiguas.

## Lo que funcionó: buscar en el foro oficial del proyecto

Antes de invertir en cualquiera de las opciones caras de arriba, **buscar directamente en
[casparcgforum.org](https://casparcgforum.org)** con los síntomas concretos que ya teníamos
(señal, subsistema, versión de SO):

```
site:casparcgforum.org SIGILL crash CEF html
site:casparcgforum.org OnMemoryDump malloc_dump_provider crash
```

El segundo intento (con términos más específicos, sacados directamente del backtrace real que ya
teníamos) encontró un hilo casi calcado:
[CasparCG crashes randomly on Ubuntu 24.04](https://casparcgforum.org/t/casparcg-crashes-randomly-on-ubuntu-24-04/7537) —
mismo `SIGILL`, mismo subsistema (`MemoryInfra`), **mismo offset de crash** reportado en máquinas
de otros usuarios, mismo sistema operativo (Ubuntu 24.04/Noble).

Ese hilo ya traía diagnosticada la causa raíz (el CEF oficial se compila con `sysroot`, lo que
rompe en hosts con glibc >= 2.33) y enlazaba un build de la comunidad sin ese problema
([mko1989/highascg](https://github.com/mko1989/highascg/releases/tag/v.142)) que otro usuario
llevaba **meses en producción con CasparCG 2.5.0 stable en Ubuntu 24 sin problemas**. El fix real
(cambiar la URL/hash de CEF en `Bootstrap_Linux.cmake`) salió de ahí, no de análisis propio de
ensamblador ni de prueba y error con versiones.

## La lección, para la próxima vez

**Antes de gastar horas compilando/depurando algo que ya viene "de fábrica" con CasparCG/CEF, dos
minutos de búsqueda en el foro oficial pueden ahorrar todo ese trabajo.** El proyecto es pequeño
y con mucha antigüedad — es muy probable que un crash reproducible y no exótico ya le haya pasado
a alguien más y esté documentado, con solución incluida.

Cómo buscar de forma efectiva (lo que funcionó aquí):
1. Sacar primero los datos concretos del propio crash (señal, versión de CEF/CasparCG, offset o
   nombre de función si hay símbolos, sistema operativo) — buscar con esos términos exactos, no
   con la descripción genérica del síntoma ("se reinicia").
2. Si la primera búsqueda no da nada, afinar con términos técnicos más específicos sacados del
   propio backtrace/log (aquí, `OnMemoryDump`/`malloc_dump_provider` en vez de solo `SIGILL`).
3. Revisar el hilo completo, no solo el primer mensaje — la solución suele estar en una respuesta
   intermedia, no en el post original.
4. Si el hilo enlaza un build/parche de un tercero, verificar que sea *drop-in* (misma versión
   base, mismo formato de distribución) antes de asumir que hace falta portar nada — aquí bastó
   con cambiar una URL y un hash en el CMake, cero cambios de código.

## Qué no intentar la próxima vez sin pasar antes por esto

- Compilar CEF desde cero con símbolos de debug "para ver qué falla" — coste de horas, y probable
  que la comunidad ya lo haya resuelto sin necesidad de llegar ahí.
- Cambiar de versión de CasparCG entera (2.6.x, forks internos) esperando que arrastre un CEF
  distinto — hay que comprobar primero si de verdad trae una versión de CEF diferente (aquí no la
  traía: mismo commit exacto de CEF, mismo bug).
- Bajar de versión de CEF a ciegas dentro de la misma rama mayor de Chromium (aquí, 142.x) — si
  el bug es de una generación de Chromium entera, un parche de versión dentro de la misma rama
  probablemente no cambia nada (comprobado contra el índice de builds: varios puntos de 142.x
  comparten el mismo `chromium_version` exacto).
