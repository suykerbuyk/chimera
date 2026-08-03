#!/bin/bash
# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# Wrapper so the VSCode CMake Tools extension runs cmake (configure + build)
# as root. Requires passwordless sudo. -E preserves the environment the
# extension sets; -n fails fast instead of hanging on a password prompt.
exec sudo -En /usr/bin/cmake "$@"
