# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
"""Derive the podman-* seccomp overlay from Podman's default profile.

Usage: python3 seccomp-libaio.py BASE.json > overlay.json

Removes io_pgetevents and io_pgetevents_time64 from every names list and
changes nothing else.  The default profile lists both in an explicit errno
group that returns EPERM.  libaio >= 0.3.113 implements io_getevents() with
io_pgetevents, and that EPERM leaves it unable to reap a completion.  Once
the names are unlisted they fall to the profile's defaultAction (ENOSYS).
libaio treats ENOSYS as "this syscall is absent" and reaps through the
allowed io_getevents syscall (measured: direct io_pgetevents returns 38, and
CMake's libaio probe exits 0).  Nothing is added to any allow list.  Allowing
the two names is not required and is not what this generator does.

Exits non-zero, writing nothing to stdout, if the base profile does not list
both names, if either is still listed afterwards, or if any io_uring_* name is
allowed.
"""

import copy
import json
import sys

UNLIST = ("io_pgetevents", "io_pgetevents_time64")


def die(msg):
    sys.stderr.write("seccomp-libaio: %s\n" % msg)
    sys.exit(1)


def listed(profile, name):
    return [i for i, rule in enumerate(profile.get("syscalls", []))
            if name in rule.get("names", [])]


def main():
    if len(sys.argv) != 2:
        die("usage: seccomp-libaio.py BASE.json")
    try:
        with open(sys.argv[1]) as f:
            base = json.load(f)
    except (OSError, ValueError) as e:
        die("cannot read base profile %s: %s" % (sys.argv[1], e))

    for name in UNLIST:
        if not listed(base, name):
            die("base profile does not list %s; refusing to guess" % name)

    out = copy.deepcopy(base)
    for rule in out["syscalls"]:
        rule["names"] = [n for n in rule.get("names", []) if n not in UNLIST]

    for name in UNLIST:
        if listed(out, name):
            die("%s is still listed after the edit" % name)

    for rule in out["syscalls"]:
        if rule.get("action") == "SCMP_ACT_ALLOW":
            uring = [n for n in rule["names"] if n.startswith("io_uring")]
            if uring:
                die("overlay would allow %s" % ", ".join(uring))

    json.dump(out, sys.stdout, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
