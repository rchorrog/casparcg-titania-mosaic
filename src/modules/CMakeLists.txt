cmake_minimum_required (VERSION 3.16)
project("modules")

add_subdirectory(image)
add_subdirectory(ffmpeg)
add_subdirectory(oal)
# DeckLink es opcional: su conversion de video v210 (consumer/v210_strategies.cpp) usa AVX2 via simde y no compila
# con ENABLE_AVX2=OFF (conflicto de tipos __m256i con immintrin.h). Un nodo sin AVX2 (Xeon E5-2620 v2 de mosaicblg4)
# se compila con -DENABLE_AVX2=OFF -DENABLE_DECKLINK=OFF. Por defecto sigue activado.
option(ENABLE_DECKLINK "Compilar el modulo DeckLink (requiere AVX2)" ON)
if (ENABLE_DECKLINK)
	add_subdirectory(decklink)
endif ()
add_subdirectory(screen)
add_subdirectory(newtek)
add_subdirectory(artnet)

if (ENABLE_HTML)
	add_subdirectory(html)
endif ()

if (MSVC)
	add_subdirectory(flash)
	add_subdirectory(bluefish)
endif()

