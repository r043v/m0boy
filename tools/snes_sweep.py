#!/usr/bin/env python3
"""Whole-collection sweep of the SNES converter.

Every .sfc/.smc under a directory goes through the converter as a precise
project — the interpreter plays the frames first (the coverage profile),
then the recompiler walks it again with the profile, and the built host must
print the same run_hash and cycles from the recompiled code as from its own
--interp.  Results go to stdout and a report file (default logs/snes_sweep.txt).

    python3 tools/snes_sweep.py roms/snes --frames 300

No ROMs in the directory: the sweep says so and exits 0 — nothing claimed.
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

EXTENSIONS = (".sfc", ".smc", ".fig", ".swc")
INSTRUCTIONS_RE = re.compile(r"instructions:\s*(\d+) compiled \((\d+) played\)")
REGIONS_RE = re.compile(r"regions:\s*(\d+), (\d+) entries")


def run(args, timeout=900):
    return subprocess.run([str(a) for a in args], text=True, capture_output=True,
                          timeout=timeout)


def sweep_one(m0recomp, rom, out_root, frames, label):
    """One ROM: profile, generate, build, native vs interp.  Returns a row."""
    row = {"rom": label, "status": "", "frames": frames, "instructions": "",
           "regions": "", "run_hash": "", "interp_hash": "", "cycles": ""}
    work = out_root / label
    shutil.rmtree(work, ignore_errors=True)
    try:
        # the profile: the standalone harness's interpreter plays the frames
        cov = out_root / f"{label}.cov"
        harness = m0recomp.parent / "snes_host"
        prof = run([harness, rom, "--frames", frames, "--interp", "--coverage", cov])
        if prof.returncode:
            row["status"] = f"profile failed: {prof.stderr.strip().splitlines()[-1:]}"
            return row
        row["run_hash"] = re.search(r"run_hash=([0-9a-f]+)", prof.stdout).group(1)
        row["interp_hash"] = row["run_hash"]  # replaced by the project's --interp below

        gen = run([m0recomp, "--system", "snes", rom, "--target", "desktop",
                   "--output-prefix", label, "-o", work / "proj", "--meta-precise",
                   "--coverage", cov])
        if gen.returncode:
            row["status"] = f"generate failed: {gen.stderr.strip().splitlines()[-1:]}"
            return row
        m = INSTRUCTIONS_RE.search(gen.stdout)
        if m:
            row["instructions"] = f"{m.group(1)} ({m.group(2)} played)"
        m = REGIONS_RE.search(gen.stdout)
        if m:
            row["regions"] = m.group(1)

        conf = run(["cmake", "-G", "Ninja", "-S", work / "proj" / "host",
                    "-B", work / "build"])
        if conf.returncode:
            row["status"] = "configure failed"
            return row
        made = run(["cmake", "--build", work / "build"])
        if made.returncode:
            row["status"] = "build failed"
            return row
        exe = work / "build" / f"{label}_host"
        if not exe.exists():
            row["status"] = "no host built"
            return row

        native = run([exe, "--frames", frames])
        interp = run([exe, "--frames", frames, "--interp"])
        if native.returncode or interp.returncode:
            row["status"] = "run failed"
            return row
        row["run_hash"] = re.search(r"run_hash=([0-9a-f]+)", native.stdout).group(1)
        row["interp_hash"] = re.search(r"run_hash=([0-9a-f]+)", interp.stdout).group(1)
        row["cycles"] = re.search(r"cycles=(\d+)", native.stdout).group(1)
        interp_cycles = re.search(r"cycles=(\d+)", interp.stdout).group(1)
        if row["run_hash"] == row["interp_hash"] and row["cycles"] == interp_cycles:
            row["status"] = "match"
        else:
            row["status"] = "MISMATCH"
    except subprocess.TimeoutExpired:
        row["status"] = "timeout"
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("roms", nargs="?", default="roms/snes",
                    help="directory of .sfc/.smc images (default roms/snes)")
    ap.add_argument("--m0recomp", default="build/bin/m0recomp")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--out", default="output/snes_sweep")
    ap.add_argument("--report", default="logs/snes_sweep.txt")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    args = ap.parse_args()

    roms_dir = pathlib.Path(args.roms)
    roms = sorted(p for p in roms_dir.iterdir()
                  if p.is_file() and p.suffix.lower() in EXTENSIONS) \
        if roms_dir.is_dir() else []
    if not roms:
        print(f"no .sfc/.smc under {roms_dir}: nothing swept")
        return 0

    m0recomp = pathlib.Path(args.m0recomp)
    out_root = pathlib.Path(args.out)
    out_root.mkdir(parents=True, exist_ok=True)
    pathlib.Path(args.report).parent.mkdir(parents=True, exist_ok=True)

    labels = {}
    for rom in roms:
        label = re.sub(r"[^A-Za-z0-9]+", "_", rom.stem).strip("_")[:48] or "rom"
        base, n = label, 2
        while label in labels:
            label, n = f"{base}_{n}", n + 1
        labels[label] = rom

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        rows = list(pool.map(lambda kv: sweep_one(
            m0recomp, kv[1], out_root, args.frames, kv[0]), labels.items()))

    lines = [f"# SNES sweep: {len(rows)} roms, {args.frames} frames, native vs interp"]
    for r in rows:
        lines.append("{rom}: {status} instructions={instructions} regions={regions} "
                     "run_hash={run_hash} interp={interp_hash} cycles={cycles}"
                     .format(**r))
    matched = sum(1 for r in rows if r["status"] == "match")
    lines.append(f"{matched}/{len(rows)} matched")
    report = "\n".join(lines) + "\n"
    print(report)
    with open(args.report, "w") as f:
        f.write(report)
    return 0 if matched == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
