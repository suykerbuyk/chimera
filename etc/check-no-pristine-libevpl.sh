#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# check-no-pristine-libevpl.sh - fail if chimera's own CMake still references the
# pristine ext/libevpl checkout instead of the build-tree overlay (LIBEVPL_SRC
# and the derived LIBEVPL_INC / PROMETHEUS_SRC / OTEL_SRC).
#
# The overlay is the libevpl the build must consume (cmake/ApplyPatches.cmake);
# a leftover literal `ext/libevpl` path would quietly build against the pinned,
# unpatched submodule.  This gate catches that.
#
# Allowed exceptions:
#   - ext/CMakeLists.txt            : adds the overlay via LIBEVPL_SRC
#   - cmake/ApplyPatches.cmake      : defines the overlay; refers to pristine by
#                                     design
#   - the single N3 guarded fallback line in
#     src/common/tests/native/CMakeLists.txt (the `if(NOT LIBEVPL_SRC)` branch),
#     which uses libevpl HEADERS ONLY when built as a standalone project.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

ALLOW_FILES=(
    "ext/CMakeLists.txt"
    "cmake/ApplyPatches.cmake"
)
# Exact text of the allowed N3 fallback line (matched by substring).
NATIVE_FILE="src/common/tests/native/CMakeLists.txt"
NATIVE_ALLOW='set(_native_libevpl_inc "${CHIMERA_SOURCE_ROOT}/ext/libevpl/include")'

is_allowed_file() {
    local f="$1"
    for a in "${ALLOW_FILES[@]}"; do
        [[ "$f" == "$a" ]] && return 0
    done
    return 1
}

offenders=()

# Chimera-owned CMake files only: skip the vendored submodule trees under ext/
# and any build directory.
while IFS= read -r -d '' file; do
    rel="${file#"$ROOT"/}"
    is_allowed_file "$rel" && continue

    # Strip '#' comments before matching: the gate is about path references in
    # CMake code, not prose that happens to name the directory.
    if [[ "$rel" == "$NATIVE_FILE" ]]; then
        # Allow exactly the guarded fallback line; flag anything else.
        hits="$(sed 's/#.*//' "$file" | grep -n 'ext/libevpl' | grep -vF "$NATIVE_ALLOW" || true)"
    else
        hits="$(sed 's/#.*//' "$file" | grep -n 'ext/libevpl' || true)"
    fi

    if [[ -n "$hits" ]]; then
        while IFS= read -r line; do
            offenders+=("$rel:$line")
        done <<< "$hits"
    fi
done < <(
    find "$ROOT" \
        -type d \( \
            -name .git -o \
            -name build -o \
            -path "$ROOT/ext/libevpl" -o \
            -path "$ROOT/ext/ndrzcc" -o \
            -path "$ROOT/ext/xdrzcc" -o \
            -path "$ROOT/ext/specs" \
        \) -prune -o \
        -type f \( -name 'CMakeLists.txt' -o -name '*.cmake' \) -print0
)

if [[ ${#offenders[@]} -gt 0 ]]; then
    echo "ERROR: pristine 'ext/libevpl' path(s) in chimera CMake; use LIBEVPL_SRC/" >&2
    echo "       LIBEVPL_INC/PROMETHEUS_SRC/OTEL_SRC (the overlay) instead:" >&2
    for o in "${offenders[@]}"; do
        echo "  $o" >&2
    done
    exit 1
fi

echo "check-no-pristine-libevpl: OK (no pristine ext/libevpl paths in chimera CMake)"
