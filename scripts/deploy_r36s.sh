#!/bin/bash
#===============================================================================
# deploy_r36s.sh — Cross-compile yabasanshiro_opt for R36S (RK3326) and deploy
#
# Usage:
#   ./scripts/deploy_r36s.sh              # build + deploy
#   ./scripts/deploy_r36s.sh --build-only # just build, don't deploy
#   ./scripts/deploy_r36s.sh --deploy-only # just deploy existing build
#   ./scripts/deploy_r36s.sh --clean      # clean + build + deploy
#
# Device: R36S (RK3326, Cortex-A35, Mali-G31, retrorun3)
# Core:   yabasanshiro_opt_libretro.so
# Path:   /home/ark/.config/retroarch/cores/yabasanshiro_opt_libretro.so
#===============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CORE_NAME="yabasanshiro_opt_libretro.so"
BUILD_DIR="$PROJECT_DIR/yabause/src/libretro"

# Device config
DEVICE_IP="192.168.0.14"
DEVICE_USER="ark"
DEVICE_PASS="ark"
DEVICE_CORE_PATH="/home/ark/.config/retroarch/cores/$CORE_NAME"
DEVICE_BIOS_DIR="/roms2/bios"
DEVICE_ROM_DIR="/roms2/saturn"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Parse args
BUILD=true
DEPLOY=true
CLEAN=false

for arg in "$@"; do
  case "$arg" in
    --build-only) DEPLOY=false ;;
    --deploy-only) BUILD=false ;;
    --clean) CLEAN=true ;;
    --help|-h)
      echo "Usage: $0 [--build-only|--deploy-only|--clean]"
      exit 0
      ;;
  esac
done

