#!/usr/bin/env bash
set -euo pipefail

viewer_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
project_dir=$(cd "${viewer_dir}/.." && pwd)

if [[ -n "${EMXX:-}" ]]; then
    compiler=${EMXX}
elif command -v em++ >/dev/null 2>&1; then
    compiler=$(command -v em++)
elif [[ -n "${EMSDK:-}" && -x "${EMSDK}/upstream/emscripten/em++" ]]; then
    compiler=${EMSDK}/upstream/emscripten/em++
else
    echo "Emscripten not found. Activate emsdk or set EMXX to em++." >&2
    exit 1
fi

mkdir -p "${viewer_dir}/src/generated"
exported_functions='["_zfs_reset","_zfs_load","_zfs_play","_zfs_back","_zfs_forward","_zfs_last_error","_zfs_state_json","_zfs_line_san"]'

"${compiler}" \
    -std=c++20 -O3 -flto -DNDEBUG -fno-exceptions -fno-rtti \
    -I"${project_dir}/include" \
    "${project_dir}/src/attacks.cpp" \
    "${project_dir}/src/position.cpp" \
    "${viewer_dir}/wasm/bridge.cpp" \
    --no-entry \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sEXPORT_NAME=createZfsModule \
    -sENVIRONMENT=web \
    -sFILESYSTEM=0 \
    -sSINGLE_FILE=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sMALLOC=emmalloc \
    -sASSERTIONS=0 \
    -sEXPORTED_FUNCTIONS="${exported_functions}" \
    -sEXPORTED_RUNTIME_METHODS='["cwrap"]' \
    -o "${viewer_dir}/src/generated/zfs.js"
