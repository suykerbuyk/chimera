# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
"""Unit tests for tools/privci/compare.py.

Covers the R1 compare semantics (satisfied/mismatch/stale expect entries, the
presence-not-non-skip --require limitation c-L2, the exit-code table and its
exit-1-dominates-exit-2 ordering), the converter-format fixtures (built by
running the real scripts/*_to_junit.py converters), and that
builders/priv/section3/{expect,require}.txt parse and match those formats.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # tools/privci
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SCRIPTS = os.path.join(REPO, "scripts")
FIX = os.path.join(HERE, "fixtures")
SECTION3 = os.path.join(REPO, "builders", "priv", "section3")

import compare


# --------------------------------------------------------------------------- #
# Helpers to build synthetic run directories.
# --------------------------------------------------------------------------- #
def _testcase(name, status):
    esc = name.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    attr = 'name="%s" classname="t"' % esc
    if status == compare.FAILED:
        return '  <testcase %s><failure>boom</failure></testcase>' % attr
    if status == compare.SKIPPED:
        return '  <testcase %s><skipped/></testcase>' % attr
    return '  <testcase %s/>' % attr


def _write_junit(path, cases):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    body = "\n".join(_testcase(n, s) for n, s in cases)
    with open(path, "w") as handle:
        handle.write('<testsuite name="t" tests="%d">\n%s\n</testsuite>\n'
                     % (len(cases), body))


def build_side(root, ctest=None, junit=None, meta=None, leg="legA"):
    """Create one run dir.

    ctest: list of (name, status) written to <root>/<leg>/ctest.xml
    junit: {relpath: [(name, status), ...]} written to <root>/<leg>/<relpath>
    meta:  {key: value} written to <root>/meta.txt
    """
    legdir = os.path.join(root, leg)
    os.makedirs(legdir, exist_ok=True)
    if ctest is not None:
        _write_junit(os.path.join(legdir, "ctest.xml"), ctest)
    for rel, cases in (junit or {}).items():
        _write_junit(os.path.join(legdir, rel), cases)
    if meta is not None:
        with open(os.path.join(root, "meta.txt"), "w") as handle:
            for key, val in meta.items():
                handle.write("%s: %s\n" % (key, val))
    return root


def _effective_lines(path):
    """The lines parse_expect/parse_require treat as entries.

    Mirrors the parser's own skip rule (non-blank, not a ``#`` comment) so a
    parse-count assertion can be derived from the file instead of hard-coded --
    editing expect.txt/require.txt then cannot silently rot the count.
    """
    out = []
    with open(path) as handle:
        for raw in handle:
            stripped = raw.strip()
            if stripped and not stripped.startswith("#"):
                out.append(stripped)
    return out


class CompareCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.mkdtemp(prefix="privci-cmp-")

    def tearDown(self):
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def _dir(self, name):
        path = os.path.join(self._tmp, name)
        os.makedirs(path, exist_ok=True)
        return path

    def _file(self, name, text):
        path = os.path.join(self._tmp, name)
        with open(path, "w") as handle:
            handle.write(text)
        return path

    def _compare(self, base, head, expect=None, require=None):
        epath = self._file("expect.txt", expect) if expect is not None else None
        rpath = self._file("require.txt", require) if require is not None else None
        cmp, _ = compare.run(base, head, epath, rpath,
                             os.path.join(head, "compare.md"))
        return cmp


# --------------------------------------------------------------------------- #
# Transition classes and the exit-code table.
# --------------------------------------------------------------------------- #
class TestSemantics(CompareCase):
    def test_every_transition_class(self):
        base = build_side(self._dir("b"), junit={"j-junit/x.xml": [
            ("pp", compare.PASSED), ("pf", compare.PASSED),
            ("fp", compare.FAILED), ("ps", compare.PASSED),
            ("gone", compare.PASSED)]})
        head = build_side(self._dir("h"), junit={"j-junit/x.xml": [
            ("pp", compare.PASSED), ("pf", compare.FAILED),
            ("fp", compare.PASSED), ("ps", compare.SKIPPED),
            ("new", compare.PASSED)]})
        cmp = self._compare(base, head)
        names = lambda ks: {k.split("::", 1)[1] for k in ks}
        self.assertEqual(names(cmp.unexpected), {"pf"})
        self.assertEqual(names(cmp.fixed), {"fp"})
        self.assertEqual(names(cmp.pass_skip), {"ps"})
        self.assertEqual(names(cmp.appeared), {"new"})
        self.assertEqual(names(cmp.disappeared), {"gone"})
        self.assertEqual(cmp.exit_code, 1)          # unexpected pf

    def test_expect_satisfied_exit_0(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.FAILED)]})
        cmp = self._compare(base, head, expect="Failed->Failed  ^foo$\n"
                            "Passed->Failed  ^foo$\n")
        # The Passed->Failed entry is satisfied, so foo is waived (not an
        # unexpected new failure).  The Failed->Failed entry matches foo too but
        # observes Passed->Failed, so it is a mismatch (not stale: it matched).
        self.assertEqual([e.verdict for e in cmp.expects],
                         ["mismatch", "satisfied"])
        self.assertEqual(cmp.unexpected, [])
        self.assertEqual(cmp.exit_code, 2)          # the mismatch entry

    def test_clean_satisfied_only_exit_0(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.FAILED)]})
        cmp = self._compare(base, head, expect="Passed->Failed  ^foo$\n")
        self.assertEqual(cmp.expects[0].verdict, "satisfied")
        self.assertEqual(cmp.exit_code, 0)

    def test_listed_PP_observed_PF_is_nonzero(self):
        """Review-cited: listed Passed->Passed, observed Passed->Failed."""
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.FAILED)]})
        cmp = self._compare(base, head, expect="Passed->Passed  ^foo$\n")
        self.assertEqual(cmp.expects[0].verdict, "mismatch")
        # It is a mismatch (exit-2) AND an unexpected new failure (exit-1);
        # exit 1 dominates.
        self.assertEqual([k.split("::", 1)[1] for k in cmp.unexpected], ["foo"])
        self.assertNotEqual(cmp.exit_code, 0)
        self.assertEqual(cmp.exit_code, 1)

    def test_listed_transition_observed_differently_exit_2(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("bar", compare.FAILED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("bar", compare.FAILED)]})
        cmp = self._compare(base, head, expect="Failed->Passed  ^bar$\n")
        self.assertEqual(cmp.expects[0].verdict, "mismatch")
        self.assertEqual(cmp.unexpected, [])
        self.assertEqual(cmp.exit_code, 2)

    def test_stale_entry_exit_2(self):
        """Review-cited: a stale expect entry -> exit 2."""
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        cmp = self._compare(base, head, expect="Passed->Passed  ^nosuch$\n")
        self.assertEqual(cmp.expects[0].verdict, "stale")
        self.assertEqual(cmp.exit_code, 2)

    def test_require_missing_one_side_exit_1(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("only", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("other", compare.PASSED)]})
        cmp = self._compare(base, head, require="^only$\n")
        self.assertEqual(len(cmp.missing_required), 1)
        self.assertEqual(cmp.exit_code, 1)

    def test_require_missing_both_sides_exit_1(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        cmp = self._compare(base, head, require="^nowhere$\n")
        self.assertEqual(len(cmp.missing_required), 1)
        self.assertEqual(cmp.exit_code, 1)

    def test_require_passed_to_skipped_still_exit_0(self):
        """c-L2: a required case going Passed->Skipped still satisfies --require."""
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("req1", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("req1", compare.SKIPPED)]})
        cmp = self._compare(base, head, require="^req1$\n")
        self.assertEqual(cmp.missing_required, [])
        self.assertEqual([k.split("::", 1)[1] for k in cmp.pass_skip], ["req1"])
        self.assertEqual(cmp.exit_code, 0)

    def test_same_case_listed_PP_makes_skip_exit_2(self):
        """c-L2 companion: listing the same case Passed->Passed gates the skip."""
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("req1", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("req1", compare.SKIPPED)]})
        cmp = self._compare(base, head,
                            expect="Passed->Passed  ^req1$\n",
                            require="^req1$\n")
        self.assertEqual(cmp.expects[0].verdict, "mismatch")
        self.assertEqual(cmp.unexpected, [])        # Skipped is not a failure
        self.assertEqual(cmp.exit_code, 2)

    def test_image_id_mismatch_exit_1(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]},
                          meta={"image_id": "sha256:aaaa", "sha": "03e81248"})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]},
                          meta={"image_id": "sha256:bbbb", "sha": "61d14614"})
        cmp = self._compare(base, head)
        self.assertTrue(cmp.id_mismatch)
        self.assertEqual(cmp.exit_code, 1)

    def test_image_id_equal_no_fail(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]},
                          meta={"image_id": "sha256:aaaa"})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]},
                          meta={"image_id": "sha256:aaaa"})
        cmp = self._compare(base, head)
        self.assertFalse(cmp.id_mismatch)
        self.assertEqual(cmp.exit_code, 0)

    def test_missing_meta_does_not_fail(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]})
        cmp = self._compare(base, head)
        self.assertFalse(cmp.id_mismatch)
        self.assertEqual(cmp.exit_code, 0)

    def test_qb_digest_mismatch_exit_1(self):
        base = build_side(self._dir("b"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]},
                          meta={"qb_yaml_digest": "sha256:1111"})
        head = build_side(self._dir("h"),
                          junit={"j-junit/x.xml": [("foo", compare.PASSED)]},
                          meta={"qb_yaml_digest": "sha256:2222"})
        cmp = self._compare(base, head)
        self.assertTrue(cmp.qb_mismatch)
        self.assertEqual(cmp.exit_code, 1)

    def test_zero_testcase_leg(self):
        base = build_side(self._dir("b"), ctest=[])
        head = build_side(self._dir("h"), ctest=[])
        cmp = self._compare(base, head, require=None)
        self.assertEqual(cmp.keys, [])
        self.assertEqual(cmp.exit_code, 0)

    def test_file_qualifier_narrows_source(self):
        junit = {"wpts-junit/x.xml": [("dup", compare.PASSED)],
                 "pynfs-junit/y.xml": [("dup", compare.PASSED)]}
        base = build_side(self._dir("b"), junit=junit)
        head = build_side(self._dir("h"), junit={
            "wpts-junit/x.xml": [("dup", compare.FAILED)],
            "pynfs-junit/y.xml": [("dup", compare.PASSED)]})
        # With file=, only the wpts dup matches; it is waived -> exit 0.
        cmp = self._compare(base, head,
                            expect="Passed->Failed  ^dup$  file=wpts-junit/x.xml\n")
        self.assertEqual(len(cmp.expects[0].matched), 1)
        self.assertEqual(cmp.exit_code, 0)
        # Without file=, the entry matches both; the pynfs dup is Passed->Passed,
        # so the entry mismatches and the wpts failure is not waived -> exit 1.
        cmp2 = self._compare(base, head, expect="Passed->Failed  ^dup$\n")
        self.assertEqual(len(cmp2.expects[0].matched), 2)
        self.assertEqual(cmp2.expects[0].verdict, "mismatch")
        self.assertEqual(cmp2.exit_code, 1)

    def test_ctest_name_part_matching(self):
        base = build_side(self._dir("b"), ctest=[
            ("chimera/posix/pjd/open/00", compare.PASSED),
            ("chimera/posix/ceph/open/00", compare.PASSED)])
        head = build_side(self._dir("h"), ctest=[
            ("chimera/posix/pjd/open/00", compare.PASSED),
            ("chimera/posix/ceph/open/00", compare.PASSED)])
        cmp = self._compare(base, head,
                            require="^chimera/posix/pjd/(open|mkdir)/\n")
        self.assertIn("ctest::chimera/posix/pjd/open/00", cmp.keys)
        self.assertEqual(cmp.missing_required, [])
        self.assertEqual(len(cmp.requires[0].base_hits), 1)   # pjd, not ceph
        self.assertEqual(cmp.exit_code, 0)


# --------------------------------------------------------------------------- #
# Parser edge cases.
# --------------------------------------------------------------------------- #
class TestParsing(CompareCase):
    def test_bad_transition_rejected(self):
        path = self._file("e.txt", "Passed->Bogus  ^x$\n")
        self.assertRaises(ValueError, compare.parse_expect, path)

    def test_missing_arrow_rejected(self):
        path = self._file("e.txt", "Passed  ^x$\n")
        self.assertRaises(ValueError, compare.parse_expect, path)

    def test_comments_and_blanks_skipped(self):
        path = self._file("e.txt",
                          "# header\n\n   \nPassed->Passed  ^x$  # why\n")
        entries = compare.parse_expect(path)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].reason, "why")
        self.assertEqual(entries[0].regex, "^x$")

    def test_absent_is_a_valid_endpoint(self):
        path = self._file("e.txt", "Absent->Passed  ^x$\n")
        entries = compare.parse_expect(path)
        self.assertEqual((entries[0].frm, entries[0].to),
                         (compare.ABSENT, compare.PASSED))


# --------------------------------------------------------------------------- #
# Converter-format fixtures (L-b): run the real converters, pin the names.
# --------------------------------------------------------------------------- #
class TestConverterFormats(CompareCase):
    def _run(self, script, inp, suite):
        out = os.path.join(self._tmp, "out.xml")
        subprocess.check_call(
            [sys.executable, os.path.join(SCRIPTS, script), inp, out, suite])
        return out

    def test_smbtorture_names_carry_suite_prefix(self):
        out = self._run("smbtorture_to_junit.py",
                        os.path.join(FIX, "smbtorture_create.txt"), "smb2.create")
        cases = dict(compare._parse_junit(out))
        self.assertIn("smb2.create.bench-holepunch", cases)   # prefix present
        self.assertEqual(cases["smb2.create.bench-holepunch"], compare.PASSED)
        self.assertEqual(cases["smb2.create.nulldacl"], compare.FAILED)
        self.assertEqual(cases["smb2.create.async-dup"], compare.SKIPPED)
        # The committed golden carries the same cases as fresh converter output
        # (compared parsed; the golden also has an inline SPDX comment header).
        golden = dict(compare._parse_junit(
            os.path.join(FIX, "smbtorture-junit", "smb2_create.xml")))
        self.assertEqual(cases, golden)

    def test_wpts_names_and_filename(self):
        out = self._run("trx_to_junit.py",
                        os.path.join(FIX, "wpts_memfs_symlink.trx"), "wpts")
        cases = dict(compare._parse_junit(out))
        self.assertIn("CreateClose_SymbolicLinkInMiddle", cases)
        self.assertEqual(cases["CreateClose_SymbolicLinkInMiddle"], compare.PASSED)
        # The per-case file for memfs symlinks is wpts-junit/memfs_symlink.xml.
        golden_path = os.path.join(FIX, "wpts-junit", "memfs_symlink.xml")
        self.assertTrue(os.path.exists(golden_path))
        self.assertEqual(cases, dict(compare._parse_junit(golden_path)))

    def test_pynfs_code_attr_folded_into_name(self):
        # Real pynfs junit (trimmed on-host: podman-priv head 61d14614): the
        # code (OPEN7) is in `code=`, the description (testOPENClaimFH) in
        # `name=`.  compare.py folds them to "<code> <name>" so a require/expect
        # pattern can address a case by its code (\bOPEN7\b).  Removing the fold
        # in compare.py leaves the bare description as the key, which \bOPEN7\b
        # does not match -- this test then goes RED (the fix is shown capable of
        # failing, not merely read as correct).
        cases = dict(compare._parse_junit(
            os.path.join(FIX, "pynfs-junit", "combined_memfs_nfs4.1.xml")))
        self.assertIn("OPEN7 testOPENClaimFH", cases)      # code folded in
        self.assertIn("COUR3 testShareReservation00", cases)
        self.assertNotIn("testOPENClaimFH", cases)         # not left bare
        # the require.txt pattern matches only because of the fold.
        self.assertTrue(re.search(r"\bOPEN7\b", "OPEN7 testOPENClaimFH"))
        self.assertIsNone(re.search(r"\bOPEN7\b", "testOPENClaimFH"))

    def test_ctest_junit_name_format(self):
        # CTest --output-junit names are the full ctest test name; the pjd
        # require pattern matches these verbatim.
        cases = dict(compare._parse_junit(os.path.join(FIX, "ctest_pjd.xml")))
        self.assertIn("chimera/posix/pjd/open/00", cases)
        self.assertEqual(cases["chimera/posix/pjd/open/00"], compare.PASSED)


# --------------------------------------------------------------------------- #
# section3/expect.txt and require.txt parse and match the P2 formats.
# --------------------------------------------------------------------------- #
class TestSection3Files(CompareCase):
    # The suites chimera actually runs (post-b6eef6e1): smb2.mkdir is a single
    # case "smb2.mkdir" (kept here as .x to exercise the \b require pattern),
    # and the real durable suites are durable-v2-regressions /
    # durable-open-disconnect, not durable-v2-open / durable-open.
    SMBT_SUITES = ["smb2.create.x", "smb2.mkdir.x", "smb2.rename.x",
                   "smb2.streams.x", "smb2.dir.x",
                   "smb2.durable-v2-regressions.x",
                   "smb2.durable-open-disconnect.x"]
    # The one timestamps subtest whose '/'-name is refused post-confinement
    # (06bd87c6): Passed@base -> Failed@head by design, waived in expect.txt
    # via a per-subtest junit entry and a per-suite CTest rollup entry.
    TS_CASE = "smb2.timestamps.test_close_not_attrib"
    TS_JUNIT = "smbtorture-junit/smbtorture_smb2_timestamps_memfs.xml"
    TS_CTEST = "chimera/server/smb/smbtorture_smb2_timestamps_memfs"
    WPTS = ["CreateClose_SymbolicLinkInMiddle", "CreateClose_SymbolicLinkAtLast",
            "CreateClose_InvalidSymbolicLink",
            "CreateClose_CreateAndCloseDirWithDotDir",
            "CreateClose_CreateAndCloseDirWithDoubleDotDir",
            "CreateClose_CreateAndCloseFileWithDotDir",
            "CreateClose_CreateAndCloseFileWithDoubleDotDir"]
    PYNFS = ["OPEN7", "COUR3"]
    PJD = ["chimera/posix/pjd/open/00", "chimera/posix/pjd/mkdir/00",
           "chimera/posix/pjd/rename/00", "chimera/posix/pjd/symlink/00",
           "chimera/posix/pjd/unlink/00"]

    def _section3_side(self, name, head=False):
        # The waived timestamps subtest and its CTest rollup are Passed on base
        # and Failed on head, matching the two expect.txt deviation entries;
        # every other case is Passed on both sides.
        root = self._dir(name)
        ts = compare.FAILED if head else compare.PASSED
        build_side(root, leg="smbtorture",
                   junit={"smbtorture-junit/suites.xml":
                          [(n, compare.PASSED) for n in self.SMBT_SUITES],
                          self.TS_JUNIT: [(self.TS_CASE, ts)]})
        build_side(root, leg="wpts",
                   junit={"wpts-junit/memfs_symlink.xml":
                          [(n, compare.PASSED) for n in self.WPTS]})
        build_side(root, leg="pynfs",
                   junit={"pynfs-junit/combined_memfs_nfs4.1.xml":
                          [(n, compare.PASSED) for n in self.PYNFS]})
        build_side(root, leg="rest",
                   ctest=[(n, compare.PASSED) for n in self.PJD]
                   + [(self.TS_CTEST, ts)])
        return root

    def test_section3_files_parse(self):
        epath = os.path.join(SECTION3, "expect.txt")
        rpath = os.path.join(SECTION3, "require.txt")
        expects = compare.parse_expect(epath)
        requires = compare.parse_require(rpath)
        # Parse-derived, not a magic total: exactly one parsed entry per
        # non-comment, non-blank line, so editing the files cannot silently rot
        # this count (the old 3 / 8+7+2+1 literals rotted when b6eef6e1 landed).
        self.assertEqual(len(expects), len(_effective_lines(epath)))
        self.assertEqual(len(requires), len(_effective_lines(rpath)))
        # Structural invariants, asserted rather than assumed.
        for e in expects:
            self.assertIn(e.frm, compare.STATUSES)
            self.assertIn(e.to, compare.STATUSES)
            re.compile(e.regex)
        for r in requires:
            re.compile(r.regex)
        # The entries this unit's repair depends on are actually present.
        expect_regexes = {e.regex for e in expects}
        self.assertIn(r"smb2\.timestamps\.test_close_not_attrib$",
                      expect_regexes)
        self.assertIn(r"CreateClose_SymbolicLinkInMiddle$", expect_regexes)

    def test_section3_clean_run_exit_0_no_stale(self):
        base = self._section3_side("b")
        head = self._section3_side("h", head=True)
        cmp, _ = compare.run(base, head,
                             os.path.join(SECTION3, "expect.txt"),
                             os.path.join(SECTION3, "require.txt"),
                             os.path.join(head, "compare.md"))
        stale = [e.raw for e in cmp.expects if e.verdict == "stale"]
        self.assertEqual(stale, [], "stale expect entries: %s" % stale)
        self.assertTrue(all(e.verdict == "satisfied" for e in cmp.expects))
        missing = [r.raw for r in cmp.missing_required]
        self.assertEqual(missing, [], "unmatched require patterns: %s" % missing)
        self.assertEqual(cmp.exit_code, 0)

    def test_section3_symlink_regression_is_caught(self):
        """A symlink case going Passed->Failed is an unexpected failure (exit 1).

        The head is an otherwise-clean section-3 side (the timestamps waiver
        satisfied, not mismatched) with ONLY the symlink flipped to Failed, so
        the exit-1 signal isolates the symlink regression instead of mixing it
        with a waiver mismatch.
        """
        base = self._section3_side("b")
        head = self._section3_side("h", head=True)
        # Flip only the symlink; rewriting the wpts leg in place.
        build_side(head, leg="wpts", junit={"wpts-junit/memfs_symlink.xml":
                   [("CreateClose_SymbolicLinkInMiddle", compare.FAILED)]
                   + [(n, compare.PASSED) for n in self.WPTS[1:]]})
        cmp, _ = compare.run(base, head,
                             os.path.join(SECTION3, "expect.txt"),
                             os.path.join(SECTION3, "require.txt"),
                             os.path.join(head, "compare.md"))
        # Listed Passed->Passed but observed Passed->Failed: mismatch + unexpected.
        self.assertEqual(cmp.exit_code, 1)
        # Isolated: the symlink is the sole unexpected new failure, and the
        # timestamps waiver entries stay satisfied (no spurious mismatch).
        self.assertEqual({k.split("::", 1)[1] for k in cmp.unexpected},
                         {"CreateClose_SymbolicLinkInMiddle"})
        ts_entries = [e for e in cmp.expects if "timestamps" in e.regex]
        self.assertTrue(ts_entries)
        self.assertTrue(all(e.verdict == "satisfied" for e in ts_entries))

    def test_matcher_fixes_present_and_behave(self):
        """One regression assertion per b6eef6e1 require-matcher fix.

        The positive matches are exercised with the exact regex pulled from
        require.txt (not a re-typed copy), so a typo in the production pattern
        cannot be masked by the same typo in the test.  The negative direction
        uses the now-removed old forms, each backed by assertNotIn to prove it
        is really gone from production.
        """
        requires = compare.parse_require(os.path.join(SECTION3, "require.txt"))
        regexes = {r.regex for r in requires}

        def prod(pattern):
            """The single production require regex equal to `pattern`."""
            self.assertIn(pattern, regexes)
            return next(r for r in regexes if r == pattern)

        # mkdir: the real case is named exactly "smb2.mkdir".  The production
        # word-boundary form matches it; the old trailing-dot form (removed
        # from require.txt) never could -- that was the bug.
        mkdir = prod(r"smb2\.mkdir\b")
        self.assertTrue(re.search(mkdir, "smb2.mkdir"))
        self.assertNotIn(r"smb2\.mkdir\.", regexes)
        self.assertIsNone(re.search(r"smb2\.mkdir\.", "smb2.mkdir"))  # old form

        # durable: the production patterns are the real suite names and match
        # them; the old durable-open / -v2-open forms are gone and would not
        # even substring-match the real names.
        v2 = prod(r"smb2\.durable-v2-regressions\.")
        disc = prod(r"smb2\.durable-open-disconnect\.")
        self.assertTrue(re.search(v2, "smb2.durable-v2-regressions.replay"))
        self.assertTrue(re.search(disc, "smb2.durable-open-disconnect.reopen"))
        self.assertNotIn(r"smb2\.durable-v2-open\.", regexes)
        self.assertNotIn(r"smb2\.durable-open\.", regexes)
        self.assertIsNone(re.search(r"smb2\.durable-open\.",           # old form
                                    "smb2.durable-open-disconnect.reopen"))
        self.assertIsNone(re.search(r"smb2\.durable-v2-open\.",        # old form
                                    "smb2.durable-v2-regressions.replay"))


if __name__ == "__main__":
    unittest.main()
