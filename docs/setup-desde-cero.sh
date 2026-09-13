#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# setup-desde-cero.sh  —  Configuración de nodo mosaico desde Ubuntu 24.04 limpio
#
# GPU objetivo : NVIDIA Quadro P1000 (Pascal · 640 CUDA · 4 GB · 1 NVENC engine)
# Usuario      : titania
# Directorio   : /home/titania/mosaic/casparcg/
#
# Uso:
#   ./setup-desde-cero.sh fase1    # Instala driver NVIDIA + container-toolkit
#   ./setup-desde-cero.sh fase2    # Analiza hardware y estima capacidad de señales
#   ./setup-desde-cero.sh fase3    # Configura el sistema (deps, Docker, kernel...)
#   ./setup-desde-cero.sh fase4b   # Instala CasparCG oficial stable (alternativa al fork)
#   ./setup-desde-cero.sh check    # Checklist de verificación final
#   ./setup-desde-cero.sh          # Muestra ayuda
#
# Documentación completa: SETUP-DESDE-CERO.md
# ═══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ── COLORES ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${CYAN}  ▸${NC} $*"; }
warn() { echo -e "${YELLOW}  ⚠${NC} $*"; }
err()  { echo -e "${RED}  ✗${NC} $*"; }
hdr()  { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

# ── CONSTANTES ────────────────────────────────────────────────────────────────
TITANIA_HOME="/home/titania"
MOSAIC_DIR="${TITANIA_HOME}/mosaic/casparcg"
REPORT_FILE="/tmp/mosaic-hardware-report.txt"
NVIDIA_DRIVER_VERSION="580"      # rama de driver confirmada en mosaic1 (2026-07-17)
NVIDIA_DRIVER_EXACT="580.159.03" # punto de versión exacto: disponible en el repo de Ubuntu Y
                                  # compatible con nvidia-patch — confirmado en mosaic1 (2026-07-17)
                                  # con el test de 16 sesiones NVENC, incluso tras reboot.
                                  # NOTA: mosaic2 (producción) usa 535.288.01 — rama distinta, no
                                  # tocar, sigue funcionando. Esta es la referencia para nodos NUEVOS
                                  # (mosaic1, mosaic4...). 535.288.01 YA NO está en el repo de Ubuntu
                                  # (confirmado 2026-07-17) — no usarla como objetivo en una instalación
                                  # nueva, es un callejón sin salida que obliga al .run manual.
                                  #
                                  # ⚠️ GOTCHA (mosaic1, 2026-07-17): si "apt-cache madison" no ofrece la
                                  # versión exacta, apt puede instalar OTRA rama en su lugar (ubuntu-drivers
                                  # decide "la recomendada") dejando dos metapaquetes nvidia-driver-XXX
                                  # instalados a la vez. Al limpiar el que sobra con
                                  # "apt purge + autoremove", autoremove puede arrastrar también paquetes
                                  # compartidos de la rama que SÍ quieres conservar (nvidia-compute-utils-*,
                                  # nvidia-dkms-*), rompiendo nvidia-smi y el rebuild DKMS del módulo activo.
                                  # Si pasa: "apt-get install --reinstall nvidia-driver-<rama-activa>"
                                  # inmediatamente después del purge, ANTES de reiniciar.

CASPARCG_STABLE_VERSION="2.5.0.stable"     # release oficial verificada en mosaic1/mosaic2 (2026-07-27)
CASPARCG_CEF_VERSION="142.0.17.g60aac24+2" # versión de CEF que acompaña a esa release
CASPARCG_RELEASE_TAG="v2.5.0-stable"       # tag de GitHub del que se descargan los .deb

# ══════════════════════════════════════════════════════════════════════════════
#  FASE 1: DRIVER NVIDIA
# ══════════════════════════════════════════════════════════════════════════════

fase1_nvidia() {
  hdr "FASE 1 — Instalación driver NVIDIA Quadro P1000"

  # 1.1 Detectar GPU
  # IMPORTANTE: NO usar "lspci | grep -i nvidia" — confirmado roto en mosaic1 y
  # mosaic4 (2026-07-17): la base de datos pci.ids no resuelve el vendor ID hex
  # a texto en estos nodos, así que el grep de texto no encuentra nada aunque la
  # tarjeta esté presente y nvidia-smi la vea sin problema. Se usa nvidia-smi
  # primero (si el driver ya está cargado) y, si no, se filtra lspci por el ID
  # de fabricante de NVIDIA (10de) en vez de por nombre.
  info "Detectando GPU NVIDIA..."
  local GPU_INFO=""
  if command -v nvidia-smi &>/dev/null; then
    GPU_INFO=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
  fi
  if [ -z "$GPU_INFO" ] && command -v lspci &>/dev/null; then
    GPU_INFO=$(lspci -d 10de: 2>/dev/null | head -1)
  fi
  if [ -z "$GPU_INFO" ]; then
    err "No se detectó ninguna GPU NVIDIA (ni por nvidia-smi ni por 'lspci -d 10de:')."
    err "Verificar que la tarjeta está bien asentada en el slot PCIe."
    exit 1
  fi
  ok "GPU detectada: ${GPU_INFO}"

  # 1.2 Verificar que no está ya instalado
  if command -v nvidia-smi &>/dev/null; then
    warn "nvidia-smi ya está disponible. ¿Reinstalar driver?"
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null \
      | while IFS=',' read -r NAME DRV; do
          ok "GPU: ${NAME} | Driver: ${DRV}"
        done
    read -r -p "  ¿Continuar igualmente con la instalación? [s/N] " RESP
    [[ "${RESP,,}" != "s" ]] && { info "Instalación de driver cancelada."; return 0; }
  fi

  # 1.3 Secure Boot check
  info "Verificando Secure Boot..."
  if command -v mokutil &>/dev/null; then
    SB_STATE=$(mokutil --sb-state 2>/dev/null || echo "unknown")
    if echo "$SB_STATE" | grep -qi "enabled"; then
      warn "Secure Boot está ACTIVADO. El módulo NVIDIA puede no cargar."
      warn "Solución: desactivar Secure Boot en BIOS/UEFI antes de continuar."
      read -r -p "  ¿Continuar de todas formas? [s/N] " RESP
      [[ "${RESP,,}" != "s" ]] && exit 1
    else
      ok "Secure Boot: ${SB_STATE}"
    fi
  fi

  # 1.4 Instalar ubuntu-drivers y el driver 550
  info "Actualizando apt e instalando ubuntu-drivers-common..."
  sudo apt-get update -qq
  sudo apt-get install -y ubuntu-drivers-common

  info "Buscando la revisión exacta de nvidia-driver-${NVIDIA_DRIVER_VERSION} que contiene ${NVIDIA_DRIVER_EXACT}..."
  local PKG_VER
  # "|| true" al final: si grep no encuentra ${NVIDIA_DRIVER_EXACT} (versión ya
  # purgada del repo de Ubuntu), grep devuelve 1 y con "set -o pipefail" eso
  # mataría el script entero en silencio (confirmado en mosaic1, 2026-07-17).
  # Con "|| true" cae correctamente en la rama "else" de más abajo.
  PKG_VER=$(apt-cache madison "nvidia-driver-${NVIDIA_DRIVER_VERSION}" 2>/dev/null \
    | awk '{print $3}' | grep "${NVIDIA_DRIVER_EXACT}" | head -1 || true)

  if [ -n "$PKG_VER" ]; then
    info "Instalando nvidia-driver-${NVIDIA_DRIVER_VERSION}=${PKG_VER} (fijado a ${NVIDIA_DRIVER_EXACT})..."
    sudo apt-get install -y --allow-downgrades "nvidia-driver-${NVIDIA_DRIVER_VERSION}=${PKG_VER}"
    sudo apt-mark hold "nvidia-driver-${NVIDIA_DRIVER_VERSION}"
    ok "Driver fijado en ${PKG_VER} y con 'apt-mark hold' (un 'apt upgrade' futuro no lo subirá)."
  else
    warn "${NVIDIA_DRIVER_EXACT} ya no está disponible en el repositorio de Ubuntu."
    warn "Instalando la última disponible de la rama ${NVIDIA_DRIVER_VERSION} — puede requerir el"
    warn "instalador .run oficial de NVIDIA para fijar ${NVIDIA_DRIVER_EXACT} exactamente."
    sudo ubuntu-drivers install "nvidia:${NVIDIA_DRIVER_VERSION}" \
      || sudo apt-get install -y "nvidia-driver-${NVIDIA_DRIVER_VERSION}"
  fi

  # 1.5 nvidia-container-toolkit
  hdr "Instalando nvidia-container-toolkit"

  if ! dpkg -l nvidia-container-toolkit &>/dev/null; then
    info "Añadiendo repositorio NVIDIA Container Toolkit..."
    curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
      | sudo gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg

    curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
      | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
      | sudo tee /etc/apt/sources.list.d/nvidia-container-toolkit.list > /dev/null

    sudo apt-get update -qq
    sudo apt-get install -y nvidia-container-toolkit
    ok "nvidia-container-toolkit instalado."
  else
    ok "nvidia-container-toolkit ya instalado."
  fi

  # Configurar Docker para GPU (si Docker ya está instalado)
  if command -v docker &>/dev/null; then
    info "Configurando runtime NVIDIA en Docker..."
    sudo nvidia-ctk runtime configure --runtime=docker
    sudo systemctl restart docker
    ok "Docker configurado con runtime NVIDIA."
  else
    warn "Docker no está instalado todavía. Ejecutar fase3 y luego reconfigurará Docker."
    warn "Cuando Docker esté instalado: sudo nvidia-ctk runtime configure --runtime=docker"
  fi

  echo ""
  echo -e "${YELLOW}╔══════════════════════════════════════════════════════╗${NC}"
  echo -e "${YELLOW}║  REINICIAR EL SISTEMA para activar el driver NVIDIA  ║${NC}"
  echo -e "${YELLOW}║  Después: nvidia-smi  para verificar                 ║${NC}"
  echo -e "${YELLOW}╚══════════════════════════════════════════════════════╝${NC}"
  echo ""
  read -r -p "  ¿Reiniciar ahora? [s/N] " RESP
  [[ "${RESP,,}" == "s" ]] && sudo reboot
}

# ══════════════════════════════════════════════════════════════════════════════
#  FASE 1B: PARCHE DE LÍMITE DE SESIONES NVENC (ejecutar DESPUÉS de reiniciar en fase1)
# ══════════════════════════════════════════════════════════════════════════════
#
# Confirmado en mosaic1 el 2026-07-17: las Quadro/GeForce "de consumo" (P1000,
# M2000, P2000...) SÍ tienen un límite de fábrica de sesiones NVENC concurrentes
# — falla con "OpenEncodeSessionEx failed: out of memory (10)" a partir de la
# 4ª-5ª sesión simultánea. Sin parchear, cualquier mosaico con más de 3-4 señales
# y USE_GPU=true falla en el encoder de salida o en las señales de entrada.

fase1b_nvenc_patch() {
  hdr "FASE 1B — Parche de límite de sesiones NVENC"

  if ! command -v nvidia-smi &>/dev/null; then
    err "nvidia-smi no disponible. Ejecutar fase1 y reiniciar antes de esta fase."
    exit 1
  fi

  local DRV_VER
  DRV_VER=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
  info "Driver activo: ${DRV_VER}"
  if [[ "${DRV_VER}" != "${NVIDIA_DRIVER_EXACT}" ]]; then
    warn "El driver activo (${DRV_VER}) no es ${NVIDIA_DRIVER_EXACT}."
    warn "nvidia-patch puede no tener soporte para esta versión — si falla más abajo,"
    warn "fijar el driver a ${NVIDIA_DRIVER_EXACT} (ver fase1) y repetir esta fase."
  fi

  local PATCH_DIR="${HOME}/nvidia-patch"
  if [ ! -d "$PATCH_DIR" ]; then
    info "Clonando nvidia-patch..."
    git clone https://github.com/keylase/nvidia-patch.git "$PATCH_DIR"
  else
    info "Actualizando clon existente de nvidia-patch (por si ya soporta esta versión)..."
    (cd "$PATCH_DIR" && git pull)
  fi

  info "Backup de libnvidia-encode.so.1 antes de parchear (si no existe ya)..."
  sudo cp -n /usr/lib/x86_64-linux-gnu/libnvidia-encode.so.1 \
             /usr/lib/x86_64-linux-gnu/libnvidia-encode.so.1.orig.bak 2>/dev/null || true

  info "Aplicando parche..."
  if ! (cd "$PATCH_DIR" && sudo bash ./patch.sh); then
    err "El driver ${DRV_VER} no está soportado por esta copia de nvidia-patch."
    err "Fijar el driver a ${NVIDIA_DRIVER_EXACT} (ver fase1) y repetir: ./setup-desde-cero.sh fase1b"
    exit 1
  fi

  hdr "Verificación: test aislado de sesiones NVENC concurrentes"

  if ! command -v docker &>/dev/null; then
    warn "Docker no está instalado todavía — no se puede verificar el límite de sesiones NVENC."
    warn "El parche SÍ se ha aplicado (patch.sh terminó sin error). Ejecuta 'fase3' para instalar"
    warn "Docker y luego repite: ./setup-desde-cero.sh fase1b para verificar las 16 sesiones."
    return 0
  fi

  info "Lanzando 16 sesiones NVENC de prueba (contenedores sueltos, no toca CasparCG/producción)..."
  local i
  for i in $(seq 1 16); do
    docker run -d --name "nvenc-verify-${i}" --gpus all jrottenberg/ffmpeg:6.1-nvidia \
      -f lavfi -i testsrc=size=640x360:rate=25 -c:v h264_nvenc -preset p2 -tune ll -g 25 -f null - \
      &>/dev/null
    sleep 0.3
  done
  sleep 5
  local SURVIVED
  SURVIVED=$(docker ps --filter "name=nvenc-verify-" --format "{{.Names}}" | wc -l)
  docker rm -f $(docker ps -aq --filter "name=nvenc-verify-") &>/dev/null

  if (( SURVIVED >= 16 )); then
    ok "Las 16 sesiones NVENC sobrevivieron. Límite de sesiones desbloqueado correctamente."
  else
    err "Solo sobrevivieron ${SURVIVED}/16 sesiones. El parche no se aplicó o no tuvo efecto."
    err "Revisar: md5sum de libnvidia-encode.so.1 contra el backup .orig.bak,"
    err "y probar 'sudo systemctl restart docker' antes de repetir el test."
  fi
}

# ══════════════════════════════════════════════════════════════════════════════
#  FASE 2: ANÁLISIS DE HARDWARE
# ══════════════════════════════════════════════════════════════════════════════

fase2_hardware() {
  hdr "FASE 2 — Análisis de hardware y viabilidad para mosaico"

  local -i SCORE=0       # puntuación acumulada (0-100)
  local -i WARN_COUNT=0
  local REPORT=""
  local MAX_SIGNALS=0

  append() { REPORT="${REPORT}\n$*"; }

  echo ""
  append "╔══════════════════════════════════════════════════════════════════════╗"
  append "║          INFORME DE HARDWARE — NODO MOSAICO CASPARCG                ║"
  append "║          $(date '+%Y-%m-%d %H:%M:%S')                              ║"
  append "╚══════════════════════════════════════════════════════════════════════╝"

  # ── CPU ──────────────────────────────────────────────────────────────────────
  append "\n── CPU ────────────────────────────────────────────────────────────────"
  local CPU_MODEL
  CPU_MODEL=$(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')
  local CPU_PHYS
  CPU_PHYS=$(grep "^physical id" /proc/cpuinfo | sort -u | wc -l)
  local CPU_CORES
  CPU_CORES=$(nproc --all)
  local CPU_MHZ
  CPU_MHZ=$(grep -m1 "cpu MHz" /proc/cpuinfo | cut -d: -f2 | sed 's/ //' | cut -d. -f1)
  local CPU_GHZ
  CPU_GHZ=$(echo "scale=2; ${CPU_MHZ:-0} / 1000" | bc 2>/dev/null || echo "?")

  append "  Modelo     : ${CPU_MODEL}"
  append "  Sockets    : ${CPU_PHYS}"
  append "  Cores tot. : ${CPU_CORES}"
  append "  Frec. base : ${CPU_GHZ} GHz"

  # Verificar AVX2 (requerido por CEF 119)
  if grep -q "avx2" /proc/cpuinfo; then
    append "  AVX2       : ✓ PRESENTE (requerido por CEF 119)"
    SCORE=$((SCORE + 25))
  else
    append "  AVX2       : ✗ NO DISPONIBLE — CEF 119 no funcionará (SIGILL)"
    warn "CPU sin AVX2: CasparCG compilado con CEF 119 crasheará con SIGILL."
    WARN_COUNT=$((WARN_COUNT + 1))
  fi

  # Estimar capacidad por CPU
  if [[ "$CPU_CORES" =~ ^[0-9]+$ ]] && (( CPU_CORES >= 4 )); then
    # Reservar 6 cores para CasparCG+SO, resto para FFmpeg dockers
    local -i FREE_CORES=$(( CPU_CORES - 6 ))
    (( FREE_CORES < 0 )) && FREE_CORES=0
    # GPU NVENC: ~0.2 cores/señal (solo demux/mux en CPU)
    MAX_SIGNALS=$(( FREE_CORES * 5 ))  # 0.2 cores/señal → ×5
    (( MAX_SIGNALS > 32 )) && MAX_SIGNALS=32  # límite práctico por CasparCG
    append "  Cap. CPU   : ~${MAX_SIGNALS} señales máx. (GPU NVENC)"

    if (( CPU_CORES >= 16 )); then
      SCORE=$((SCORE + 25)); append "  Rating CPU : ★★★ Excelente (≥ 16 cores)"
    elif (( CPU_CORES >= 8 )); then
      SCORE=$((SCORE + 20)); append "  Rating CPU : ★★☆ Bueno (8-15 cores)"
    elif (( CPU_CORES >= 4 )); then
      SCORE=$((SCORE + 10)); append "  Rating CPU : ★☆☆ Mínimo (4-7 cores, ≤ 8 señales)"
      warn "CPU con menos de 8 cores: rendimiento limitado para mosaicos grandes."
      WARN_COUNT=$((WARN_COUNT + 1))
    fi
  fi

  # ── RAM ──────────────────────────────────────────────────────────────────────
  append "\n── MEMORIA RAM ────────────────────────────────────────────────────────"
  local RAM_TOTAL_KB
  RAM_TOTAL_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
  local RAM_GB=$(( RAM_TOTAL_KB / 1024 / 1024 ))
  local RAM_FREE_GB=$(( $(grep MemAvailable /proc/meminfo | awk '{print $2}') / 1024 / 1024 ))
  append "  RAM total  : ${RAM_GB} GB"
  append "  RAM libre  : ${RAM_FREE_GB} GB"

  if (( RAM_GB >= 32 )); then
    SCORE=$((SCORE + 20)); append "  Rating RAM : ★★★ Excelente (≥ 32 GB)"
  elif (( RAM_GB >= 16 )); then
    SCORE=$((SCORE + 15)); append "  Rating RAM : ★★☆ Correcto (16-31 GB)"
  elif (( RAM_GB >= 8 )); then
    SCORE=$((SCORE + 5));  append "  Rating RAM : ★☆☆ Mínimo (8-15 GB)"
    warn "RAM < 16 GB: posibles problemas con mosaicos de 20 señales."
    WARN_COUNT=$((WARN_COUNT + 1))
  else
    append "  Rating RAM : ✗ INSUFICIENTE (< 8 GB)"
    err "RAM insuficiente. Mínimo 8 GB, recomendado 16-32 GB."
    WARN_COUNT=$((WARN_COUNT + 1))
  fi

  # ── GPU ──────────────────────────────────────────────────────────────────────
  append "\n── GPU ────────────────────────────────────────────────────────────────"
  local GPU_PRESENT=false
  local GPU_NVENC=false

  # IMPORTANTE: se comprueba nvidia-smi PRIMERO, no lspci. Confirmado en mosaic4
  # (2026-07-17) que "lspci | grep -i nvidia" puede no encontrar nada (base de datos
  # pci.ids sin resolver el ID hex a texto) aunque nvidia-smi vea la GPU perfectamente
  # y el driver funcione. nvidia-smi es la fuente de verdad real; lspci es solo dato
  # complementario para cuando el driver todavía no está instalado.
  if command -v nvidia-smi &>/dev/null && nvidia-smi --query-gpu=name --format=csv,noheader &>/dev/null; then
    GPU_PRESENT=true
    local GPU_NAME GPU_DRV GPU_VRAM GPU_TEMP GPU_UTIL
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || echo "?")
    GPU_DRV=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1 || echo "?")
    GPU_VRAM=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader 2>/dev/null | head -1 || echo "?")
    GPU_TEMP=$(nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader 2>/dev/null | head -1 || echo "?")
    GPU_UTIL=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader 2>/dev/null | head -1 || echo "?")

    append "  GPU name   : ${GPU_NAME}"
    append "  Driver     : ${GPU_DRV}"
    append "  VRAM       : ${GPU_VRAM}"
    append "  Temp.      : ${GPU_TEMP} °C"
    append "  Uso GPU    : ${GPU_UTIL}"

    if [[ "${GPU_DRV}" != "${NVIDIA_DRIVER_EXACT}" ]]; then
      append "  ⚠ Driver distinto de la referencia ${NVIDIA_DRIVER_EXACT} (mosaic2) — confirmar"
      append "    compatibilidad con nvidia-patch antes de asumir sesiones NVENC ilimitadas."
      warn "Driver ${GPU_DRV} distinto de ${NVIDIA_DRIVER_EXACT}. Ejecutar fase1b para verificar el límite de sesiones NVENC."
      WARN_COUNT=$((WARN_COUNT + 1))
    fi

    # NOTA: las Quadro/GeForce de consumo SÍ tienen límite de fábrica de sesiones
    # NVENC concurrentes (confirmado en mosaic1, 2026-07-17) — "NVENC disponible"
    # no implica "sin límite". Verificar siempre con: ./setup-desde-cero.sh fase1b
    GPU_NVENC=true
    append "  NVENC      : ✓ DISPONIBLE (verificar límite de sesiones con: ./setup-desde-cero.sh fase1b)"
    SCORE=$((SCORE + 20))
    append "  Rating GPU : ★★★ NVENC disponible (codificación HW)"
  elif command -v lspci &>/dev/null && lspci -d 10de: 2>/dev/null | grep -q .; then
    warn "GPU NVIDIA detectada por lspci (vendor 10de) pero nvidia-smi no responde. Ejecutar fase1 y reiniciar."
    append "  GPU NVIDIA : detectada por lspci, driver no operativo — ejecutar fase1"
    append "  NVENC      : DESCONOCIDO hasta instalar/cargar el driver"
    WARN_COUNT=$((WARN_COUNT + 1))
  else
    append "  GPU NVIDIA : NO DETECTADA (ni por nvidia-smi ni por lspci)"
    warn "Sin GPU NVIDIA: el sistema usará CPU (libx264). Rendimiento muy reducido."
    WARN_COUNT=$((WARN_COUNT + 1))
  fi

  # ── KERNEL / OS ──────────────────────────────────────────────────────────────
  append "\n── SISTEMA OPERATIVO ──────────────────────────────────────────────────"
  local OS_INFO KERNEL_VER
  OS_INFO=$(grep PRETTY_NAME /etc/os-release | cut -d'"' -f2)
  KERNEL_VER=$(uname -r)
  append "  OS         : ${OS_INFO}"
  append "  Kernel     : ${KERNEL_VER}"

  if echo "$OS_INFO" | grep -q "24.04"; then
    SCORE=$((SCORE + 5)); append "  Rating OS  : ✓ Ubuntu 24.04 LTS (recomendado)"
  else
    warn "SO no es Ubuntu 24.04 LTS. La compatibilidad no está garantizada."
    WARN_COUNT=$((WARN_COUNT + 1))
  fi

  # Comprobar buffer UDP
  append "\n── RED / UDP BUFFERS ───────────────────────────────────────────────────"
  local RMQ
  RMQ=$(sysctl -n net.core.rmem_max 2>/dev/null || echo "0")
  append "  rmem_max   : ${RMQ}  (mínimo requerido: 67108864)"
  if (( RMQ >= 67108864 )); then
    SCORE=$((SCORE + 5)); append "  Buffer UDP : ✓ Correcto"
  else
    append "  Buffer UDP : ✗ INSUFICIENTE — ejecutar fase3 para corregir"
    WARN_COUNT=$((WARN_COUNT + 1))
  fi

  # NICs y multicast
  append "\n── INTERFACES DE RED ───────────────────────────────────────────────────"
  local NIC_COUNT
  NIC_COUNT=$(ip link show | grep "^[0-9]" | grep -v "lo:" | wc -l)
  append "  NICs       : ${NIC_COUNT} (excl. loopback)"
  ip link show | grep -v "lo:" | grep "^[0-9]" | awk '{print "  "$2}' \
    | while read -r NIC; do
        local IP
        IP=$(ip addr show "${NIC%:}" 2>/dev/null | grep "inet " | awk '{print $2}' | head -1)
        append "    ${NIC}: ${IP:-sin IP asignada}"
        echo -e "    ${NIC}: ${IP:-sin IP asignada}"
      done

  # ── ALMACENAMIENTO ────────────────────────────────────────────────────────────
  append "\n── ALMACENAMIENTO ─────────────────────────────────────────────────────"
  df -h / | tail -1 | awk '{printf "  Raíz  / :  total=%s  usado=%s  libre=%s\n", $2, $3, $4}' \
    | while read -r LINE; do append "  ${LINE}"; done
  df -h /home | tail -1 | awk '{printf "  /home   :  total=%s  usado=%s  libre=%s\n", $2, $3, $4}' \
    | while read -r LINE; do append "  ${LINE}"; done 2>/dev/null || true

  # ── RESUMEN DE VIABILIDAD ─────────────────────────────────────────────────────
  append "\n══ RESUMEN DE VIABILIDAD ══════════════════════════════════════════════"

  local VIABILIDAD RECOMENDACION
  if (( SCORE >= 80 )); then
    VIABILIDAD="✓✓ EXCELENTE — Equipo apto para mosaico profesional"
  elif (( SCORE >= 60 )); then
    VIABILIDAD="✓  BUENO — Equipo apto con alguna limitación"
  elif (( SCORE >= 40 )); then
    VIABILIDAD="⚠  ACEPTABLE — Equipo con limitaciones relevantes"
  else
    VIABILIDAD="✗  INSUFICIENTE — Revisar hardware antes de continuar"
  fi

  # Estimar señales con GPU NVENC
  local SIGNALS_GPU SIGNALS_CPU
  if [[ "$CPU_CORES" =~ ^[0-9]+$ ]]; then
    local -i FREE=$(( CPU_CORES - 6 ))
    (( FREE < 0 )) && FREE=0
    SIGNALS_GPU=$(( FREE * 5 )); (( SIGNALS_GPU > 32 )) && SIGNALS_GPU=32
    SIGNALS_CPU=$(( FREE )); (( SIGNALS_CPU > 20 )) && SIGNALS_CPU=20
    # Ajustar por RAM
    (( RAM_GB < 16 )) && { SIGNALS_GPU=$(( SIGNALS_GPU / 2 )); SIGNALS_CPU=$(( SIGNALS_CPU / 2 )); }
  else
    SIGNALS_GPU="?"
    SIGNALS_CPU="?"
  fi

  append "  Puntuación   : ${SCORE}/100"
  append "  Viabilidad   : ${VIABILIDAD}"
  append "  Señales max. : ${SIGNALS_GPU} (con GPU NVENC) | ${SIGNALS_CPU} (sin GPU, libx264)"
  append "  Advertencias : ${WARN_COUNT}"
  append ""

  # Recomendación de mosaico
  if [[ "$SIGNALS_GPU" =~ ^[0-9]+$ ]]; then
    if (( SIGNALS_GPU >= 20 )); then
      append "  Mosaico recomendado: 5×4 (20 señales) — start-mosaic5x4.sh"
    elif (( SIGNALS_GPU >= 16 )); then
      append "  Mosaico recomendado: 4×4 (16 señales) — start-mosaic.sh"
    elif (( SIGNALS_GPU >= 12 )); then
      append "  Mosaico recomendado: 4×3 (12 señales)"
    else
      append "  Mosaico recomendado: 4×2 (8 señales) o menor"
    fi
  fi

  append "══════════════════════════════════════════════════════════════════════"

  # Mostrar y guardar informe
  echo -e "$REPORT"
  echo -e "$REPORT" > "$REPORT_FILE"
  info "Informe guardado en: ${REPORT_FILE}"

  if (( WARN_COUNT > 0 )); then
    echo ""
    warn "${WARN_COUNT} advertencia(s) encontradas. Revisar antes de continuar."
  fi
  if (( SCORE < 40 )); then
    err "Puntuación demasiado baja. Revisar el hardware antes de continuar con fase3."
    exit 1
  fi
}

