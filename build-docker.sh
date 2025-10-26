#!/bin/bash
# Faster PrusaSlicer build inside Docker with incremental builds, ccache, and Ninja.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="prusa-build-env:ubuntu22.04"
REBUILD_ENV=false
CLEAN_BUILD=false
CMAKE_PRESET=""
DEFAULT_CCACHE_DIR="${SCRIPT_DIR}/.ccache"
CCACHE_DIR="${DEFAULT_CCACHE_DIR}"
CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
CCACHE_MOUNT="/workspace/.ccache"
DEPS_BUILD_SUBDIR="docker-build"
HOST_DEPS_BUILD_DIR="${SCRIPT_DIR}/deps/${DEPS_BUILD_SUBDIR}"

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --rebuild-env          Rebuild the Docker build environment image.
  --clean                Remove build/deps artefacts before compiling.
  --preset <name>        Use a specific CMake configure preset (default: ${CMAKE_PRESET}).
  --ccache-dir <path>    Override the host ccache directory (default: ${CCACHE_DIR}).
  --help                 Show this help and exit.

Environment variables:
  CCACHE_MAXSIZE         Max cache size passed to ccache (default: ${CCACHE_MAXSIZE}).
  BUILD_JOBS             Override parallel build jobs (default: nproc inside container).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild-env)
            REBUILD_ENV=true
            ;;
        --clean)
            CLEAN_BUILD=true
            ;;
        --preset)
            shift
            if [[ $# -eq 0 ]]; then
                echo "error: --preset requires an argument" >&2
                exit 1
            fi
            CMAKE_PRESET="$1"
            ;;
        --ccache-dir)
            shift
            if [[ $# -eq 0 ]]; then
                echo "error: --ccache-dir requires an argument" >&2
                exit 1
            fi
            CCACHE_DIR="$1"
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option '$1'" >&2
            usage
            exit 1
            ;;
    esac
    shift
done

# Ensure the selected ccache directory is writable; fall back if an old cache is root-owned.
ensure_ccache_dir() {
    local target_dir="$1"
    mkdir -p "${target_dir}/tmp" 2>/dev/null || return 1
    local probe="${target_dir}/.ccache-write-test-$$"
    local probe_tmp="${target_dir}/tmp/.ccache-write-test-$$"
    if ! touch "${probe}" 2>/dev/null; then
        return 1
    fi
    if ! touch "${probe_tmp}" 2>/dev/null; then
        rm -f "${probe}" >/dev/null 2>&1 || true
        return 1
    fi
    rm -f "${probe}" "${probe_tmp}" >/dev/null 2>&1 || true
    return 0
}

if ! ensure_ccache_dir "${CCACHE_DIR}"; then
    FALLBACK_CCACHE_DIR="${SCRIPT_DIR}/.ccache-${HOST_UID}"
    if [[ "${CCACHE_DIR}" == "${DEFAULT_CCACHE_DIR}" ]]; then
        echo "[cache] '${CCACHE_DIR}' is not writable, using '${FALLBACK_CCACHE_DIR}' instead."
    else
        echo "[cache] '${CCACHE_DIR}' is not writable, falling back to '${FALLBACK_CCACHE_DIR}'." >&2
    fi
    if ! ensure_ccache_dir "${FALLBACK_CCACHE_DIR}"; then
        echo "[cache] failed to prepare writable ccache directory ('${CCACHE_DIR}' and '${FALLBACK_CCACHE_DIR}')." >&2
        exit 1
    fi
    CCACHE_DIR="${FALLBACK_CCACHE_DIR}"
fi

echo "[1/4] Preparing build environment image..."
if [[ "${REBUILD_ENV}" == "true" ]]; then
    docker build -q -f "${SCRIPT_DIR}/Dockerfile.buildenv" -t "${IMAGE_NAME}" "${SCRIPT_DIR}"
elif ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    echo "Building build environment image (one-time setup)..."
    docker build -q -f "${SCRIPT_DIR}/Dockerfile.buildenv" -t "${IMAGE_NAME}" "${SCRIPT_DIR}"
fi

echo "[2/4] Ensuring host caches..."
mkdir -p "${CCACHE_DIR}"
mkdir -p "${HOST_DEPS_BUILD_DIR}"

if [[ "${CLEAN_BUILD}" == "true" ]]; then
    echo "[clean] Removing cached artefacts on host before container run..."
    rm -rf "${HOST_DEPS_BUILD_DIR}" \
           "${SCRIPT_DIR}/build/docker-ninja" \
           "${SCRIPT_DIR}/build/docker-ninja/CMakeCache.txt" 2>/dev/null || true
fi

echo "[3/4] Launching build container..."

CONTAINER_SCRIPT=$(cat <<'EOS'
set -euo pipefail

cd /workspace

export HOME="/workspace"
export USER="${CONTAINER_USER:-builder}"
export CCACHE_DIR="${CCACHE_DIR_IN_CONTAINER:-/workspace/.ccache}"
export CCACHE_BASEDIR="/workspace"
export CCACHE_NOHASHDIR=1
DEPS_BUILD_SUBDIR="${DEPS_BUILD_SUBDIR:-docker-build}"
DEPS_BUILD_DIR="/workspace/deps/${DEPS_BUILD_SUBDIR}"
DEPS_DESTDIR="${DEPS_BUILD_DIR}/destdir/usr/local"
mkdir -p "${DEPS_BUILD_DIR}"
unset CC CXX

ccache --zero-stats >/dev/null 2>&1 || true
if [[ -n "${CCACHE_MAXSIZE:-}" ]]; then
    ccache --max-size="${CCACHE_MAXSIZE}" >/dev/null 2>&1 || true
fi

COMMON_CMAKE_ARGS=(
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    "-DOPENSSL_ROOT_DIR=${DEPS_DESTDIR}"
    "-DOPENSSL_INCLUDE_DIR=${DEPS_DESTDIR}/include"
    "-DOPENSSL_USE_STATIC_LIBS=ON"
)

if [[ "${CLEAN_BUILD}" == "true" ]]; then
    echo "[clean] Removing cached artefacts inside container..."
    rm -rf "${DEPS_BUILD_DIR}" /workspace/build/docker-ninja
fi

echo "[deps] Configuring (Ninja generator)..."
cmake -S /workspace/deps -B "${DEPS_BUILD_DIR}" -G Ninja -DDEP_WX_GTK3=ON "${COMMON_CMAKE_ARGS[@]}" >/dev/null

echo "[deps] Building dependencies..."
JOBS="${BUILD_JOBS:-$(nproc)}"
cmake --build "${DEPS_BUILD_DIR}" --parallel "${JOBS}"

if [[ -d "${DEPS_DESTDIR}/lib" && ! -e "${DEPS_DESTDIR}/lib64" ]]; then
    ln -sfn lib "${DEPS_DESTDIR}/lib64"
fi

GTK_CFLAGS="$(pkg-config --cflags gtk+-3.0 || echo "")"
CONFIGURE_FLAGS=()
if [[ -n "${GTK_CFLAGS}" ]]; then
    CONFIGURE_FLAGS+=("-DCMAKE_CXX_FLAGS=${GTK_CFLAGS}")
fi
CONFIGURE_FLAGS+=("${COMMON_CMAKE_ARGS[@]}")

MANUAL_ARGS=(
    -S /workspace
    -B /workspace/build/docker-ninja
    -G Ninja
    -DSLIC3R_STATIC=1
    -DSLIC3R_GTK=3
    -DSLIC3R_PCH=OFF
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_PREFIX_PATH=${DEPS_DESTDIR}"
)

BUILD_COMMAND=()
if [[ -n "${CMAKE_PRESET:-}" ]] && cmake --list-presets 2>/dev/null | grep -q "\"${CMAKE_PRESET}\""; then
    echo "[cmake] Configuring with preset '${CMAKE_PRESET}'..."
    cmake --preset "${CMAKE_PRESET}" "${CONFIGURE_FLAGS[@]}"
    BUILD_COMMAND=(cmake --build --preset "${CMAKE_PRESET}" --parallel "${JOBS}")
else
    if [[ -n "${CMAKE_PRESET:-}" ]]; then
        echo "[cmake] Preset '${CMAKE_PRESET}' not found; falling back to manual settings."
    fi
    echo "[cmake] Configuring manually with Ninja..."
    cmake "${MANUAL_ARGS[@]}" "${CONFIGURE_FLAGS[@]}" >/dev/null
    BUILD_COMMAND=(cmake --build /workspace/build/docker-ninja --parallel "${JOBS}")
fi

echo "[build] Building PrusaSlicer and all targets..."
"${BUILD_COMMAND[@]}"

TARGET_LINK="/workspace/build/docker-ninja/resources"
ln -sfn ../../resources "${TARGET_LINK}"

echo "[ccache] Stats after build:"
ccache --show-stats || true
EOS
)

docker run --rm \
    -v "${SCRIPT_DIR}:/workspace" \
    -v "${CCACHE_DIR}:${CCACHE_MOUNT}" \
    -w /workspace \
    -u "${HOST_UID}:${HOST_GID}" \
    -e CCACHE_DIR_IN_CONTAINER="${CCACHE_MOUNT}" \
    -e CONTAINER_USER="${USER:-builder}" \
    -e DEPS_BUILD_SUBDIR="${DEPS_BUILD_SUBDIR}" \
    -e CLEAN_BUILD="${CLEAN_BUILD}" \
    -e CMAKE_PRESET="${CMAKE_PRESET}" \
    -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE}" \
    -e BUILD_JOBS="${BUILD_JOBS:-}" \
    "${IMAGE_NAME}" \
    bash -lc "${CONTAINER_SCRIPT}"

echo "[4/4] Build complete: ${SCRIPT_DIR}/build/docker-ninja/src/prusa-slicer"
