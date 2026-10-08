#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
"""Compare two privileged-leg run directories and gate regressions.

This is the comparison tool for the opt-in privileged Podman target
(``builders/priv``).  Each privileged ``test`` / ``compare`` / ``diff`` writes
one *run directory* per side; this script reads two of them, classifies every
testcase transition from the base side to the head side, checks a pre-registered
expectation and presence contract, and exits with a code the operator can gate
on.

Run-directory layout (produced by ``builders/priv/run-legs.sh``)
---------------------------------------------------------------
A run dir holds one subdirectory per leg.  Within a leg::

    <run>/<leg>/ctest.xml                     CTest --output-junit (one
                                              <testcase> per CTest test)
    <run>/<leg>/<suite>-junit/<file>.xml      per-case JUnit written by the
                                              consolidated entries (pynfs,
                                              smbtorture, wpts)

and, at the top of the run dir::

    <run>/meta.txt                            run identity + recorded deviations

Collection keys (plan section 2.3)
----------------------------------
* Each CTest ``<testcase>`` is keyed ``ctest::<test name>`` so, e.g., pjd and
  ceph names match per test across the two sides.
* Each per-case JUnit ``<testcase>`` is keyed ``<suite>-junit/<file>::<name>``,
  i.e. the JUnit path *relative to its leg* joined to the case name.  A given
  consolidated suite runs in exactly one leg, so these keys are leg-independent.

Patterns in ``--expect`` and ``--require`` are matched (``re.search``) against
the **name part** of the key, after ``::``.  An optional ``file=<glob>``
qualifier narrows the **source** (the part before ``::``: ``ctest`` or the
``<suite>-junit/<file>`` path) with :func:`fnmatch.fnmatch`.

Each case has exactly one status on each side: ``Passed``, ``Failed``,
``Skipped`` or ``Absent`` (the key is not present on that side).

``--expect`` format (plan R1)
-----------------------------
One entry per line::

    <FROM>-><TO>  <name-regex>  [file=<glob>]  # reason

``FROM`` and ``TO`` are each one of ``Passed``/``Failed``/``Skipped``/``Absent``
and carry the entry's whole transition.  An entry is **satisfied** only if every
case it matches shows exactly ``FROM`` on base and ``TO`` on head.  An entry that
matches no case is **stale**; an entry whose matched cases do not all show
``FROM->TO`` is a **mismatch**.  A listed case can never be *waived* by being
listed: a ``Passed->Passed`` case observed ``Passed->Failed`` is a mismatch
(exit 2) *and* an unexpected new failure (exit 1).

``--require`` format
--------------------
One regex per line (``file=<glob>`` also allowed), matched against the name part.
It must match at least one case on **each** side.

Known limitation -- ``--require`` guards presence, not non-skip (plan c-L2)
--------------------------------------------------------------------------
A ``--require`` pattern is satisfied by any matched case present on each side,
*including a Skipped one*.  So a required case that regresses ``Passed->Skipped``
still satisfies ``--require`` (present on both sides, just skipped on head).  That
regression is **reported** in the Passed<->Skipped section but, with no exit-1 or
exit-2 condition of its own, exits 0.  ``--require`` answers only "is this
section-3 criterion present on both sides at all" -- the presence question it was
added for.  To gate a specific case against silent skipping, list it in the
expect file as ``Passed->Passed``; an observed ``Passed->Skipped`` is then a
mismatch (exit 2).

Exit status (plan R1)
---------------------
* 0 -- everything clean.
* 1 -- an unexpected new failure (a ``Passed->Failed`` not covered by a
  *satisfied* expect entry); a required pattern missing on a side; the image
  ``.Id``\\ s differ; or the qb.yaml digests differ (the last is meaningful only
  for ``diff A B`` across tooling versions -- it cannot occur in ``compare``).
* 2 -- operator review required: at least one ``--expect`` entry is a mismatch or
  stale, and no exit-1 condition holds.

Exit 1 dominates exit 2.

meta.txt contract (read here; written by priv-ci.sh in P1)
----------------------------------------------------------
``meta.txt`` is ``key: value`` (``=`` also accepted); unrecognized lines are
ignored, so it may also carry free-text deviations.  Keys read here:

    image_id        the image config ``.Id`` the run used (checked, not .Digest)
    repo_digest     the rootless RepoDigest (reported only)
    sha             the source commit of this side (reported only)
    qb_yaml_digest  digest of the qb.yaml the legs came from

A missing file or key degrades gracefully: the field shows ``(unknown)`` and the
corresponding exit-1 check is skipped rather than firing.  (Boundary note for the
Chair: these key names must match what P1's priv-ci.sh writes.)
"""
import argparse
import fnmatch
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