# ══════════════════════════════════════════════════════════════════════════════
#  FASE 3: CONFIGURACIÓN DEL SISTEMA
# ══════════════════════════════════════════════════════════════════════════════

fase3_sistema() {
  hdr "FASE 3 — Configuración del sistema"
  echo ""
  warn "Esta fase realiza cambios en el sistema. Se requieren permisos sudo."
  read -r -p "  ¿Continuar? [s/N] " RESP
  [[ "${RESP,,}" != "s" ]] && { info "Cancelado."; exit 0; }

  # 3.1 Crear usuario titania
  hdr "3.1 — Usuario titania"
  if id titania &>/dev/null; then
    ok "Usuario titania ya existe."
  else
    info "Creando usuario titania..."
    sudo useradd -m -s /bin/bash titania
    ok "Usuario titania creado."
  fi
  sudo usermod -aG docker,video,audio titania 2>/dev/null || true
  ok "Grupos asignados: docker, video, audio"

  # sudoers completo para titania (acceso total sin contraseña — inseguro, pero
  # es la política actual pedida para este usuario mientras el equipo esté así).
  local SUDOERS_LINE="titania ALL=(ALL) NOPASSWD:ALL"
  if [ -f /etc/sudoers.d/titania-mosaic ] && sudo grep -qF "$SUDOERS_LINE" /etc/sudoers.d/titania-mosaic; then
    ok "Sudoers titania ya configurado (NOPASSWD:ALL)."
  else
    echo "$SUDOERS_LINE" | sudo tee /etc/sudoers.d/titania-mosaic > /dev/null
    sudo chmod 440 /etc/sudoers.d/titania-mosaic
    if sudo visudo -c -f /etc/sudoers.d/titania-mosaic &>/dev/null; then
      ok "Sudoers configurado para titania (NOPASSWD:ALL)."
    else
      err "Sintaxis inválida en /etc/sudoers.d/titania-mosaic — revisar a mano antes de continuar."
      exit 1
    fi
  fi

  # 3.2 Acceso SSH por password
  hdr "3.2 — Acceso SSH por password"
  local SSHD_CONFIG="/etc/ssh/sshd_config"
  if bash -c 'sudo sshd -T 2>/dev/null | grep -qi "^passwordauthentication yes"'; then
    ok "PasswordAuthentication ya está activado (sshd -T)."
  else
    sudo cp "$SSHD_CONFIG" "${SSHD_CONFIG}.bak-$(date +%Y%m%d%H%M%S)"
    if grep -qE "^\s*#?\s*PasswordAuthentication\b" "$SSHD_CONFIG"; then
      sudo sed -i -E 's/^\s*#?\s*PasswordAuthentication\s+.*/PasswordAuthentication yes/' "$SSHD_CONFIG"
    else
      echo "PasswordAuthentication yes" | sudo tee -a "$SSHD_CONFIG" > /dev/null
    fi
    # Ubuntu incluye sshd_config.d/*, que se lee DESPUÉS de sshd_config y puede
    # pisar el valor de arriba si algún drop-in fuerza "no".
    if [ -d /etc/ssh/sshd_config.d ]; then
      sudo grep -lE "^\s*PasswordAuthentication\s+no" /etc/ssh/sshd_config.d/*.conf 2>/dev/null \
        | while read -r F; do
            sudo sed -i -E 's/^\s*PasswordAuthentication\s+no/PasswordAuthentication yes/' "$F"
            warn "Corregido PasswordAuthentication en ${F} (pisaba el valor de sshd_config)."
          done || true
    fi
    if ! sudo sshd -t; then
      err "sshd_config quedó con sintaxis inválida — restaurando backup."
      sudo cp "${SSHD_CONFIG}.bak-"* "$SSHD_CONFIG" 2>/dev/null || true
      exit 1
    fi
    sudo systemctl reload ssh 2>/dev/null || sudo systemctl reload sshd 2>/dev/null || true
    ok "PasswordAuthentication activado en sshd_config."
  fi

  # 3.3 Dependencias del sistema
  hdr "3.3 — Dependencias del sistema"
  info "Actualizando apt..."
  sudo apt-get update -qq
  sudo apt-get upgrade -y -qq

  info "Instalando dependencias de compilación y runtime..."
  sudo apt-get install -y --no-install-recommends \
    autoconf automake cmake ninja-build curl bzip2 wget \
    clang g++ gcc git gperf libtool make perl pkg-config \
    python3 python3-pip zlib1g-dev libexpat1-dev lsb-release \
    libglew-dev libtbb-dev libopenal-dev \
    libxcursor-dev libxinerama-dev libxi-dev \
    libsfml-dev libxrandr-dev libudev-dev \
    libglu1-mesa-dev libgl1-mesa-dev libegl1-mesa-dev \
    libboost-all-dev \
    libnss3-dev libcups2-dev libxdamage-dev \
    libxcomposite-dev libatk1.0-dev libatspi2.0-dev \
    libatk-bridge2.0-dev \
    libavcodec-dev libavformat-dev libavdevice-dev \
    libavutil-dev libavfilter-dev \
    libswscale-dev libswresample-dev \
    libsimde-dev \
    netcat-openbsd patchelf bc jq
  ok "Dependencias de compilación instaladas."

  # Node.js 20 LTS
  if ! command -v node &>/dev/null || ! node --version | grep -q "^v20"; then
    info "Instalando Node.js 20 LTS..."
    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
    sudo apt-get install -y nodejs
    ok "Node.js $(node --version) instalado."
  else
    ok "Node.js $(node --version) ya instalado."
  fi

  # 3.4 Buffer UDP del kernel (crítico para multicast)
  hdr "3.4 — Buffer UDP del kernel"
  local RMQ
  RMQ=$(sysctl -n net.core.rmem_max)
  if (( RMQ < 67108864 )); then
    info "Ajustando buffers UDP del kernel..."
    sudo sysctl -w net.core.rmem_max=67108864
    sudo sysctl -w net.core.rmem_default=67108864
  fi

  grep -q "rmem_max" /etc/sysctl.conf || \
    echo "net.core.rmem_max=67108864" | sudo tee -a /etc/sysctl.conf > /dev/null
  grep -q "rmem_default" /etc/sysctl.conf || \
    echo "net.core.rmem_default=67108864" | sudo tee -a /etc/sysctl.conf > /dev/null

  # RP filter para multicast
  grep -q "rp_filter=2" /etc/sysctl.conf || {
    echo "net.ipv4.conf.all.rp_filter=2" | sudo tee -a /etc/sysctl.conf > /dev/null
    echo "net.ipv4.conf.default.rp_filter=2" | sudo tee -a /etc/sysctl.conf > /dev/null
  }
  sudo sysctl -p > /dev/null
  ok "Buffers UDP: rmem_max=$(sysctl -n net.core.rmem_max)"

  # 3.5 Docker (repositorio oficial — docs.docker.com/engine/install/ubuntu)
  hdr "3.5 — Docker Engine (repositorio oficial)"
  if command -v docker &>/dev/null && dpkg -l docker-ce &>/dev/null; then
    ok "Docker ya instalado: $(docker --version)"
  else
    info "Instalando Docker Engine desde el repositorio oficial..."
    # Paquetes viejos en conflicto (no-op en un equipo recién formateado)
    for PKG in docker.io docker-doc docker-compose podman-docker containerd runc; do
      sudo apt-get remove -y "$PKG" 2>/dev/null || true
    done

    sudo apt-get update -qq
    sudo apt-get install -y ca-certificates curl
    sudo install -m 0755 -d /etc/apt/keyrings
    sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
    sudo chmod a+r /etc/apt/keyrings/docker.asc

    local DOCKER_ARCH DOCKER_CODENAME
    DOCKER_ARCH=$(dpkg --print-architecture)
    DOCKER_CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME")
    echo "deb [arch=${DOCKER_ARCH} signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu ${DOCKER_CODENAME} stable" \
      | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

    sudo apt-get update -qq
    sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
    ok "Docker instalado: $(docker --version)"
  fi

  if docker compose version &>/dev/null; then
    ok "Docker Compose plugin: $(docker compose version 2>/dev/null | head -1)"
  else
    err "Docker Compose plugin no disponible tras la instalación — revisar manualmente."
  fi

  sudo usermod -aG docker titania 2>/dev/null || true

  # Configurar runtime NVIDIA si el driver está instalado
  if command -v nvidia-ctk &>/dev/null && command -v nvidia-smi &>/dev/null; then
    info "Configurando runtime NVIDIA en Docker..."
    sudo nvidia-ctk runtime configure --runtime=docker
    sudo systemctl restart docker
    ok "Runtime NVIDIA configurado en Docker."
  fi

  # Descargar imágenes FFmpeg
  info "Descargando imagen jrottenberg/ffmpeg:6.1-ubuntu..."
  docker pull jrottenberg/ffmpeg:6.1-ubuntu
  info "Descargando imagen jrottenberg/ffmpeg:6.1-nvidia..."
  docker pull jrottenberg/ffmpeg:6.1-nvidia || \
    warn "No se pudo descargar imagen nvidia (normal si el driver no está listo)."
  ok "Imágenes FFmpeg descargadas."

  # 3.6 docker login registry.overon.es
  hdr "3.6 — docker login registry.overon.es"
  if sudo -u titania bash -c 'test -f ~/.docker/config.json && grep -q registry.overon.es ~/.docker/config.json' 2>/dev/null; then
    ok "titania ya tiene sesión guardada en registry.overon.es."
  else
    echo ""
    warn "Credenciales de registry.overon.es (usuario/contraseña en Teampass)."
    read -r -p "  Usuario del registry [Intro para omitir]: " REGISTRY_USER
    if [ -n "$REGISTRY_USER" ]; then
      read -r -s -p "  Password del registry: " REGISTRY_PASS
      echo ""
      if echo "$REGISTRY_PASS" | sudo -u titania docker login registry.overon.es -u "$REGISTRY_USER" --password-stdin; then
        ok "docker login correcto en registry.overon.es (usuario titania)."
      else
        err "docker login falló — revisar usuario/contraseña e intentar de nuevo a mano:"
        err "  sudo -u titania docker login registry.overon.es"
      fi
      unset REGISTRY_PASS
    else
      warn "Omitido. Ejecutar más tarde: sudo -u titania docker login registry.overon.es"
    fi
  fi

  # 3.7 Usuario noc en grupo docker
  hdr "3.7 — Usuario noc en grupo docker"
  if id noc &>/dev/null; then
    ok "Usuario noc ya existe."
  else
    warn "Usuario noc no existe en este equipo."
    read -r -p "  ¿Crear el usuario noc ahora? [s/N] " RESP
    if [[ "${RESP,,}" == "s" ]]; then
      sudo useradd -m -s /bin/bash noc
      ok "Usuario noc creado."
    else
      warn "Omitido — usuario noc no creado, no se puede añadir al grupo docker."
    fi
  fi
  if id noc &>/dev/null; then
    sudo usermod -aG docker noc
    ok "Usuario noc añadido al grupo docker (no necesita elevar permisos para Docker)."
  fi

  # 3.8 Demonio SNMP (comunidad de lectura public-titania)
  hdr "3.8 — Demonio SNMP"
  if ! command -v snmpd &>/dev/null; then
    info "Instalando snmpd..."
    sudo apt-get install -y snmpd snmp
  fi

  local SNMPD_CONF="/etc/snmp/snmpd.conf"
  if sudo grep -qE '^\s*rocommunity\s+public-titania\b' "$SNMPD_CONF" 2>/dev/null; then
    ok "snmpd ya tiene la comunidad de lectura public-titania."
  else
    sudo cp -n "$SNMPD_CONF" "${SNMPD_CONF}.orig.bak" 2>/dev/null || true
    echo "rocommunity public-titania" | sudo tee -a "$SNMPD_CONF" > /dev/null
    sudo systemctl enable snmpd
    sudo systemctl restart snmpd
    ok "snmpd configurado con comunidad de lectura public-titania."
  fi
  warn "El snmpd.conf de Ubuntu por defecto solo escucha en 127.0.0.1 ('agentAddress')."
  warn "Si el resto de instancias escucha en todas las interfaces, ajusta esa línea a mano en"
  warn "${SNMPD_CONF} — no se toca aquí por no asumir la política de red del resto de nodos."

  # 3.9 Estructura de directorios
  hdr "3.9 — Estructura de directorios"
  sudo -u titania bash << 'EOF'
mkdir -p ~/mosaic/casparcg/{bin,media,log,data,templates,thumbnails,font}
mkdir -p ~/mosaic/casparcg/templates/{vumeter,warning-restart,signal-info,Mosaic5x4,teletext}
mkdir -p ~/mosaic/server
EOF
  ok "Directorios creados en ${MOSAIC_DIR}"

  # 3.10 Servicio systemd CasparCG
  hdr "3.10 — Servicio systemd casparcg"
  if [ ! -f /etc/systemd/system/casparcg.service ]; then
    sudo tee /etc/systemd/system/casparcg.service > /dev/null << 'SVCEOF'
[Unit]
Description=CasparCG Server
After=network.target

[Service]
Type=simple
User=titania
WorkingDirectory=/home/titania/mosaic/casparcg
ExecStart=/home/titania/mosaic/casparcg/bin/casparcg
Restart=on-failure
RestartSec=5
LimitCORE=infinity
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
SVCEOF
    sudo systemctl daemon-reload
    sudo systemctl enable casparcg
    ok "Servicio casparcg.service creado y habilitado."
  else
    ok "Servicio casparcg.service ya existe."
  fi

  # 3.11 Servicio systemd osc-bridge
  hdr "3.11 — Servicio systemd osc-bridge"
  if [ ! -f /etc/systemd/system/osc-bridge.service ]; then
    sudo tee /etc/systemd/system/osc-bridge.service > /dev/null << 'SVCEOF'
[Unit]
Description=OSC Bridge para VU-meters CasparCG
After=network.target casparcg.service

[Service]
Type=simple
User=titania
WorkingDirectory=/home/titania/mosaic/casparcg/templates/vumeter
ExecStart=/usr/bin/node osc-bridge.js
Restart=on-failure
RestartSec=5
StandardOutput=append:/home/titania/mosaic/casparcg/log/osc-bridge.log
StandardError=append:/home/titania/mosaic/casparcg/log/osc-bridge.log

[Install]
WantedBy=multi-user.target
SVCEOF
    sudo systemctl daemon-reload
    sudo systemctl enable osc-bridge
    ok "Servicio osc-bridge.service creado y habilitado."
  else
    ok "Servicio osc-bridge.service ya existe."
  fi

  # 3.12 Servicio systemd teletext-bridge
  hdr "3.12 — Servicio systemd teletext-bridge"
  if [ ! -f /etc/systemd/system/teletext-bridge.service ]; then
    sudo tee /etc/systemd/system/teletext-bridge.service > /dev/null << 'SVCEOF'
[Unit]
Description=Teletext Bridge para subtítulos CasparCG
After=network.target casparcg.service

[Service]
Type=simple
User=titania
WorkingDirectory=/home/titania/mosaic/casparcg/templates/teletext
ExecStart=/usr/bin/node teletext-bridge.js
Restart=on-failure
RestartSec=5
StandardOutput=append:/home/titania/mosaic/casparcg/log/teletext-bridge.log
StandardError=append:/home/titania/mosaic/casparcg/log/teletext-bridge.log

[Install]
WantedBy=multi-user.target
SVCEOF
    sudo systemctl daemon-reload
    sudo systemctl enable teletext-bridge
    ok "Servicio teletext-bridge.service creado y habilitado."
  else
    ok "Servicio teletext-bridge.service ya existe."
  fi

  # 3.13 Límites de journald y coredumps
  hdr "3.13 — Límites de journald y coredumps"

  # Journal: tope permanente (evita que /var/log/journal crezca sin límite)
  if grep -q "^SystemMaxUse=1G$" /etc/systemd/journald.conf 2>/dev/null; then
    ok "journald ya tiene SystemMaxUse=1G"
  else
    if grep -q "^#\?SystemMaxUse=" /etc/systemd/journald.conf 2>/dev/null; then
      sudo sed -i 's/^#\?SystemMaxUse=.*/SystemMaxUse=1G/' /etc/systemd/journald.conf
    else
      echo "SystemMaxUse=1G" | sudo tee -a /etc/systemd/journald.conf > /dev/null
    fi
    sudo systemctl restart systemd-journald
    ok "journald limitado a SystemMaxUse=1G"
  fi

  # Coredumps: tope + margen de disco libre garantizado, en vez de "sin límite" por defecto.
  # Visto en mosaic4: un SIGILL recurrente de CasparCG sin este límite llegó a llenar el disco
  # al 99%, y encima algún coredump se perdió por falta de espacio justo cuando más hacía falta.
  if grep -q "^MaxUse=3G$" /etc/systemd/coredump.conf 2>/dev/null; then
    ok "systemd-coredump ya tiene MaxUse=3G/KeepFree=10G"
  else
    sudo tee -a /etc/systemd/coredump.conf > /dev/null <<'EOF'
MaxUse=3G
KeepFree=10G
EOF
    ok "systemd-coredump limitado a MaxUse=3G, KeepFree=10G"
  fi

  echo ""
  ok "╔══════════════════════════════════════════════════════════════╗"
  ok "║  FASE 3 COMPLETADA                                           ║"
  ok "║  Siguiente paso: desplegar CasparCG desde Jenkins / Titania  ║"
  ok "║  Ver: SETUP-DESDE-CERO.md § Fase 4                          ║"
  ok "╚══════════════════════════════════════════════════════════════╝"
}

