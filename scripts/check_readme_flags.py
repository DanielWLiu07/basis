#!/usr/bin/env python3
"""Fail if the docs document a flag or subcommand that does not exist.

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
    # The README plus the docs it hands the reader off to.
    #
    # It was the README alone, which was right while the README carried
    # every command. Splitting the long sections into docs/ moved flags
    # like --kalshi-pem and --seconds out of the checked file, so a
    # renamed flag would have gone stale in a document nothing verified -
    # exactly the rot this script exists to catch, reintroduced by tidying.
    docs = ["README.md",
            "docs/live_capture.md",
            "docs/venues.md",
            "docs/reliability.md",
            "docs/design.md"]
    readme = "\n".join(
        (root / d).read_text(encoding="utf-8") for d in docs if (root / d).exists())

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
        print(f"  docs document {f}, which usage does not list")
        failures += 1

    # Subcommands are looked for in CODE only - fenced blocks and inline
    # spans - not in prose. "basis" is also the financial term this repo is
    # named after, so design.md's "basis stats per event" and "basis per
    # event" both parsed as subcommands the moment the scan widened beyond
    # the README. Reading code spans is the fix, and it is the more correct
    # rule anyway: a command is documented by being shown, not mentioned.
    # An invocation has `basis` as its FIRST token, optionally behind a
    # path. A fenced data-flow diagram in design.md has the line
    #
    #     analytics::DivergenceTracker          basis stats per event
    #
    # which is inside code and is not a command - "basis" there is the
    # financial term. Anchoring to the start of the line separates the two
    # without a list of words to exclude.
    subs = set()
    for block in re.findall(r"```.*?```", readme, re.S):
        subs |= set(re.findall(r"(?m)^\s*(?:\$\s*)?(?:[\w./-]*/)?basis "
                               r"([a-z][a-z-]+)", block))
    for span in re.findall(r"`([^`\n]+)`", readme):
        subs |= set(re.findall(r"^(?:[\w./-]*/)?basis ([a-z][a-z-]+)", span))
    subs -= NOT_SUBCOMMANDS
    known = set(re.findall(r"^  basis ([a-z][a-z-]+)", usage, re.M))
    for s in sorted(subs - known):
        print(f"  docs document subcommand '{s}', which usage does not list")
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
