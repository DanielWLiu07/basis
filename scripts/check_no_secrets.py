#!/usr/bin/env python3
"""Refuse to let private key material enter the repository.

.gitignore protects a path; this protects the repository. A key pasted into
a scratch file at the root, or a capture that happens to embed one, is
invisible to `secrets/` in .gitignore but caught here.

  check_no_secrets.py            scan every tracked file (CI)
  check_no_secrets.py --staged   scan what is about to be committed (hook)

Exit 1 names the offending file and line. There is deliberately no
override flag: a commit that trips this should be fixed, not forced.
"""

import re
import subprocess
import sys
from pathlib import Path

# Assembled from parts so this file never contains the literal it hunts for
# and cannot flag itself.
_BEGIN = "-" * 5 + "BEGIN"
PATTERNS = [
    (re.compile(_BEGIN + r" (?:[A-Z0-9]+ )*PRIVATE KEY" + "-" * 5), "private key block"),
    (re.compile(r"\bKALSHI-ACCESS-SIGNATURE\s*[:=]\s*\S{20,}"), "signed Kalshi header"),
]

SKIP_SUFFIXES = {".gz", ".png", ".jpg", ".jpeg", ".gif", ".pdf", ".zip", ".feedlog"}
SELF = "check_no_secrets.py"


def local_literals():
    """Exact values from gitignored credential files, so that pasting one of
    them into a tracked file is caught even when its shape (a bare UUID) is
    too generic to blocklist by pattern."""
    out = []
    for name in ("kalshi_key_id.txt",):
        try:
            value = Path("secrets", name).read_text().strip()
        except OSError:
            continue
        if len(value) >= 16:
            out.append((re.compile(re.escape(value)), "credential from secrets/" + name))
    return out


def offending_lines(text):
    checks = PATTERNS + local_literals()
    for lineno, line in enumerate(text.splitlines(), 1):
        for pattern, label in checks:
            if pattern.search(line):
                yield lineno, label


def content(path, staged):
    if staged:
        out = subprocess.run(["git", "show", ":" + path], capture_output=True)
        if out.returncode != 0:
            return None
        raw = out.stdout
    else:
        try:
            raw = Path(path).read_bytes()
        except OSError:
            return None
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return None


def main():
    staged = "--staged" in sys.argv
    cmd = (["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"]
           if staged else ["git", "ls-files"])
    files = subprocess.run(cmd, capture_output=True, text=True,
                           check=True).stdout.split()

    findings = []
    for path in files:
        if path.endswith(SELF) or Path(path).suffix in SKIP_SUFFIXES:
            continue
        text = content(path, staged)
        if text is None:
            continue
        for lineno, label in offending_lines(text):
            findings.append((path, lineno, label))

    if findings:
        where = "staged for commit" if staged else "tracked in the repository"
        print("SECRET MATERIAL " + where.upper(), file=sys.stderr)
        for path, lineno, label in findings:
            print("  {}:{}: {}".format(path, lineno, label), file=sys.stderr)
        print("", file=sys.stderr)
        print("Nothing was committed. Move the key under secrets/ (gitignored)",
              file=sys.stderr)
        print("and, if it ever reached a remote, rotate it at the provider.",
              file=sys.stderr)
        return 1

    print("no secret material in {} file(s)".format(len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