# ══════════════════════════════════════════════════════════════════════════════
#  FASE 4B: CASPARCG OFICIAL STABLE (alternativa al fork de la Fase 4)
# ══════════════════════════════════════════════════════════════════════════════
#
# Confirmado en mosaic1/mosaic2 (2026-07-27): los parches de estabilidad del fork
# propio (de una época sin GPU/NVENC) provocan que ciertas señales se queden
# atascadas en "transition" sin frame en CasparCG, aunque el docker de origen
# emita vídeo/audio 100% válido. La misma señal funciona sin problema en el
# binario oficial. Ver SETUP-DESDE-CERO.md § Fase 4B / Troubleshooting.

fase4b_casparcg_stable() {
  hdr "FASE 4B — CasparCG oficial ${CASPARCG_STABLE_VERSION} (alternativa al fork)"

  # AVISO (2026-09-13): el .deb oficial de CEF (${CEF_DEB} más abajo) se compila con `sysroot`,
  # lo que crashea con SIGILL (OnMemoryDump/memory-infra, dentro de libcef.so) en hosts con
  # glibc >= 2.33 - Ubuntu 24.04/Noble lo incluye. Producción (Fase 4C, no esta Fase 4B) ya usa
  # un build sin sysroot (mko1989/highascg) via Bootstrap_Linux.cmake. Si se usa esta Fase 4B en
  # un host Noble, sustituir el .deb de CEF por el build sin sysroot antes de dar el host por
  # bueno. Ver BUILD.md y docs/leccion-sigill-cef-sysroot-buscar-en-foro.md para el detalle
  # completo (causa raiz, hilo del foro, fix).

  local CODENAME
  CODENAME=$(grep VERSION_CODENAME /etc/os-release | cut -d= -f2)
  if [[ "$CODENAME" != "jammy" && "$CODENAME" != "noble" ]]; then
    err "Codename '${CODENAME}' no reconocido (se esperaba jammy o noble)."
    err "Descargar manualmente el .deb correspondiente desde:"
    err "  https://github.com/CasparCG/server/releases/tag/${CASPARCG_RELEASE_TAG}"
    exit 1
  fi
  ok "Ubuntu ${CODENAME} detectado — usando paquetes ${CODENAME}1"

  local DL_DIR="${HOME}/Downloads/casparcg-stable"
  mkdir -p "$DL_DIR" && cd "$DL_DIR"

  local CEF_DEB="casparcg-cef-142_${CASPARCG_CEF_VERSION}-${CODENAME}1_amd64.deb"
  local SERVER_DEB="casparcg-server-2.5_${CASPARCG_STABLE_VERSION}-${CODENAME}1_amd64.deb"
  local BASE_URL="https://github.com/CasparCG/server/releases/download/${CASPARCG_RELEASE_TAG}"

  for DEB in "$CEF_DEB" "$SERVER_DEB"; do
    if [ ! -f "$DEB" ]; then
      info "Descargando ${DEB}..."
      wget -q --show-progress "${BASE_URL}/${DEB}"
    else
      ok "${DEB} ya descargado."
    fi
  done

  info "Instalando paquetes (apt resuelve dependencias)..."
  sudo apt install -y "./${CEF_DEB}" "./${SERVER_DEB}"

  local MISSING
  MISSING=$(ldd /usr/bin/casparcg-server-2.5 2>&1 | grep -i "not found" || true)
  if [ -n "$MISSING" ]; then
    err "Dependencias sin resolver en el binario oficial:"
    err "$MISSING"
    exit 1
  fi
  ok "Binario /usr/bin/casparcg-server-2.5 instalado, dependencias OK."

  # Si el fork (Fase 4) ya estaba desplegado y corriendo, ofrecer migrarlo.
  if [ -f "${MOSAIC_DIR}/casparcg_webrtc.config" ]; then
    warn "Config existente detectada en ${MOSAIC_DIR}/casparcg_webrtc.config"
    read -r -p "  ¿Migrar este nodo del fork al oficial ahora (para/reemplaza casparcg.service)? [s/N] " RESP
    if [[ "${RESP,,}" == "s" ]]; then
      info "Parando servicios del fork si existen..."
      sudo systemctl stop casparcg casparcg-watchdog 2>/dev/null || true
      sudo systemctl disable casparcg casparcg-watchdog 2>/dev/null || true
      sudo rm -f /etc/systemd/system/casparcg-watchdog.service
      sudo rm -f /etc/systemd/system/casparcg.service
      sudo rm -rf /etc/systemd/system/casparcg.service.d
      sudo systemctl reset-failed casparcg 2>/dev/null || true

      if [ -d "${MOSAIC_DIR}/bin" ] && [ ! -L "${MOSAIC_DIR}/bin" ]; then
        info "Moviendo ${MOSAIC_DIR}/bin (fork) a backup (no se borra)..."
        mv "${MOSAIC_DIR}/bin" "${MOSAIC_DIR}-fork-bin-backup"
      fi
      mkdir -p "${MOSAIC_DIR}/thumbnails" "${MOSAIC_DIR}/font"

      sudo tee /etc/systemd/system/casparcg.service > /dev/null << EOF
[Unit]
Description=CasparCG Server 2.5 (oficial, stable)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=titania
Group=titania
WorkingDirectory=${MOSAIC_DIR}
ExecStartPre=/usr/bin/mkdir -p ${MOSAIC_DIR}/cef-cache
ExecStartPre=/usr/bin/rm -rf ${MOSAIC_DIR}/cef-cache/*
ExecStart=/usr/bin/casparcg-server-2.5 ${MOSAIC_DIR}/casparcg_webrtc.config
Restart=on-failure
RestartSec=5
LimitNOFILE=1048576
UMask=0002
SyslogIdentifier=casparcg

[Install]
WantedBy=multi-user.target
EOF
      sudo systemctl daemon-reload
      sudo systemctl enable casparcg
      warn "Arrancar el servicio corta cualquier señal en reproducción — el mosaico"
      warn "quedará en negro hasta que Titania relance START_MOSAIC_N."
      read -r -p "  ¿Arrancar casparcg.service (oficial) ahora? [s/N] " RESP2
      if [[ "${RESP2,,}" == "s" ]]; then
        sudo systemctl start casparcg
        sleep 3
        systemctl status casparcg --no-pager || true
        echo -e "VERSION\r\n" | nc -q2 localhost 5250 || warn "AMCP no respondió — revisar 'journalctl -u casparcg'."
      fi
    fi
  else
    ok "No hay config previa del fork — instalación limpia, usar el config de ejemplo:"
    ok "  /usr/share/casparcg-server-2.5/casparcg.config"
  fi
}

# ══════════════════════════════════════════════════════════════════════════════
#  FASE 4C: COMPILAR EL BINARIO CON EL PARCHE OSC-POR-CAPA (vúmetro)
# ══════════════════════════════════════════════════════════════════════════════
#
# El .deb oficial (Fase 4B) NO trae el desglose de audio por capa vía OSC
# (/channel/N/mixer/layer/L/audio/peak/CH) que necesita el vúmetro — confirmado
# en mosaic1/mosaic2 (2026-07-27). El parche que lo añade vive aislado en
# tools/patches/osc-audio-per-layer-on-v2.5.0-stable.patch (7 ficheros de
# src/core/mixer y src/core/producer/stage — NO toca av_input.cpp/av_producer.cpp,
# así que no reintroduce el bug de "transition" del fork).
#
# Requiere que ~/casparcg-osc-build.tar.gz ya esté copiado a mano en este nodo
# (generado una vez en el puesto de desarrollo: descargar
# https://github.com/CasparCG/server/archive/refs/tags/v2.5.0-stable.zip,
# aplicar el .patch de arriba con "patch -p1", empaquetar con tar). Este paso NO
# se automatiza aquí porque requiere acceso al histórico git del repo server
# (para regenerar el parche si hiciera falta) y transferencia manual del
# resultado — ver SETUP-DESDE-CERO.md § Fase 4C.

fase4c_build_osc_patch() {
  hdr "FASE 4C — Compilar CasparCG oficial + parche OSC por capa (vúmetro)"

  local SRC_TARBALL="${HOME}/casparcg-osc-build.tar.gz"
  if [ ! -f "$SRC_TARBALL" ]; then
    err "No se encuentra ${SRC_TARBALL}"
    err "Cópialo a mano a este nodo antes de ejecutar esta fase (ver SETUP-DESDE-CERO.md § Fase 4C)."
    exit 1
  fi

  local BUILD_ROOT="${HOME}/casparcg-osc-build"
  if [ -d "$BUILD_ROOT" ]; then
    warn "Ya existe ${BUILD_ROOT} de un intento anterior."
    read -r -p "  ¿Borrar y extraer de nuevo desde el .tar.gz? [s/N] " RESP
    [[ "${RESP,,}" == "s" ]] && rm -rf "$BUILD_ROOT"
  fi

  if [ ! -d "$BUILD_ROOT" ]; then
    info "Extrayendo ${SRC_TARBALL}..."
    tar xzf "$SRC_TARBALL" -C "$HOME"
  fi

  info "Configurando CMake (misma receta que build-and-deploy.sh)..."
  mkdir -p "${BUILD_ROOT}/build" && cd "${BUILD_ROOT}/build"
  cmake ../src \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_AVX2=ON \
    -DUSE_SYSTEM_CEF=OFF \
    -DUSE_STATIC_BOOST=OFF \
    -G Ninja

  info "Compilando con $(nproc) cores (5-30 min, CEF se descarga la primera vez)..."
  cmake --build . --parallel "$(nproc)" 2>&1 | tee /tmp/casparcg_osc_build.log

  info "Instalando en staging..."
  cmake --install . --prefix "${BUILD_ROOT}/staging"

  # Antes se instalaba en "${MOSAIC_DIR}-osc-test/bin" (ruta separada del oficial sin
  # parche, para poder alternar sin reinstalar mientras se validaba el parche). Ya está
  # validado en producción (mosaic2/4/5) — a partir de ahora se instala directamente en
  # el bin/ definitivo para no dejar dos carpetas paralelas de CasparCG por nodo.
  local TEST_BIN_DIR="${MOSAIC_DIR}/bin"
  if [ -d "$TEST_BIN_DIR" ] && [ "$(ls -A "$TEST_BIN_DIR" 2>/dev/null)" ]; then
    warn "${TEST_BIN_DIR} ya existe y no está vacío (¿binario del fork antiguo?)."
    read -r -p "  ¿Moverlo a ${TEST_BIN_DIR}.bak-$(date +%Y%m%d) antes de continuar? [s/N] " RESP
    [[ "${RESP,,}" == "s" ]] && mv "$TEST_BIN_DIR" "${TEST_BIN_DIR}.bak-$(date +%Y%m%d)"
  fi
  mkdir -p "$TEST_BIN_DIR"
  cp "${BUILD_ROOT}/staging/bin/casparcg" "$TEST_BIN_DIR/"
  cp "${BUILD_ROOT}/staging/lib/"*.so* "$TEST_BIN_DIR/" 2>/dev/null || true

  local CEF_SRC
  for CEF_SRC in "${BUILD_ROOT}/build/cef-prefix/src/cef/Release" \
                 "${BUILD_ROOT}/build/cef-prefix/src/cef/Resources" \
                 "${BUILD_ROOT}/staging/lib"; do
    [ -d "$CEF_SRC" ] || continue
    cp "$CEF_SRC"/icudtl.dat "$CEF_SRC"/*.pak "$CEF_SRC"/v8_context_snapshot.bin \
       "$CEF_SRC"/vk_swiftshader_icd.json "$CEF_SRC"/libcef.so "$CEF_SRC"/libEGL.so \
       "$CEF_SRC"/libGLESv2.so "$CEF_SRC"/libvk_swiftshader.so "$CEF_SRC"/libvulkan.so.1 \
       "$CEF_SRC"/chrome-sandbox "$TEST_BIN_DIR/" 2>/dev/null || true
    [ -d "$CEF_SRC/locales" ] && cp -r "$CEF_SRC/locales" "$TEST_BIN_DIR/" 2>/dev/null || true
  done

  patchelf --set-rpath '$ORIGIN' "$TEST_BIN_DIR/casparcg"
  sudo chown root:root "$TEST_BIN_DIR/chrome-sandbox" 2>/dev/null || true
  sudo chmod 4755 "$TEST_BIN_DIR/chrome-sandbox" 2>/dev/null || true

  local MISSING
  MISSING=$(ldd "$TEST_BIN_DIR/casparcg" 2>&1 | grep -i "not found" || true)
  if [ -n "$MISSING" ]; then
    err "Dependencias sin resolver en el binario compilado:"
    err "$MISSING"
    exit 1
  fi
  ok "Binario compilado y verificado: ${TEST_BIN_DIR}/casparcg"
  readelf -d "$TEST_BIN_DIR/casparcg" | grep -iE "rpath|runpath"

  if [ ! -f /etc/systemd/system/casparcg.service ]; then
    warn "No existe /etc/systemd/system/casparcg.service todavía — ejecutar fase4b primero."
    return 0
  fi

  warn "Apuntar casparcg.service a este binario para probarlo corta cualquier señal en"
  warn "reproducción (se pierden los PLAY en memoria — habrá que relanzar el mosaico)."
  read -r -p "  ¿Apuntar casparcg.service al binario nuevo ahora? [s/N] " RESP
  if [[ "${RESP,,}" == "s" ]]; then
    sudo cp /etc/systemd/system/casparcg.service /etc/systemd/system/casparcg.service.bak-oficial
    sudo sed -i "s|^ExecStart=.*/casparcg[^ ]* |ExecStart=${TEST_BIN_DIR}/casparcg |" /etc/systemd/system/casparcg.service
    grep "^ExecStart=" /etc/systemd/system/casparcg.service
    sudo systemctl daemon-reload
    sudo systemctl kill -s SIGKILL casparcg 2>/dev/null || true
    sleep 1
    sudo pkill -9 -f casparcg 2>/dev/null || true
    sleep 1
    sudo systemctl start casparcg
    sleep 3
    systemctl status casparcg --no-pager || true
    echo -e "VERSION\r\n" | nc -q2 localhost 5250 || warn "AMCP no respondió — revisar 'journalctl -u casparcg'."
    ok "Para volver al oficial sin parche: sudo cp /etc/systemd/system/casparcg.service.bak-oficial /etc/systemd/system/casparcg.service && sudo systemctl daemon-reload && sudo systemctl restart casparcg"
  fi
}

