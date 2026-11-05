# Imagen de PRUEBA: compila el árbol en Ubuntu 24.04 (lo que corren mosaic1/2/4/5) y ejecuta
# tools/test/replay_test.py dentro del contenedor. No es la imagen de producción
# (tools/linux/Dockerfile), que parte de jammy y cuyo FFmpeg 4.4 no tiene la API ch_layout que
# ya usa el módulo ffmpeg de este árbol.
#
#   docker build -f tools/test/Dockerfile -t casparcg-replay-test .
#
#   # GPU NVIDIA (nvidia-container-toolkit):
#   docker run --rm --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all casparcg-replay-test
#
#   # WSL2, a través del driver D3D12 de Mesa:
#   docker run --rm --device /dev/dxg -v /usr/lib/wsl:/usr/lib/wsl:ro \
#       -e LD_LIBRARY_PATH=/usr/lib/wsl/lib -e GALLIUM_DRIVER=d3d12 casparcg-replay-test
#
# Sale con 0 solo si pasan todas las comprobaciones.

FROM docker.io/buildpack-deps:noble AS build
    ADD tools/linux/install-dependencies /
    RUN apt-get update && sed -i 's/\r$//' /install-dependencies && bash /install-dependencies

    COPY ./src /source
    WORKDIR /build
    RUN cmake -GNinja /source -DCMAKE_BUILD_TYPE=Release -DENABLE_HTML=OFF \
              -DUSE_STATIC_BOOST=OFF -DUSE_SYSTEM_CEF=OFF -DBUILD_REPLAY=ON \
     && cmake --build . --parallel "$(nproc)"

FROM build AS test
    RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            ffmpeg libgl1-mesa-dri libegl-mesa0 libglx-mesa0 mesa-utils \
     && rm -rf /var/lib/apt/lists/*
    COPY tools/test/replay_test.py /test/replay_test.py
    # Sin X: EGL sobre la plataforma surfaceless de Mesa.
    ENV EGL_PLATFORM=surfaceless
    WORKDIR /test
    CMD ["python3", "-B", "/test/replay_test.py", "--server", "/build/shell/casparcg", "--workdir", "/test/work"]
