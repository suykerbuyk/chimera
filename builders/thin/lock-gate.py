# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
"""Fail the thin image build if any installed package is not in the snapshot.

builders/thin/Dockerfile points apt at one snapshot.ubuntu.com timestamp and
nothing else, so every installed (Package, Architecture, Version) must appear
in that snapshot's Packages indexes.  A package that does not -- one left over
from the base image that the snapshot never carried, or one fetched from a
live archive -- means the image no longer matches its pins.

Inputs are plain files so the gate can run against fixtures:

  --status   dpkg-query -W -f '${db:Status-Status}\\t${Package}\\t${Architecture}\\t${Version}\\n'
             Only rows whose first field is exactly "installed" count.
  --lists    Packages stanzas as text (apt-helper cat-file output), repeatable.
  --pin      NAME=VALUE, repeatable; an empty VALUE is an input error.
  --manifest DIR: on success, write DIR/packages.txt and DIR/pins.

Exit status: 0 every installed package is in the snapshot; 2 usage or input
error (no lists, no stanzas, no installed rows, an empty pin); 3 at least one
installed package is missing.  On 3 the gate prints only the missing
Package:Arch=Version triples, one per line, never its inputs, so a caller can
check exactly which rows failed.

Stdlib only: it runs on the image's python3 before anything else is
installed with pip.
"""

import argparse
import os
import sys

EXIT_OK = 0
EXIT_INPUT = 2
EXIT_MISSING = 3


class InputError(Exception):
    pass


def read_status(path):
    rows = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) != 4 or fields[0] != "installed":
                continue
            rows.add((fields[1], fields[2], fields[3]))
    return rows


def read_lists(paths):
    """Return the set of (Package, Architecture, Version) in the stanzas."""
    entries = set()
    for path in paths:
        stanza = {}
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in list(f) + ["\n"]:
                line = line.rstrip("\n")
                if not line:
                    if {"Package", "Architecture", "Version"} <= stanza.keys():
                        entries.add((stanza["Package"], stanza["Architecture"],
                                     stanza["Version"]))
                    stanza = {}
                    continue
                if line[0] in " \t":
                    continue
                key, sep, value = line.partition(":")
                if sep and key in ("Package", "Architecture", "Version"):
                    stanza[key] = value.strip()
    return entries


def parse_pins(pins):
    out = []
    for pin in pins:
        name, sep, value = pin.partition("=")
        if not sep or not name or not value:
            raise InputError("empty or malformed pin: %r" % pin)
        out.append((name, value))
    return out


def triple(t):
    return "%s:%s=%s" % t


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--status", required=True)
    ap.add_argument("--lists", action="append", default=[])
    ap.add_argument("--pin", action="append", default=[])
    ap.add_argument("--manifest")
    try:
        args = ap.parse_args(argv)
    except SystemExit:
        return EXIT_INPUT

    try:
        pins = parse_pins(args.pin)
        if not args.lists:
            raise InputError("no --lists files given")
        entries = read_lists(args.lists)
        if not entries:
            raise InputError("no Packages stanzas in %d list file(s)"
                             % len(args.lists))
        installed = read_status(args.status)
        if not installed:
            raise InputError("no installed rows in %s" % args.status)
    except (InputError, OSError) as e:
        print("lock-gate: %s" % e, file=sys.stderr)
        return EXIT_INPUT

    missing = sorted(installed - entries)
    if missing:
        for t in missing:
            print(triple(t))
        print("lock-gate: %d installed package(s) not in the snapshot"
              % len(missing), file=sys.stderr)
        return EXIT_MISSING

    snapshot = dict(pins).get("UBUNTU_SNAPSHOT", "?")
    print("lock: %d installed packages, all in %d snapshot index entries "
          "(%d files), snapshot %s"
          % (len(installed), len(entries), len(args.lists), snapshot))

    if args.manifest:
        os.makedirs(args.manifest, exist_ok=True)
        with open(os.path.join(args.manifest, "packages.txt"), "w") as f:
            for t in sorted(installed):
                f.write(triple(t) + "\n")
        with open(os.path.join(args.manifest, "pins"), "w") as f:
            for name, value in pins:
                f.write("%s=%s\n" % (name, value))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