# ══════════════════════════════════════════════════════════════════════════════
#  CHECKLIST DE VERIFICACIÓN FINAL
# ══════════════════════════════════════════════════════════════════════════════

check_sistema() {
  hdr "CHECKLIST DE VERIFICACIÓN — Nodo Mosaico"
  echo ""
  local -i OK=0 FAIL=0

  chk() {
    local DESC="$1"; shift
    if "$@" &>/dev/null; then
      echo -e "  ${GREEN}✓${NC} ${DESC}"
      OK=$((OK + 1))
    else
      echo -e "  ${RED}✗${NC} ${DESC}"
      FAIL=$((FAIL + 1))
    fi
  }

  chk "nvidia-smi funciona"                       command -v nvidia-smi
  chk "GPU NVIDIA detectada (nvidia-smi)"          nvidia-smi --query-gpu=name --format=csv,noheader
  chk "Docker instalado"                           command -v docker
  # NOTA: los checks que necesitan un pipe ("cmd | grep ...") van envueltos en
  # "bash -c '...'" para que el pipe se evalúe DENTRO de chk (sobre la salida del
  # comando real), no fuera de ella (sobre el propio texto "✓ ..." que imprime chk).
  # Sin este envoltorio, con "set -o pipefail" el script entero moría en silencio
  # en la primera de estas líneas (confirmado en mosaic4, 2026-07-17).
  chk "Docker runtime NVIDIA"                      bash -c 'docker info 2>/dev/null | grep -q nvidia'
  chk "Docker prueba GPU (NVENC)"                  docker run --rm --gpus all jrottenberg/ffmpeg:6.1-nvidia -f lavfi -i testsrc=size=64x64:rate=1:duration=1 -c:v h264_nvenc -f null -
  chk "Imagen ffmpeg:6.1-ubuntu presente"          bash -c 'docker images | grep -q "6.1-ubuntu"'
  chk "Imagen ffmpeg:6.1-nvidia presente"          bash -c 'docker images | grep -q "6.1-nvidia"'
  chk "Buffer UDP rmem_max=67108864"               bash -c '[[ $(sysctl -n net.core.rmem_max) -ge 67108864 ]]'
  chk "AVX2 disponible en CPU"                     grep -q avx2 /proc/cpuinfo
  chk "Usuario titania existe"                     id titania
  chk "titania en grupo docker"                    bash -c 'groups titania | grep -q docker'
  chk "Sudoers titania NOPASSWD:ALL"               bash -c 'sudo -l -U titania 2>/dev/null | grep -q "NOPASSWD: ALL"'
  chk "SSH PasswordAuthentication activado"        bash -c 'sudo sshd -T 2>/dev/null | grep -qi "^passwordauthentication yes"'
  chk "Docker Compose plugin"                      bash -c 'docker compose version &>/dev/null'
  chk "docker login registry.overon.es (titania)"  bash -c 'sudo -u titania bash -c "grep -q registry.overon.es ~/.docker/config.json" 2>/dev/null'
  if id noc &>/dev/null; then
    chk "usuario noc en grupo docker"              bash -c 'groups noc | grep -q docker'
  fi
  chk "snmpd activo con comunidad public-titania"  bash -c 'systemctl is-active --quiet snmpd && sudo grep -q "rocommunity public-titania" /etc/snmp/snmpd.conf'
  chk "Node.js v20 instalado"                      bash -c 'node --version | grep -q "^v20"'
  chk "netcat-openbsd instalado"                   command -v nc
  chk "patchelf instalado"                         command -v patchelf
  chk "Servicio casparcg habilitado"               systemctl is-enabled casparcg
  chk "Servicio osc-bridge habilitado"             systemctl is-enabled osc-bridge
  # El binario puede ser el fork (Fase 4, en ${MOSAIC_DIR}/bin/casparcg) o el
  # oficial (Fase 4B, /usr/bin/casparcg-server-2.5) — comprobar el que exista.
  if [ -f /usr/bin/casparcg-server-2.5 ]; then
    chk "CasparCG binario existe (oficial 2.5.0 stable)"  test -f /usr/bin/casparcg-server-2.5
    chk "CasparCG RUNPATH autocontenido (CEF)"             bash -c 'readelf -d /usr/bin/casparcg-server-2.5 | grep -qi "casparcg-cef"'
  else
    chk "CasparCG binario existe (fork)"              test -f "${MOSAIC_DIR}/bin/casparcg"
    chk "CasparCG RPATH correcto (fork)"              bash -c "readelf -d \"${MOSAIC_DIR}/bin/casparcg\" | grep -q \"${MOSAIC_DIR}/bin\""
    chk "chrome-sandbox setuid root (fork)"           bash -c '[[ $(stat -c "%U %a" "${MOSAIC_DIR}/bin/chrome-sandbox" 2>/dev/null) == "root 4755" ]]'
  fi
  chk "CasparCG escucha en 5250"                   nc -z localhost 5250
  chk "osc-bridge responde en :6253"               curl -sf http://localhost:6253/ -o /dev/null
  chk "teletext-bridge habilitado"                 systemctl is-enabled teletext-bridge
  chk "Directorio templates existe"                test -d "${MOSAIC_DIR}/templates"

  echo ""
  echo -e "  Resultado: ${GREEN}${OK} OK${NC} | ${RED}${FAIL} FALLIDOS${NC}"
  echo ""

  if (( FAIL == 0 )); then
    ok "Sistema completamente configurado. Listo para arrancar el mosaico."
    info "Ejecutar: cd ~/mosaic/server/tools/linux && ./start-mosaic5x4.sh start"
  else
    warn "${FAIL} verificación(es) fallida(s). Revisar los items marcados con ✗."
    info "Consultar: SETUP-DESDE-CERO.md § Troubleshooting"
  fi
}

