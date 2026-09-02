#!/usr/bin/env python3
"""Fail if a doc under docs/bench/ names an input that is not committed.

The rule this enforces is the repo's own, from the README: a figure is
either measured on a committed capture or it is labelled as not yet
measured. There was one exception and nobody noticed, because the
exception is invisible from inside a working tree - `/captures/` is
gitignored, the file sat there, every command ran fine, and the row it
produced could not be reproduced by anyone who cloned the repo.

That is the failure mode worth guarding: a benchmark that works perfectly
for its author and for nobody else. It cannot be caught by running the
benchmark, only by asking whether its input ships.

    scripts/check_bench_inputs.py
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Paths that are meant to be produced, not shipped: scratch files a command
# in the doc generates first. Referencing one is correct.
GENERATED = re.compile(r"^(/tmp/|out\.|.*\bmktemp\b)")


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True,
                         text=True, check=True).stdout
    return set(out.split())


def main():
    tracked = tracked_files()
    failures = 0
    checked = 0

    for doc in sorted((ROOT / "docs" / "bench").glob("*.md")):
        text = doc.read_text(encoding="utf-8")
        # Angle brackets mark a usage metavariable - `basis xvenue-lead
        # <capture.feedlog>` names an argument, not a file. Strip those
        # first so the check does not demand that a placeholder ship.
        prose = re.sub(r"<[^<>\n]*\.feedlog(?:\.gz)?>", "", text)
        # Any path ending in .feedlog or .feedlog.gz that the prose names.
        for ref in sorted(set(re.findall(r"[\w./-]+\.feedlog(?:\.gz)?", prose))):
            if GENERATED.match(ref):
                continue
            checked += 1
            # A doc may name either the .gz that ships or the file a reader
            # gunzips it into; the .gz is what has to be tracked.
            gz = ref if ref.endswith(".gz") else ref + ".gz"
            candidates = {ref, gz,
                          f"docs/bench/{pathlib.PurePath(gz).name}"}
            if candidates & tracked:
                continue
            print(f"  {doc.relative_to(ROOT)} references {ref}, "
                  f"which is not committed")
            failures += 1

    print(f"\n{checked} capture references across docs/bench/")
    if failures:
        print(f"FAIL: {failures} reference(s) to inputs that do not ship")
        return 1
    print("ok: every referenced capture is committed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