PASSED, FAILED, SKIPPED, ABSENT = "Passed", "Failed", "Skipped", "Absent"
STATUSES = (PASSED, FAILED, SKIPPED, ABSENT)


# --------------------------------------------------------------------------- #
# Collection
# --------------------------------------------------------------------------- #
def _localname(tag):
    """Strip any XML namespace from an element tag."""
    return tag.rsplit("}", 1)[-1]


def _case_status(tc):
    """Status of one <testcase> element."""
    kids = {_localname(c.tag) for c in tc}
    if "failure" in kids or "error" in kids:
        return FAILED
    if "skipped" in kids:
        return SKIPPED
    attr = (tc.get("status") or "").lower()
    if attr in ("fail", "failed", "error"):
        return FAILED
    if attr in ("skipped", "notrun", "disabled", "skip"):
        return SKIPPED
    return PASSED


def _parse_junit(path):
    """Yield (name, status) for each <testcase> in a JUnit/CTest XML file."""
    try:
        root = ET.parse(path).getroot()
    except Exception:
        return
    for tc in root.iter():
        if _localname(tc.tag) != "testcase":
            continue
        yield (tc.get("name") or "?"), _case_status(tc)


def collect(run_dir):
    """Map each testcase key to (file, name, status) for one run dir."""
    cases = {}
    for path in sorted(glob.glob(os.path.join(run_dir, "**", "ctest.xml"),
                                 recursive=True)):
        for name, status in _parse_junit(path):
            cases["ctest::" + name] = ("ctest", name, status)
    for path in sorted(glob.glob(os.path.join(run_dir, "**", "*-junit", "*.xml"),
                                 recursive=True)):
        suite = os.path.basename(os.path.dirname(path))
        src = suite + "/" + os.path.basename(path)
        for name, status in _parse_junit(path):
            cases[src + "::" + name] = (src, name, status)
    return cases