#===============================================================================
# Step 1: Generate M68K opcode tables (one-time)
#===============================================================================
if [ "$BUILD" = true ]; then
  echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
  echo -e "${CYAN}  Step 1: Generate M68K opcode tables${NC}"
  echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"

  if [ ! -f "$BUILD_DIR/musashi/m68kmake" ]; then
    echo -e "${YELLOW}  Building m68kmake...${NC}"
    make -C "$BUILD_DIR" generate-files
  else
    echo -e "${GREEN}  m68kmake already exists, skipping.${NC}"
  fi

  #=============================================================================
  # Step 2: Clean (optional)
  #=============================================================================
  if [ "$CLEAN" = true ]; then
    echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Step 2: Clean${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
    make -C "$BUILD_DIR" clean
  fi

  #=============================================================================
  # Step 3: Cross-compile for R36S (Cortex-A35, Mali-G31, GLES 3.0)
  #=============================================================================
  echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
  echo -e "${CYAN}  Step 3: Cross-compile for R36S (arm64_cortex_a53_gles3)${NC}"
  echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"

  # Detect cross-compiler
  CC=""
  for c in aarch64-linux-gnu-gcc aarch64-linux-gnu-gcc-13 aarch64-linux-gnu-gcc-12; do
    if command -v "$c" &>/dev/null; then
      CC="$c"
      break
    fi
  done

  if [ -z "$CC" ]; then
    echo -e "${RED}ERROR: No aarch64 cross-compiler found!${NC}"
    echo "Install with: sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
    exit 1
  fi

  echo -e "${GREEN}  Using compiler: $CC${NC}"
  echo -e "${GREEN}  Target: arm64_cortex_a53_gles3${NC}"
  echo ""

  # Build
  # platform=arm64_cortex_a53_gles3 sets:
  #   - ARCH_IS_LINUX=1, FORCE_GLES=1
  #   - USE_AARCH64_DRC=1, DYNAREC_DEVMIYAX=1
  #   - -march=armv8-a+crc+fp+simd -mcpu=cortex-a53 -mtune=cortex-a53
  make -C "$BUILD_DIR" platform=arm64_cortex_a53_gles3 -j"$(nproc)"

  # Verify output
  if [ ! -f "$BUILD_DIR/yabasanshiro_libretro.so" ]; then
    echo -e "${RED}ERROR: Build failed — no .so produced${NC}"
    exit 1
  fi

  # Rename to opt name
  cp "$BUILD_DIR/yabasanshiro_libretro.so" "$BUILD_DIR/$CORE_NAME"

  # Show size
  CORE_SIZE=$(stat --format=%s "$BUILD_DIR/$CORE_NAME")
  echo -e "${GREEN}  Build complete: $(numfmt --to=iec $CORE_SIZE)${NC}"
fi

#===============================================================================
# Step 4: Deploy to R36S
#===============================================================================
if [ "$DEPLOY" = true ]; then
  echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
  echo -e "${CYAN}  Step 4: Deploy to R36S ($DEVICE_IP)${NC}"
  echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"

  if [ ! -f "$BUILD_DIR/$CORE_NAME" ]; then
    echo -e "${RED}ERROR: $BUILD_DIR/$CORE_NAME not found. Build first.${NC}"
    exit 1
  fi

  # Test connectivity
  if ! sshpass -p "$DEVICE_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
       "${DEVICE_USER}@${DEVICE_IP}" "echo connected" &>/dev/null; then
    echo -e "${RED}ERROR: Cannot reach $DEVICE_IP${NC}"
    exit 1
  fi

  # Backup old core
  echo -e "${YELLOW}  Backing up old core...${NC}"
  sshpass -p "$DEVICE_PASS" ssh "${DEVICE_USER}@${DEVICE_IP}" \
    "cp $DEVICE_CORE_PATH ${DEVICE_CORE_PATH}.bak 2>/dev/null; echo 'done'"

  # Copy new core
  echo -e "${YELLOW}  Copying new core...${NC}"
  sshpass -p "$DEVICE_PASS" scp "$BUILD_DIR/$CORE_NAME" \
    "${DEVICE_USER}@${DEVICE_IP}:$DEVICE_CORE_PATH"

  # Verify
  REMOTE_SIZE=$(sshpass -p "$DEVICE_PASS" ssh "${DEVICE_USER}@${DEVICE_IP}" \
    "stat --format=%s $DEVICE_CORE_PATH")
  echo -e "${GREEN}  Deployed: $(numfmt --to=iec $REMOTE_SIZE)${NC}"

  # Set permissions
  sshpass -p "$DEVICE_PASS" ssh "${DEVICE_USER}@${DEVICE_IP}" \
    "chmod +x $DEVICE_CORE_PATH"

  echo -e "${GREEN}  Deploy complete!${NC}"
fi

#===============================================================================
# Step 5: Print run instructions
#===============================================================================
echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Run on device${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  ${YELLOW}SSH into device:${NC}"
echo -e "    sshpass -p $DEVICE_PASS ssh ${DEVICE_USER}@${DEVICE_IP}"
echo ""
echo -e "  ${YELLOW}Run with retrorun3 (FPS counter on):${NC}"
echo -e "    sudo perfmax performance /roms2/saturn/your_game.chd && \\"
echo -e "    retrorun3 -c /home/ark/.config/retrorun.cfg --triggers \\"
echo -e "      -s /roms2/saturn -d /roms2/bios \\"
echo -e "      $DEVICE_CORE_PATH \\"
echo -e "      /roms2/saturn/your_game.chd"
echo ""
echo -e "  ${YELLOW}Or run a quick perf test (5 seconds):${NC}"
echo -e "    sshpass -p $DEVICE_PASS ssh ${DEVICE_USER}@${DEVICE_IP} \\"
echo -e "      'timeout 5 retrorun3 -c /home/ark/.config/retrorun.cfg --triggers \\"
echo -e "        -s /roms2/saturn -d /roms2/bios \\"
echo -e "        $DEVICE_CORE_PATH \\"
echo -e "        /roms2/saturn/your_game.chd & sleep 2 && ps -o %cpu -C retrorun3'"
echo ""
echo -e "  ${YELLOW}Force GPU to max frequency:${NC}"
echo -e "    echo 520000000 > /sys/class/misc/mali0/device/devfreq/ff400000.gpu/max_freq"
echo -e "    echo 520000000 > /sys/class/misc/mali0/device/devfreq/ff400000.gpu/min_freq"
echo -e "    echo userspace > /sys/class/misc/mali0/device/devfreq/ff400000.gpu/governor"
echo ""
echo -e "${GREEN}  Done!${NC}"
