#!/usr/bin/env python3
"""Fail if the README documents a flag or subcommand that does not exist.

Prose is the only part of a project with no failure mode. Nothing in the
build reads it, so it does not break when the code moves - it just becomes
wrong and stays that way until someone reads it beside the code. This week
that was a contract registry whose every contract had resolved, and a
README paragraph still describing it in the present tense.

This checks the slice a machine can: every --flag and every `basis <sub>`
the README mentions has to appear in the usage text. It says nothing about
whether the surrounding sentence is true, which is the larger half.

    scripts/check_readme_flags.py [path/to/basis]

Run it against a BASIS_ENABLE_NET build. record and live only appear in
the usage text there, so a non-net binary reports them missing.
"""

import pathlib
import re
import subprocess
import sys

# cmake and ctest flags appear in the build instructions and are not ours.
FOREIGN = {"--build", "--test-dir", "--output-on-failure", "--target"}

# "per-event basis statistics" and "basis prints as the books move" are
# prose about the price difference, not invocations of the binary. The
# regex cannot tell; this list can.
NOT_SUBCOMMANDS = {"prints", "statistics"}


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build-net/src/basis"
    root = pathlib.Path(__file__).resolve().parent.parent
    readme = (root / "README.md").read_text(encoding="utf-8")

    try:
        # No arguments prints usage and exits nonzero, which is correct
        # behaviour and not an error here.
        usage = subprocess.run([binary], capture_output=True, text=True,
                               timeout=60).stdout
    except (OSError, subprocess.SubprocessError) as e:
        print(f"cannot run {binary}: {e}")
        return 1
    if not usage.strip():
        print(f"{binary} printed no usage text")
        return 1

    failures = 0

    in_readme = set(re.findall(r"(--[a-z][a-z0-9-]+)", readme)) - FOREIGN
    in_usage = set(re.findall(r"(--[a-z][a-z0-9-]+)", usage))
    for f in sorted(in_readme - in_usage):
        print(f"  README documents {f}, which usage does not list")
        failures += 1

    subs = set(re.findall(r"basis ([a-z][a-z-]+)", readme)) - NOT_SUBCOMMANDS
    known = set(re.findall(r"^  basis ([a-z][a-z-]+)", usage, re.M))
    for s in sorted(subs - known):
        print(f"  README documents subcommand '{s}', which usage does not list")
        failures += 1

    print(f"\n{len(in_readme)} flags and {len(subs)} subcommands referenced")
    undocumented = sorted(in_usage - in_readme)
    if undocumented:
        print(f"in usage but not the README (fine, for awareness): "
              f"{' '.join(undocumented)}")
    if failures:
        print(f"\nFAIL: {failures} documented item(s) do not exist")
        return 1
    print("ok: everything the README documents exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
