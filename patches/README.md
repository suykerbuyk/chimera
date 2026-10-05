<!--
SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
SPDX-License-Identifier: LGPL-2.1-only
-->

# `patches/` — build-tree overlay for pinned submodules

Chimera builds against vendored submodules under `ext/` that stay **pinned**:
the gitlinks never move and the checked-out trees are never edited. When a local
fix is needed against one of them, it lives here as a tracked patch, and the
build applies it to a **copy** of the submodule in the build tree — the
*overlay* — rather than to the submodule itself.

The series currently carries the libevpl/xdrzcc decode-length bound patches; the
overlay is the pristine submodule with that series applied on top, and the full
build must succeed against it. An empty series is also valid — the overlay is
then a byte-faithful copy of the pristine submodule.

## Which libevpl/xdrzcc is this?

The build uses libevpl's **nested** copy of xdrzcc at
`ext/libevpl/ext/xdrzcc` — that gitlink is what gets built, via libevpl's own
CMake. Chimera *also* has a top-level `ext/xdrzcc` gitlink, but it is
**uninitialised and unbuilt**; do **not** initialise or patch it. All libevpl
and nested-xdrzcc patches target the copy under `ext/libevpl`.

## How the overlay is built (`cmake/ApplyPatches.cmake`)

On every configure, included from the top-level `CMakeLists.txt` before the
first `ext/libevpl` reference:

1. A staging copy of `ext/libevpl` is made under
   `${CMAKE_BINARY_DIR}/ext-overlay-staging/` — every file except any `.git`
   component, text files normalised to LF, file modes preserved (libevpl ships
   executable scripts such as `scripts/netns_test_wrapper.sh`), non-text files
   verbatim.
2. Each series is applied into the staging tree with **git-context isolation**
   (`cmake -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
   GIT_CEILING_DIRECTORIES=<staging parent> git apply …`). Without it, a
   git-style patch applied from inside a work tree is silently **skipped**
   (`git apply` prints `Skipped patch …` and exits 0, touching nothing). The
   apply probes with `--check` first (git apply has no `--dry-run`); any
   `Skipped` line or non-zero status is a fatal error.
3. Each applied patch is verified: the post-apply staged file must differ
   (sha256) from the **post-normalisation pre-apply staged copy** — not from the
   pristine source, since those legitimately differ on a CRLF checkout — and the
   patch's sentinel token must be found in a target file. A `+++ /dev/null`
   target asserts the file is gone.
4. The staging tree is published to the consumed overlay at
   `${CMAKE_BINARY_DIR}/ext-overlay/libevpl` with copy-if-different semantics,
   pruning overlay entries no longer in staging. The build points
   `LIBEVPL_SRC`/`LIBEVPL_INC`/`PROMETHEUS_SRC`/`OTEL_SRC` at the overlay.

A configure-time self-test (`cmake/selftest/`) proves the isolation, the
`Skipped` detector, the sha/sentinel verifier and the overlay refresh each work.

## Layout

```
patches/
  README.md
  <repo>/
    series        ordered list of patch filenames — the SOLE ordering authority
    series.sha    pinned base SHA + per-(patch,file) sentinel tokens
    NNNN-<slug>.patch
```

`<repo>` names the overlaid tree, e.g. `libevpl-xdrzcc` for the nested xdrzcc
copy under libevpl. `series.sha` also declares `repo-root`, the path of the
patched repo relative to the libevpl overlay root (`ext/xdrzcc`), which
`git apply --directory` uses.

* **`series`** is the only thing that decides whether and in what order a patch
  is applied. A `.patch` file present on disk but absent from `series` is not
  applied.
* **`series.sha`** records the base SHA the series was authored against (so an
  upstream advance past it produces a configure warning) and, per
  `(patch, file)`, a sentinel token that must appear in the patched file and
  must be **absent** from the pristine source.

## Adding a patch

1. Author the change against the overlay, then capture it as a git-style diff
   (`git diff` / `git format-patch` output) with paths relative to the repo root
   (`-p1`, i.e. `a/…` `b/…`). Save it as `patches/<repo>/NNNN-<slug>.patch`.
2. Add the filename to `patches/<repo>/series` at the right position.
3. Add a `sentinel <patch> <file> <token>` row to `series.sha` for a string the
   patch introduces (and that is absent upstream).
4. Reconfigure; the apply, verify and self-test run automatically.

## Dropping a patch (upstream landed it)

When the fix is upstreamed and the submodule is bumped past the series base,
configure prints a warning. To drop the patch: remove its line from `series`,
remove its `sentinel` row(s) from `series.sha`, delete the `.patch` file, and —
when the submodule pin is advanced — update `base` in `series.sha`. Reconfigure
to confirm the overlay still builds.

## Overlay dev ergonomics

Edit the **patch**, never the overlay. The overlay under
`${CMAKE_BINARY_DIR}/ext-overlay/libevpl` is regenerated from the pristine
submodule plus the series on every reconfigure, so any hand-edit there is lost.
To iterate: change the `.patch` (or the pristine submodule), reconfigure, and
the overlay is rebuilt. Touching any pristine libevpl file or any series/patch
file re-triggers configure.

## `COVERAGE_INCLUDE_EXT=1` caveat

`etc/coverage-report.sh` treats everything under the build directory as
generated code and excludes it. Because the libevpl that actually compiles now
lives in the overlay **under the build directory**, it would be excluded even
when `COVERAGE_INCLUDE_EXT=1` asks for the bundled `ext/` projects. So that
script's include case carves the overlay path
(`<build>/ext-overlay/libevpl`) back in, keeping libevpl covered. If you move or
rename the overlay, update that carve-out too.
