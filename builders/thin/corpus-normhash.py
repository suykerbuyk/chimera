#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
"""Content hash of a specs-corpus tree that ignores generation time.

Usage: corpus-normhash.py CORPUS_DIR > corpus.normsha256

Compare two build trees' MBT corpora (stdlib only, any python3):

  python3 builders/thin/corpus-normhash.py OLD/Debug/specs-corpus > old.normsha256
  python3 builders/thin/corpus-normhash.py NEW/Debug/specs-corpus > new.normsha256
  cmp old.normsha256 new.normsha256 && echo identical

Every trace is ITF JSON whose #meta.description and #meta.timestamp record
when quint generated it, so raw hashes differ on every generation.  This
drops exactly those two keys, re-serialises canonically (sorted keys, compact
separators, UTF-8), and prints "<sha256>  ./<relpath>" sorted by path in
byte order -- the same layout as `find . -type f | LC_ALL=C sort | sha256sum`.
A file that is not JSON is hashed raw and marked "(raw)".  Two corpora match
when the outputs are identical.
"""

import hashlib
import json
import os
import sys


def norm_hash(path):
    with open(path, "rb") as f:
        data = f.read()
    try:
        obj = json.loads(data)
    except ValueError:
        return hashlib.sha256(data).hexdigest(), True
    meta = obj.get("#meta") if isinstance(obj, dict) else None
    if isinstance(meta, dict):
        meta.pop("description", None)
        meta.pop("timestamp", None)
    canon = json.dumps(obj, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(canon).hexdigest(), False


def main(root):
    rels = []
    for d, _, files in os.walk(root):
        for name in files:
            rels.append(os.path.relpath(os.path.join(d, name), root))
    if not rels:
        print("corpus-normhash: no files under %s" % root, file=sys.stderr)
        return 2
    for rel in sorted(rels, key=lambda r: r.encode("utf-8")):
        h, raw = norm_hash(os.path.join(root, rel))
        print("%s  ./%s%s" % (h, rel, "  (raw)" if raw else ""))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
