#!/usr/bin/env python3
"""Physical-design check: component levelization over src/.

Lakos's rule for large C++ systems is that the physical dependency graph
must be a DAG, and that every component sits at a level: level 1 depends
on nothing else in the project, level N depends only on levels below N.
An acyclic graph can be understood, tested, and linked bottom-up; a cyclic
one cannot be tested in isolation at all, because a cycle has no first
component to test.

This checks three things and fails the build on any of them:

  1. No cycles among components (a component is a .h/.cpp pair).
  2. No cycles among packages (the src/ subdirectories).
  3. Package dependencies stay inside the set declared in ALLOWED below,
     which is the layering src/CMakeLists.txt already encodes in what each
     library links. A dependency that compiles because everything ends up
     in one binary still violates the design; this is what catches it.

Reports the level structure so the physical shape is visible rather than
implied.

Usage: scripts/levelize.py [src_dir]
"""

import os
import re
import sys
from collections import defaultdict

# Package -> packages it may depend on. Mirrors src/CMakeLists.txt.
ALLOWED = {
    "core": set(),
    "model": {"core"},
    "feed": {"core", "model"},
    "feed_live": {"core", "model", "feed", "net"},
    "normalize": {"core", "model"},
    "analytics": {"core", "model"},
    "api": {"core", "model"},
    "exec": {"core", "model"},
    "alloc": {"core"},
    "net": {"core"},
    "bench": {"core", "model", "feed", "normalize", "analytics", "api", "exec"},
    # The composition root: it is allowed to see everything, which is what
    # makes it the only package that may.
    "cli": {"core", "model", "feed", "feed_live", "normalize", "analytics",
            "api", "exec", "bench", "alloc", "net"},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def component_of(path):
    """A component is a package/stem pair: foo/bar.h and foo/bar.cpp are one."""
    pkg, base = path.split("/", 1) if "/" in path else ("", path)
    stem = base.rsplit(".", 1)[0]
    return pkg, stem


def collect(src_dir):
    """component -> set(components it includes), plus the file list."""
    deps = defaultdict(set)
    known = set()
    for root, _dirs, files in os.walk(src_dir):
        for f in files:
            if not f.endswith((".h", ".cpp")):
                continue
            rel = os.path.relpath(os.path.join(root, f), src_dir)
            if "/" not in rel:
                continue  # main.cpp: the top, owned by no package
            known.add(component_of(rel))
    for root, _dirs, files in os.walk(src_dir):
        for f in files:
            if not f.endswith((".h", ".cpp")):
                continue
            full = os.path.join(root, f)
            rel = os.path.relpath(full, src_dir)
            if "/" not in rel:
                continue
            me = component_of(rel)
            with open(full, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for inc in INCLUDE_RE.findall(text):
                target = component_of(inc)
                if target in known and target != me:
                    deps[me].add(target)
    return deps, known


def find_cycle(nodes, edges):
    """Any one cycle as a node list, or None. Iterative DFS with colours."""
    WHITE, GREY, BLACK = 0, 1, 2
    colour = {n: WHITE for n in nodes}
    parent = {}
    for start in nodes:
        if colour[start] != WHITE:
            continue
        stack = [(start, iter(sorted(edges.get(start, ()))))]
        colour[start] = GREY
        while stack:
            node, it = stack[-1]
            advanced = False
            for nxt in it:
                if colour.get(nxt, BLACK) == GREY:
                    cycle = [nxt]
                    cur = node
                    while cur != nxt and cur in parent:
                        cycle.append(cur)
                        cur = parent[cur]
                    cycle.append(nxt)
                    return list(reversed(cycle))
                if colour.get(nxt, BLACK) == WHITE:
                    parent[nxt] = node
                    colour[nxt] = GREY
                    stack.append((nxt, iter(sorted(edges.get(nxt, ())))))
                    advanced = True
                    break
            if not advanced:
                colour[node] = BLACK
                stack.pop()
    return None


def levels(nodes, edges):
    """Longest-path level per node. Only valid on a DAG."""
    memo = {}

    def level_of(n):
        if n in memo:
            return memo[n]
        memo[n] = 1  # guard; the graph is known acyclic by here
        deps = edges.get(n, ())
        memo[n] = 1 + max((level_of(d) for d in deps), default=0)
        return memo[n]

    return {n: level_of(n) for n in nodes}


def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else "src"
    if not os.path.isdir(src_dir):
        print(f"levelize: no such directory: {src_dir}", file=sys.stderr)
        return 2

    deps, components = collect(src_dir)
    failures = []

    cycle = find_cycle(sorted(components), deps)
    if cycle:
        path = " -> ".join(f"{p}/{s}" for p, s in cycle)
        failures.append(f"component cycle: {path}")

    pkg_deps = defaultdict(set)
    for (pkg, _stem), targets in deps.items():
        for tpkg, _tstem in targets:
            if tpkg != pkg:
                pkg_deps[pkg].add(tpkg)
    packages = sorted({p for p, _ in components})

    pkg_cycle = find_cycle(packages, pkg_deps)
    if pkg_cycle:
        failures.append("package cycle: " + " -> ".join(pkg_cycle))

    for pkg in packages:
        allowed = ALLOWED.get(pkg)
        if allowed is None:
            failures.append(f"package '{pkg}' is not declared in ALLOWED")
            continue
        for dep in sorted(pkg_deps.get(pkg, ())):
            if dep not in allowed:
                failures.append(
                    f"undeclared dependency: {pkg} -> {dep} "
                    f"(allowed: {', '.join(sorted(allowed)) or 'none'})")

    print(f"levelize: {len(components)} components in {len(packages)} packages")
    if not cycle and not pkg_cycle:
        by_level = defaultdict(list)
        for pkg, lvl in levels(packages, pkg_deps).items():
            by_level[lvl].append(pkg)
        for lvl in sorted(by_level):
            print(f"  package level {lvl}: {' '.join(sorted(by_level[lvl]))}")
        comp_levels = levels(sorted(components), deps)
        print(f"  deepest component level: {max(comp_levels.values(), default=0)}")

    if failures:
        print()
        for f in failures:
            print(f"LEVELIZE FAIL  {f}")
        return 1
    print("levelize: acyclic, and every package dependency is declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