def read_meta(run_dir):
    """Parse <run>/meta.txt into a lower-cased key->value dict."""
    meta = {}
    try:
        with open(os.path.join(run_dir, "meta.txt"), errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                for sep in (":", "="):
                    if sep in line:
                        key, val = line.split(sep, 1)
                        meta[key.strip().lower()] = val.strip()
                        break
    except OSError:
        pass
    return meta


def _meta_get(meta, *keys):
    for key in keys:
        if meta.get(key):
            return meta[key]
    return None


def image_id(meta):
    return _meta_get(meta, "image_id", "image.id", ".id", "id")


def repo_digest(meta):
    return _meta_get(meta, "repo_digest", "repodigest", "repo_digests")


def source_sha(meta):
    return _meta_get(meta, "sha", "source_sha", "commit")


def qb_digest(meta):
    return _meta_get(meta, "qb_yaml_digest", "qb_digest", "qb.yaml")


# --------------------------------------------------------------------------- #
# Expect / require parsing
# --------------------------------------------------------------------------- #
class Expect:
    __slots__ = ("frm", "to", "regex", "file_glob", "reason", "raw",
                 "matched", "verdict")

    def __init__(self, frm, to, regex, file_glob, reason, raw):
        self.frm = frm
        self.to = to
        self.regex = regex
        self.file_glob = file_glob
        self.reason = reason
        self.raw = raw
        self.matched = []
        self.verdict = None     # "satisfied" | "mismatch" | "stale"


class Require:
    __slots__ = ("regex", "file_glob", "raw", "base_hits", "head_hits")

    def __init__(self, regex, file_glob, raw):
        self.regex = regex
        self.file_glob = file_glob
        self.raw = raw
        self.base_hits = []
        self.head_hits = []


_COMMENT = re.compile(r"\s#")


def _strip_comment(line):
    """Return (body, reason) splitting off a trailing ``  # ...`` comment."""
    m = _COMMENT.search(line)
    if m:
        return line[:m.start()].rstrip(), line[m.start():].lstrip(" \t#").rstrip()
    return line.rstrip(), ""


def _split_file_qualifier(tokens):
    """Pull a trailing ``file=<glob>`` token out of a token list."""
    if tokens and tokens[-1].startswith("file="):
        return tokens[:-1], tokens[-1][len("file="):]
    return tokens, None


def parse_expect(path):
    entries = []
    with open(path, errors="replace") as handle:
        for lineno, raw in enumerate(handle, 1):
            stripped = raw.strip()
            if not stripped or stripped.startswith("#"):
                continue
            body, reason = _strip_comment(raw.rstrip("\n"))
            tokens = body.split()
            if not tokens or "->" not in tokens[0]:
                raise ValueError(
                    "%s:%d: expected '<FROM>-><TO> <regex> [file=<glob>]': %r"
                    % (path, lineno, raw.rstrip()))
            frm, to = tokens[0].split("->", 1)
            if frm not in STATUSES or to not in STATUSES:
                raise ValueError(
                    "%s:%d: FROM/TO must be one of %s: %r"
                    % (path, lineno, "/".join(STATUSES), tokens[0]))
            rest, file_glob = _split_file_qualifier(tokens[1:])
            regex = " ".join(rest)
            if not regex:
                raise ValueError(
                    "%s:%d: missing name regex" % (path, lineno))
            re.compile(regex)
            entries.append(Expect(frm, to, regex, file_glob, reason,
                                  raw.rstrip("\n")))
    return entries


def parse_require(path):
    entries = []
    with open(path, errors="replace") as handle:
        for lineno, raw in enumerate(handle, 1):
            stripped = raw.strip()
            if not stripped or stripped.startswith("#"):
                continue
            body, _ = _strip_comment(raw.rstrip("\n"))
            tokens = body.split()
            rest, file_glob = _split_file_qualifier(tokens)
            regex = " ".join(rest)
            if not regex:
                raise ValueError(
                    "%s:%d: missing require regex" % (path, lineno))
            re.compile(regex)
            entries.append(Require(regex, file_glob, raw.rstrip("\n")))
    return entries


# --------------------------------------------------------------------------- #
# Comparison
# --------------------------------------------------------------------------- #
class Comparison:
    def __init__(self, base, head, expects, requires, base_meta, head_meta):
        self.base = base
        self.head = head
        self.expects = expects
        self.requires = requires
        self.base_meta = base_meta
        self.head_meta = head_meta
        self.keys = sorted(set(base) | set(head))
        self._evaluate()

    def _status(self, side, key):
        rec = side.get(key)
        return rec[2] if rec else ABSENT

    def _file_name(self, key):
        rec = self.base.get(key) or self.head.get(key)
        return rec[0], rec[1]

    def _matches(self, regex, file_glob, key):
        src, name = self._file_name(key)
        if not re.search(regex, name):
            return False
        return file_glob is None or fnmatch.fnmatch(src, file_glob)

    def _evaluate(self):
        base, head = self.base, self.head
        self.satisfied_keys = set()

        # Expect entries.
        for e in self.expects:
            e.matched = [k for k in self.keys
                         if self._matches(e.regex, e.file_glob, k)]
            if not e.matched:
                e.verdict = "stale"
                continue
            ok = all(self._status(base, k) == e.frm
                     and self._status(head, k) == e.to for k in e.matched)
            e.verdict = "satisfied" if ok else "mismatch"
            if ok:
                self.satisfied_keys.update(e.matched)

        # Require entries (presence on each side, status-blind -- c-L2).
        for r in self.requires:
            r.base_hits = [k for k in self.keys
                           if self._status(base, k) != ABSENT
                           and self._matches(r.regex, r.file_glob, k)]
            r.head_hits = [k for k in self.keys
                           if self._status(head, k) != ABSENT
                           and self._matches(r.regex, r.file_glob, k)]

        # Transition buckets.
        self.unexpected = [
            k for k in self.keys
            if self._status(base, k) == PASSED
            and self._status(head, k) == FAILED
            and k not in self.satisfied_keys]
        self.fixed = [k for k in self.keys
                      if self._status(base, k) == FAILED
                      and self._status(head, k) == PASSED]
        self.pass_skip = [
            k for k in self.keys
            if {self._status(base, k), self._status(head, k)} == {PASSED, SKIPPED}]
        self.appeared = [k for k in self.keys
                         if self._status(base, k) == ABSENT
                         and self._status(head, k) != ABSENT]
        self.disappeared = [k for k in self.keys
                            if self._status(base, k) != ABSENT
                            and self._status(head, k) == ABSENT]

        # Identity checks.
        bid, hid = image_id(self.base_meta), image_id(self.head_meta)
        self.id_mismatch = bool(bid and hid and bid != hid)
        bq, hq = qb_digest(self.base_meta), qb_digest(self.head_meta)
        self.qb_mismatch = bool(bq and hq and bq != hq)

        self.missing_required = [r for r in self.requires
                                 if not r.base_hits or not r.head_hits]
        self.mismatched_expects = [e for e in self.expects
                                   if e.verdict in ("mismatch", "stale")]

    @property
    def exit_code(self):
        if (self.unexpected or self.missing_required
                or self.id_mismatch or self.qb_mismatch):
            return 1
        if self.mismatched_expects:
            return 2
        return 0

    # ------------------------------------------------------------------ #
    # Report
    # ------------------------------------------------------------------ #
    def _counts(self, side):
        out = dict.fromkeys(STATUSES[:3], 0)
        for _, _, status in side.values():
            out[status] = out.get(status, 0) + 1
        return out

    def report(self):
        base_meta, head_meta = self.base_meta, self.head_meta
        out = ["# Privileged-leg comparison", ""]

        # 1. Run identity.
        out.append("## 1. Run identity")
        for label, meta in (("base", base_meta), ("head", head_meta)):
            out.append("- %s: sha `%s`, image .Id `%s`, RepoDigest `%s`, "
                       "qb.yaml `%s`"
                       % (label, source_sha(meta) or "(unknown)",
                          image_id(meta) or "(unknown)",
                          repo_digest(meta) or "(unknown)",
                          qb_digest(meta) or "(unknown)"))
        if self.id_mismatch:
            out.append("- **image .Id MISMATCH** -> exit 1")
        if self.qb_mismatch:
            out.append("- **qb.yaml digest MISMATCH** -> exit 1")
        out.append("")

        # 2. Required presence.
        out.append("## 2. Required presence")
        if not self.requires:
            out.append("- (none)")
        for r in self.requires:
            flag = "" if (r.base_hits and r.head_hits) else "  **MISSING**"
            out.append("- `%s` (file=%s): base %d, head %d%s"
                       % (r.regex, r.file_glob or "*",
                          len(r.base_hits), len(r.head_hits), flag))
        out.append("")

        # 3. Expected transitions.
        out.append("## 3. Expected transitions")
        if not self.expects:
            out.append("- (none)")
        for e in self.expects:
            verdict = e.verdict.upper() if e.verdict != "satisfied" else "satisfied"
            out.append("- [%s] `%s->%s` `%s` (file=%s) -- %d matched%s"
                       % (verdict, e.frm, e.to, e.regex, e.file_glob or "*",
                          len(e.matched),
                          ("  # " + e.reason) if e.reason else ""))
            for k in e.matched:
                out.append("    - %s: %s->%s" % (
                    k, self._status(self.base, k), self._status(self.head, k)))
        out.append("")

        # 4. Unexpected new failures.
        out.append("## 4. Unexpected new failures (Passed->Failed, not waived)")
        for k in self.unexpected:
            out.append("- %s" % k)
        if not self.unexpected:
            out.append("- (none)")
        out.append("")

        # 5. Failed->Passed.
        out.append("## 5. Failed->Passed")
        for k in self.fixed:
            out.append("- %s" % k)
        if not self.fixed:
            out.append("- (none)")
        out.append("")

        # 6. Passed<->Skipped.
        out.append("## 6. Passed<->Skipped")
        for k in self.pass_skip:
            out.append("- %s: %s->%s" % (
                k, self._status(self.base, k), self._status(self.head, k)))
        if not self.pass_skip:
            out.append("- (none)")
        out.append("")

        # 7. Appeared / disappeared.
        out.append("## 7. Appeared / disappeared")
        for k in self.appeared:
            out.append("- appeared: %s (%s)" % (k, self._status(self.head, k)))
        for k in self.disappeared:
            out.append("- disappeared: %s (was %s)" % (
                k, self._status(self.base, k)))
        if not self.appeared and not self.disappeared:
            out.append("- (none)")
        out.append("")

        # 8. Counts.
        bc, hc = self._counts(self.base), self._counts(self.head)
        out.append("## 8. Counts")
        out.append("- base: %d cases (%s)" % (
            len(self.base), ", ".join("%s %d" % (s, bc[s]) for s in STATUSES[:3])))
        out.append("- head: %d cases (%s)" % (
            len(self.head), ", ".join("%s %d" % (s, hc[s]) for s in STATUSES[:3])))
        out.append("- exit %d" % self.exit_code)
        out.append("")
        return "\n".join(out)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def run(base_dir, head_dir, expect_path, require_path, report_path):
    expects = parse_expect(expect_path) if expect_path else []
    requires = parse_require(require_path) if require_path else []
    cmp = Comparison(collect(base_dir), collect(head_dir),
                     expects, requires,
                     read_meta(base_dir), read_meta(head_dir))
    text = cmp.report()
    if report_path is None:
        report_path = os.path.join(head_dir, "compare.md")
    try:
        with open(report_path, "w") as handle:
            handle.write(text)
    except OSError as exc:
        sys.stderr.write("warning: could not write %s: %s\n"
                         % (report_path, exc))
    return cmp, text


def _summary_line(cmp):
    code = cmp.exit_code
    if code == 1:
        reasons = []
        if cmp.unexpected:
            reasons.append("%d unexpected new failure(s)" % len(cmp.unexpected))
        if cmp.missing_required:
            reasons.append("%d required pattern(s) missing"
                           % len(cmp.missing_required))
        if cmp.id_mismatch:
            reasons.append("image .Id mismatch")
        if cmp.qb_mismatch:
            reasons.append("qb.yaml digest mismatch")
        return "FAIL (exit 1): " + "; ".join(reasons)
    if code == 2:
        n = len(cmp.mismatched_expects)
        return ("OPERATOR REVIEW REQUIRED: %d expected-transition mismatch(es)"
                % n)
    return "OK (exit 0): no unexpected new failures"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare two privileged-leg run directories.")
    parser.add_argument("base", help="base-side run directory")
    parser.add_argument("head", help="head-side run directory")
    parser.add_argument("--expect", help="pre-registered transition file")
    parser.add_argument("--require", help="presence-check pattern file")
    parser.add_argument("--report",
                        help="write the report here (default <head>/compare.md)")
    args = parser.parse_args(argv)

    cmp, text = run(args.base, args.head, args.expect, args.require,
                    args.report)
    sys.stdout.write(text)
    if not text.endswith("\n"):
        sys.stdout.write("\n")
    sys.stdout.write(_summary_line(cmp) + "\n")
    return cmp.exit_code


if __name__ == "__main__":
    sys.exit(main())
