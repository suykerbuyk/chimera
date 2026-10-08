#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

"""Unit tests for builders/priv/priv-ci.sh leg selection and command extraction.

These exercise the no-sudo, no-build parts of the host driver (Plan rev 4,
section 2.1 and section 3 "test_extract.py"): the substituted leg scripts, the
build keys, the unmatched-word report, continuation-line skipping, and the
guards (disallowed task, unknown PRIV_MODE, empty PRIV_SUDO, PRIV_DIR sanity,
missing marker).  Everything runs against the real qb.yaml and qb-ci.sh in the
worktree; dry-run works under any uid (this file runs as root in CI's rest leg).
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent          # builders/priv/tests
PRIV_CI = HERE.parent / "priv-ci.sh"            # builders/priv/priv-ci.sh
ROOT = HERE.parents[2]                           # repo/worktree top level
BASH = shutil.which("bash") or "/bin/bash"


def run(sub, *, priv_dir, legs=None, mode="release", sudo=None,
        clean_store=None, args=()):
    """Run priv-ci.sh <sub> with a controlled environment; capture everything."""
    env = dict(os.environ)
    env["PRIV_DIR"] = str(priv_dir)
    env["PRIV_MODE"] = mode
    for k in ("PRIV_DRY_RUN", "PRIV_REF", "PRIV_CLEAN_STORE", "PRIV_FRESH"):
        env.pop(k, None)
    if legs is not None:
        env["PRIV_LEGS"] = legs
    if sudo is not None:
        env["PRIV_SUDO"] = sudo
    if clean_store is not None:
        env["PRIV_CLEAN_STORE"] = clean_store
    return subprocess.run(
        [BASH, str(PRIV_CI), sub, *args],
        env=env, capture_output=True, text=True,
    )


class Base(unittest.TestCase):
    def setUp(self):
        # A PRIV_DIR outside the repo and outside ~/.cache/chimera.
        self.priv = tempfile.mkdtemp(prefix="privci-test-")
        self.addCleanup(shutil.rmtree, self.priv, ignore_errors=True)

    def dry(self, legs, mode="release"):
        r = run("dry-run", priv_dir=self.priv, legs=legs, mode=mode)
        self.assertEqual(r.returncode, 0, msg=f"dry-run failed:\n{r.stderr}")
        return r

    def script(self, leg):
        return (Path(self.priv) / "dry" / f"{leg}.sh").read_text()

    def tsv(self):
        rows = {}
        for line in (Path(self.priv) / "dry" / "legs.tsv").read_text().splitlines():
            leg, key, path = line.split("\t")
            rows[leg] = (key, path)
        return rows


class TestExtraction(Base):
    def test_ctest_rest_release(self):
        """Folded cmake joined, CTEST_FILTER substituted, --output-junit added."""
        self.dry("ctest_*_rest", mode="release")
        s = self.script("ctest_release_rest")
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", s)
        self.assertIn("-S /context -B /build", s)          # folded scalar joined
        # Single-quoted regex preserved; textual substitution of CTEST_FILTER.
        self.assertIn("-LE '^pynfs$|^smbtorture$|^wpts$|^kvm$'", s)
        self.assertIn("--output-junit /results/ctest_release_rest/ctest.xml", s)
        # The ctest line is the one that carries --output-junit.
        ctest_lines = [ln for ln in s.splitlines() if ln.startswith("ctest ")]
        self.assertEqual(len(ctest_lines), 1)
        self.assertTrue(ctest_lines[0].endswith(
            "--output-junit /results/ctest_release_rest/ctest.xml"))

    def test_quint_no_variables(self):
        """A task with no variables: block still extracts."""
        self.dry("ctest_quint_*", mode="release")
        s = self.script("ctest_quint_release")
        self.assertIn("ctest -L quint", s)
        self.assertIn("--output-junit /results/ctest_quint_release/ctest.xml", s)

    def test_kvm_quoting_and_single_quoted_scalar(self):
        """KVM: MODE/PLATFORM/KVM_FILTER substitution and a single-quoted scalar."""
        self.dry("kvm_release_ubuntu2604", mode="release")
        s = self.script("kvm_release_ubuntu2604")
        # Single-quoted YAML scalar is unwrapped (no surrounding ' left).
        self.assertIn("[ -w /dev/kvm ] || { echo", s)
        self.assertNotIn("'[ -w /dev/kvm ]", s)
        # KVM_FILTER keeps its inner single quotes, unquoted flag outside.
        self.assertIn("-E '/(nfstest|ltp)/'", s)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", s)     # ${MODE}
        self.assertIn("ninja kvm_ubuntu2604_image", s)     # ${PLATFORM}
        self.assertIn("--output-junit /results/kvm_release_ubuntu2604/ctest.xml", s)

    def test_build_keys_differ_by_mode(self):
        """M4: ctest_kvm Release vs Debug get different build keys."""
        self.dry("kvm_release_ubuntu2604", mode="release")
        rel = self.tsv()["kvm_release_ubuntu2604"][0]
        self.dry("kvm_debug_ubuntu2604", mode="debug")
        dbg = self.tsv()["kvm_debug_ubuntu2604"][0]
        self.assertNotEqual(rel, dbg)
        self.assertTrue(rel.startswith("wt/ctest_kvm-"))
        self.assertTrue(dbg.startswith("wt/ctest_kvm-"))

    def test_legs_tsv_shape(self):
        """legs.tsv has leg, non-empty build key, and the in-container script."""
        self.dry("ctest_*_rest", mode="release")
        key, path = self.tsv()["ctest_release_rest"]
        self.assertTrue(key.startswith("wt/ctest_release-"))
        self.assertEqual(path, "/results/ctest_release_rest/leg.sh")


class TestSelectionGuards(Base):
    def test_unmatched_word_reported_by_name(self):
        r = run("list", priv_dir=self.priv, legs="ctest_bogus_nope")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("ctest_bogus_nope", r.stderr)

    def test_disallowed_static_analysis_skips_continuation(self):
        """static_analysis is refused by task, and its '#   on failure:'
        continuation line is skipped (no parse error)."""
        r = run("list", priv_dir=self.priv, legs="static_analysis_release_amd64")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("static_analysis_release", r.stderr)
        self.assertNotIn("unexpected line", r.stderr)

    def test_disallowed_coverage_skips_downloads_continuation(self):
        r = run("list", priv_dir=self.priv, legs="coverage")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("ctest_coverage", r.stderr)
        self.assertNotIn("unexpected line", r.stderr)

    def test_unknown_mode(self):
        r = run("list", priv_dir=self.priv, legs="ctest_*_rest", mode="fast")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("PRIV_MODE", r.stderr)

    def test_empty_sudo_refused(self):
        r = run("dry-run", priv_dir=self.priv, legs="ctest_*_rest", sudo="")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("PRIV_SUDO", r.stderr)


class TestPrivDirGuards(Base):
    def test_priv_dir_is_home(self):
        r = run("list", priv_dir=os.path.expanduser("~"), legs="ctest_*_rest")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("PRIV_DIR", r.stderr)

    def test_priv_dir_is_cache_chimera(self):
        r = run("list", priv_dir=os.path.expanduser("~/.cache/chimera"),
                legs="ctest_*_rest")
        self.assertNotEqual(r.returncode, 0)

    def test_priv_dir_inside_repo(self):
        r = run("list", priv_dir=str(ROOT / "build-priv-xyz"), legs="ctest_*_rest")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("inside the repo", r.stderr)

    def test_priv_dir_relative_refused(self):
        r = run("list", priv_dir="relative/dir", legs="ctest_*_rest")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("absolute", r.stderr)


class TestMarker(Base):
    def test_clean_without_marker_refused(self):
        r = run("clean", priv_dir=self.priv)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(".chimera-podman-priv", r.stderr)

    def test_clean_with_marker_succeeds(self):
        (Path(self.priv) / ".chimera-podman-priv").write_text("")
        # A leftover subdir to prove clean removes it.
        (Path(self.priv) / "runs").mkdir()
        r = run("clean", priv_dir=self.priv)
        self.assertEqual(r.returncode, 0, msg=r.stderr)
        self.assertFalse((Path(self.priv) / "runs").exists())


class TestDryRunAnyUid(Base):
    def test_dry_run_prints_wrapper_and_legs(self):
        r = self.dry("ctest_*_rest ctest_quint_*", mode="release")
        self.assertIn("scoped podman wrapper", r.stdout)
        self.assertIn("--tmpdir /run/chimera-priv", r.stdout)
        self.assertIn("--root ", r.stdout)
        self.assertIn("--runroot ", r.stdout)
        self.assertIn("--volumepath ", r.stdout)
        # Nothing was written outside PRIV_DIR/dry/.
        self.assertTrue((Path(self.priv) / "dry" / "legs.tsv").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
