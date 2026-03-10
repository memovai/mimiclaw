#!/bin/bash
#
# Build MicroPython embed port for MimiClaw.
#
# This script:
#   1. Clones/updates MicroPython source as a git submodule
#   2. Builds mpy-cross (required by the embed port)
#   3. Copies our port config into the embed port
#   4. Builds the embed port to generate amalgamated C/H files
#   5. Copies the generated files into components/micropython_embed/generated/
#
# After running this script, rebuild the project with: idf.py build
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MP_DIR="${PROJECT_DIR}/lib/micropython"
COMPONENT_DIR="${PROJECT_DIR}/components/micropython_embed"
PORT_DIR="${COMPONENT_DIR}/port"
GENERATED_DIR="${COMPONENT_DIR}/generated"

echo "=== MimiClaw MicroPython Embed Builder ==="
echo ""

# Step 1: Get MicroPython source
if [ ! -d "${MP_DIR}/ports/embed" ]; then
    echo "[1/5] Cloning MicroPython..."
    mkdir -p "${PROJECT_DIR}/lib"
    git -C "${PROJECT_DIR}" submodule add https://github.com/micropython/micropython.git lib/micropython 2>/dev/null || true
    git -C "${PROJECT_DIR}" submodule update --init lib/micropython
else
    echo "[1/5] MicroPython source found at ${MP_DIR}"
fi

# Step 2: Build mpy-cross
echo "[2/5] Building mpy-cross..."
make -C "${MP_DIR}/mpy-cross" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

# Step 3: Copy port config
echo "[3/5] Copying port configuration..."
cp "${PORT_DIR}/mpconfigport.h" "${MP_DIR}/ports/embed/port/"
cp "${PORT_DIR}/mphalport.h" "${MP_DIR}/ports/embed/port/"

# Step 4: Build embed port
echo "[4/5] Building MicroPython embed port..."
make -C "${MP_DIR}/ports/embed" clean
make -C "${MP_DIR}/ports/embed" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

# Step 5: Copy generated files
echo "[5/5] Copying generated files..."
mkdir -p "${GENERATED_DIR}"
cp "${MP_DIR}/ports/embed/build-standard/micropython_embed.c" "${GENERATED_DIR}/"
cp "${MP_DIR}/ports/embed/build-standard/micropython_embed.h" "${GENERATED_DIR}/"

echo ""
echo "=== Done! ==="
echo "Generated files:"
ls -lh "${GENERATED_DIR}/micropython_embed."*
echo ""
echo "Now build the project:"
echo "  idf.py build"