# ══════════════════════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════════════════════

CMD="${1:-help}"

case "$CMD" in
  fase1)
    fase1_nvidia
    ;;
  fase1b)
    fase1b_nvenc_patch
    ;;
  fase2)
    fase2_hardware
    ;;
  fase3)
    fase3_sistema
    ;;
  fase4b)
    fase4b_casparcg_stable
    ;;
  fase4c)
    fase4c_build_osc_patch
    ;;
  check)
    check_sistema
    ;;
  *)
    echo ""
    echo -e "${BOLD}setup-desde-cero.sh${NC} — Configuración de nodo mosaico CasparCG"
    echo -e "GPU objetivo: NVIDIA Quadro P1000 | Ubuntu 24.04 LTS"
    echo ""
    echo "  Uso: $0 <fase>"
    echo ""
    echo -e "  ${CYAN}fase1${NC}   Instalar driver NVIDIA ${NVIDIA_DRIVER_VERSION} (fijado a ${NVIDIA_DRIVER_EXACT}) + nvidia-container-toolkit"
    echo -e "  ${CYAN}fase1b${NC}  Aplicar nvidia-patch (límite de sesiones NVENC) + verificar — ejecutar TRAS reiniciar fase1"
    echo -e "  ${CYAN}fase2${NC}   Analizar hardware: CPU/RAM/GPU/red → informe de viabilidad"
    echo -e "  ${CYAN}fase3${NC}   Configurar sistema: usuario, SSH, deps, Docker+login, noc, snmpd, kernel UDP, systemd, dirs"
    echo -e "  ${CYAN}fase4b${NC}  Instalar CasparCG oficial ${CASPARCG_STABLE_VERSION} (recomendado, ver gotcha del fork)"
    echo -e "  ${CYAN}fase4c${NC}  Compilar + activar el parche OSC por capa (vúmetro) sobre el oficial"
    echo -e "  ${CYAN}check${NC}   Checklist de verificación final (todos los componentes)"
    echo ""
    echo "  Orden recomendado:"
    echo "    1) ./setup-desde-cero.sh fase1    # instalar driver (${NVIDIA_DRIVER_EXACT} fijado) + reboot"
    echo "    2) ./setup-desde-cero.sh fase1b   # aplica el parche NVENC (en equipo virgen, la verificación"
    echo "                                      # se salta porque Docker aún no existe — normal, se repite en el paso 4)"
    echo "    3) ./setup-desde-cero.sh fase3    # configurar sistema: instala Docker, SSH, sudo, noc, snmpd..."
    echo "    4) ./setup-desde-cero.sh fase1b   # repetir: ahora sí verifica las 16 sesiones NVENC (Docker ya existe)"
    echo "    5) ./setup-desde-cero.sh fase2    # verificar hardware"
    echo "    6a) ./setup-desde-cero.sh fase4b  # RECOMENDADO: CasparCG oficial (evita el bug de 'transition' del fork)"
    echo "    6b) [alternativa: Deploy desde Jenkins/Titania — build + deploy del fork propio]"
    echo "    6c) ./setup-desde-cero.sh fase4c  # RECOMENDADO: compilar el parche OSC (vúmetro por capa)"
    echo "    7) ./setup-desde-cero.sh check    # verificación final"
    echo ""
    echo "  Documentación: SETUP-DESDE-CERO.md"
    echo ""
    exit 1
    ;;
esac
