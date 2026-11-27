#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# MM3D whole-AOT Switch product (.nro). Runs inside devkitpro/devkita64.
# Inputs come from the user's own dump (never versioned). GENERATED_DIR must be
# produced by this tree's generate_whole_aot_cpp.py (--shards 256
# --shard-strategy affinity --native-vfp-aarch64 --trap-unsupported); shards
# from another tree use a different runtime ABI. CODE_BIN is the Thumb-free
# image the program was built from.
#
# Optional: OOT3D_SWITCH_DEPS_DIR with pre-fetched sources (one directory per
# dependency, lower-case name) for offline configure.
# If GitHub is unreachable, CMake's stb_image.h download leaves an empty file:
# place the header with the hash pinned in dependencies/common.cmake there.
set -euo pipefail

if [[ $# -ne 5 ]]; then
    printf 'usage: %s GENERATED_DIR AOT_PROGRAM SELECTION CODE_BIN EXHEADER\n' "$0" >&2
    exit 2
fi
generated=$1 program=$2 selection=$3 code=$4 exheader=$5
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${OOT3D_SWITCH_PRODUCT_BUILD_DIR:-"${repo}/build-switch-mm3d"}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="${DEVKITPRO}/devkitA64/bin:${DEVKITPRO}/tools/bin:${PATH}"

mkdir -p "${build}"
contract=${build}/mm3d_product_contract.json
python3 "${repo}/tools/mm3d/make_product_contract.py" \
    --program "${program}" --selection "${selection}" --code "${code}" \
    --exheader "${exheader}" \
    --generated-manifest "${generated}/whole_aot_cpp_manifest.json" \
    --output "${contract}"

offline=()
if [[ -n ${OOT3D_SWITCH_DEPS_DIR:-} ]]; then
    offline+=(-DFETCHCONTENT_FULLY_DISCONNECTED=ON)
    for dir in "${OOT3D_SWITCH_DEPS_DIR}"/*/; do
        name=$(basename "${dir}")
        offline+=("-DFETCHCONTENT_SOURCE_DIR_${name^^}=${dir%/}")
    done
fi

cmake -S "${repo}" -B "${build}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DOOT3D_ENABLE_VULKAN_RENDERER=OFF \
    -DOOT3D_WHOLE_AOT_PRODUCT_MODE=ON -DOOT3D_REQUIRE_WHOLE_AOT=ON \
    -DOOT3D_REBUILD_WHOLE_AOT=ON -DOOT3D_REGENERATE_WHOLE_AOT=OFF \
    -DOOT3D_WHOLE_AOT_MAX_PARALLEL_COMPILES="${OOT3D_AOT_JOBS:-6}" \
    -DOOT3D_WHOLE_AOT_PRODUCT_MANIFEST="${contract}" \
    -DOOT3D_WHOLE_AOT_GENERATED_DIR="${generated}" \
    -DOOT3D_WHOLE_AOT_PROGRAM="${program}" \
    -DOOT3D_WHOLE_AOT_SELECTION="${selection}" \
    -DOOT3D_WHOLE_AOT_CODE_BIN="${code}" \
    -DOOT3D_WHOLE_AOT_EXHEADER="${exheader}" \
    "${offline[@]}"

stb=${build}/_deps/stb/stb_image.h
if [[ ! -s ${stb} ]]; then
    printf 'stb_image.h is empty (download failed): %s\n' "${stb}" >&2
    exit 1
fi

cmake --build "${build}" --target oot3d_native_game_nro \
    -j "${CMAKE_BUILD_PARALLEL_LEVEL:-7}"

nro=${build}/oot3d_native_game.nro
[[ $(dd if="${nro}" bs=1 skip=16 count=4 2>/dev/null) == NRO0 ]] || {
    printf 'invalid NRO0 header: %s\n' "${nro}" >&2; exit 1; }
printf 'MM3D Switch NRO: %s\n' "${nro}"
sha256sum "${nro}"
