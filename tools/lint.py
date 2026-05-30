#!/usr/bin/env python3
"""
clang-tidy harness for uFW — single source of truth for CLI, CMake targets, and CI.

Why a driver and not bare clang-tidy / run-clang-tidy:
  * Headers carrying templates (e.g. entity_ref<T>) are *uninstantiated* when a
    .cpp is analysed, so their non-dependent findings never surface. IDEs catch
    them because they analyse the header as its own translation unit. This driver
    reproduces that: every first-party header is linted as a standalone TU, using
    the compile flags borrowed from a representative TU in the same component.
  * run-clang-tidy only walks the compile DB, so it shares the blind spot above.

Output:
  * human-readable diagnostics + a per-check summary (always);
  * --json <path>      machine-readable summary parsed from the diagnostics;
  * --export-fixes <d> raw clang-tidy YAML per file (offsets + replacement edits).

No third-party deps — standard library only.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_EXTS = {".cpp", ".cc", ".cxx"}
HDR_EXTS = {".hpp", ".hh", ".hxx", ".h"}

# `path:line:col: level: message [check,check]`
DIAG_RE = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<level>warning|error): (?P<msg>.*?)"
    r"(?: \[(?P<checks>[a-z0-9.\-]+(?:,[a-z0-9.\-]+)*)\])?$"
)


def load_db(build_dir: Path) -> list[dict]:
    db_path = build_dir / "compile_commands.json"
    if not db_path.is_file():
        sys.exit(f"error: {db_path} not found — configure with "
                 f"CMAKE_EXPORT_COMPILE_COMMANDS=ON")
    return json.loads(db_path.read_text())


def entry_args(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def entry_file(entry: dict) -> Path:
    return (Path(entry["directory"]) / entry["file"]).resolve()


def first_party_dirs(include_aux: bool) -> list[Path]:
    dirs = [ROOT / "ufw"]
    if include_aux:
        dirs += [ROOT / "tests", ROOT / "benchmarks"]
    return [d for d in dirs if d.is_dir()]


def under_any(path: Path, roots: list[Path]) -> bool:
    return any(root in path.parents for root in roots)


def common_components(a: Path, b: Path) -> int:
    n = 0
    for x, y in zip(a.parts, b.parts):
        if x != y:
            break
        n += 1
    return n


def representative_flags(header: Path, db: list[dict]) -> list[str] | None:
    """Borrow compile flags from the DB entry sharing the longest path prefix."""
    best, best_score = None, -1
    for entry in db:
        score = common_components(header, entry_file(entry))
        if score > best_score:
            best, best_score = entry, score
    if best is None:
        return None

    inp = entry_file(best)
    args = entry_args(best)
    flags: list[str] = []
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if i == 0:                       # the compiler
            continue
        if a == "-c":
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-MD", "-MMD", "-MP", "-MG", "-MM"):
            continue
        resolved = (Path(best["directory"]) / a).resolve()
        if resolved == inp:              # the input source file
            continue
        if a == "-Werror" or a.startswith("-Werror="):
            # Header parsed in isolation may trip a compiler warning that never
            # fires in context; don't let -Werror abort clang-tidy. The tidy
            # checks' own WarningsAsErrors (.clang-tidy) still gates findings.
            continue
        flags.append(a)
    return ["-xc++-header", *flags]


def run_clang_tidy(cmd: list[str]) -> tuple[int, str]:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def export_path(export_dir: Path, target: Path) -> Path:
    mangled = str(target.relative_to(ROOT)).replace(os.sep, "_")
    return export_dir / f"{mangled}.yaml"


def build_jobs(args, db: list[dict]) -> list[tuple[str, Path, list[str]]]:
    """Return (kind, target, clang_tidy_argv) for every unit to lint."""
    fp_dirs = first_party_dirs(args.all)
    clang_tidy = [args.clang_tidy]
    if args.fix:
        clang_tidy += ["--fix", "--fix-notes"]

    jobs: list[tuple[str, Path, list[str]]] = []

    # TU pass — flags come from the compile DB via -p.
    seen: set[Path] = set()
    for entry in db:
        f = entry_file(entry)
        if f.suffix not in SRC_EXTS or not under_any(f, fp_dirs) or f in seen:
            continue
        seen.add(f)
        cmd = [*clang_tidy, "-p", str(args.build_dir)]
        if args.export_fixes:
            cmd += ["--export-fixes", str(export_path(args.export_fixes, f))]
        cmd.append(str(f))
        jobs.append(("tu", f, cmd))

    # Header pass — each header as a standalone TU (catches template headers).
    if args.headers:
        for d in fp_dirs:
            for h in sorted(d.rglob("*")):
                if h.suffix not in HDR_EXTS:
                    continue
                flags = representative_flags(h, db)
                if flags is None:
                    continue
                cmd = [*clang_tidy]
                if args.export_fixes:
                    cmd += ["--export-fixes",
                            str(export_path(args.export_fixes, h))]
                cmd += [str(h), "--", *flags]
                jobs.append(("header", h, cmd))
    return jobs


def parse_diags(text: str, kind: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        m = DIAG_RE.match(line)
        if not m:
            continue
        f = m["file"]
        try:
            f = str(Path(f).resolve().relative_to(ROOT))
        except ValueError:
            pass
        if "/ufw/" not in f and not f.startswith("ufw/"):
            continue  # ignore third-party headers pulled in transitively
        # clang-tidy appends a synthetic '-warnings-as-errors' to the check
        # list under WarningsAsErrors:'*'; it is not a real check.
        checks = [c for c in (m["checks"] or "").split(",")
                  if c and c != "-warnings-as-errors"]
        out.append({
            "file": f, "line": int(m["line"]), "col": int(m["col"]),
            "level": m["level"], "message": m["msg"],
            "checks": checks,
            "source": kind,
        })
    return out


def main() -> int:
    p = argparse.ArgumentParser(description="clang-tidy harness for uFW")
    p.add_argument("--build-dir", type=Path, default=ROOT / "build" / "Debug",
                   help="dir containing compile_commands.json (default build/Debug)")
    p.add_argument("--clang-tidy",
                   default=os.environ.get("CLANG_TIDY", "clang-tidy"),
                   help="clang-tidy binary ($CLANG_TIDY, else PATH)")
    p.add_argument("--fix", action="store_true", help="apply fixes in place")
    p.add_argument("--no-headers", dest="headers", action="store_false",
                   help="skip the standalone-header pass")
    p.add_argument("--all", action="store_true",
                   help="also lint tests/ and benchmarks/")
    p.add_argument("--json", type=Path, help="write machine-readable summary")
    p.add_argument("--export-fixes", type=Path,
                   help="dir for raw clang-tidy YAML (offsets + replacements)")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = p.parse_args()

    args.build_dir = args.build_dir.resolve()
    if args.export_fixes:
        args.export_fixes = args.export_fixes.resolve()
        args.export_fixes.mkdir(parents=True, exist_ok=True)

    db = load_db(args.build_dir)
    jobs = build_jobs(args, db)
    if not jobs:
        print("no first-party translation units found", file=sys.stderr)
        return 2

    # --fix mutates files; run serially so concurrent edits can't collide.
    workers = 1 if args.fix else min(args.jobs, len(jobs))
    diags: list[dict] = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = pool.map(lambda j: (j[0], j[1], *run_clang_tidy(j[2])), jobs)
        for kind, target, rc, text in results:
            diags.extend(parse_diags(text, kind))

    # Dedup: a header finding can arrive via both its standalone pass and a TU
    # that instantiates it. Keep one, preferring the standalone-header source.
    unique: dict[tuple, dict] = {}
    for d in diags:
        key = (d["file"], d["line"], d["col"], tuple(d["checks"]))
        if key not in unique or d["source"] == "header":
            unique[key] = d
    diags = sorted(unique.values(), key=lambda d: (d["file"], d["line"], d["col"]))

    by_check: dict[str, int] = {}
    for d in diags:
        for c in (d["checks"] or ["<none>"]):
            by_check[c] = by_check.get(c, 0) + 1

    for d in diags:
        checks = ",".join(d["checks"])
        print(f"{d['file']}:{d['line']}:{d['col']}: {d['level']}: "
              f"{d['message']} [{checks}]")
    print(f"\n{'=' * 60}")
    for check, n in sorted(by_check.items(), key=lambda kv: -kv[1]):
        print(f"  {n:3d}  {check}")
    print(f"  {'-' * 40}\n  {len(diags):3d}  total findings "
          f"across {len(jobs)} units")

    if args.json:
        ver = subprocess.run([args.clang_tidy, "--version"],
                             capture_output=True, text=True).stdout.strip()
        args.json.write_text(json.dumps({
            "clang_tidy_version": ver,
            "build_dir": str(args.build_dir),
            "total": len(diags),
            "summary": dict(sorted(by_check.items(), key=lambda kv: -kv[1])),
            "diagnostics": diags,
        }, indent=2))
        print(f"\nwrote {args.json}")

    return 1 if diags else 0


if __name__ == "__main__":
    sys.exit(main())
