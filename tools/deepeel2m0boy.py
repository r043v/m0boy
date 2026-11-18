#!/usr/bin/env python3
"""deepeel cov v1 -> m0boy coverage profile (sms/gg/msx/sg1000).

Feeds a deepeel campaign's coverage into `m0recomp --target pc` for the z80
systems, whose generators read their own profile format.  Executed lines only
(flags & 1): the profile is an execution trace, speculative-only and data-only
lines never become code.  Locations are `bank << 16 | cpu-pc`, the encoding the
m0boy hosts write (gg_host/sg_host, `# bank-shift 14`).

The top-hit pcs also go out as `# entry` seeds: dynamic jump targets (RET,
JP (HL) landing sites) the region scan cannot see statically.

--feedback PROFILE...: merge the host harness's own profiles (the pcs it
interpreted in session) and turn every one of them into an `# entry`.  Two
reasons make this iteration mandatory on the banked systems: the host's
locations carry the true window per pc (a GG shows its ROM through 4000-7FFF
AND 8000-BFFF; the deepeel cov records by bank only), and a dynamic jump that
leaves compiled code never reaches the runtime's target recorder.  Without the
feedback the dispatch misses those regions and the interpreter eats the loops.

--rom ROM: drop the executed heads inside a padding ramp (a 1-byte $FF in a run
of at least 16, or $00 in a run of at least 33, same rule as the kernel's
pad_unmarked since 0.396.0).  Older coverages marked a fork's slide through the
filler (rst $38 returning into the next $FF) as executed code: m0boy compiled
thousands of rst $38 for nothing (Ristar GG: 28 % of its instructions).
"""
import argparse

RAMP = {0xFF: 16, 0x00: 33}


def ramp_bytes(rom):
    """ROM indices inside a ramp (rom index = bank * 0x4000 + offset & 0x3FFF)."""
    out, i, n = set(), 0, len(rom)
    while i < n:
        v, j = rom[i], i
        while j < n and rom[j] == v:
            j += 1
        if v in RAMP and j - i >= RAMP[v]:
            out.update(range(i, j))
        i = j
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("system", choices=["sms", "gg", "msx", "sg1000"])
    ap.add_argument("cov", help="deepeel coverage v1 file (explore --cov)")
    ap.add_argument("--out", required=True, help="the m0boy profile to write")
    ap.add_argument("--bank-shift", type=int, default=14, help="16 for sms/gg, 13 for msx 8 KiB units")
    ap.add_argument("--entries", type=int, default=5000, help="top-hit pcs offered as region entries")
    ap.add_argument("--min-hits", type=int, default=8, help="minimum executions for an entry seed")
    ap.add_argument("--frames", type=int, default=600, help="nominal session length (rate ranking)")
    ap.add_argument("--feedback", action="append", default=[],
                    help="host profile(s) from the built project's harness; every pc becomes an entry")
    ap.add_argument("--rom", help="the ROM image: drop executed heads inside $FF/$00 padding ramps")
    args = ap.parse_args()
    ramps = set()
    if args.rom:
        with open(args.rom, "rb") as f:
            rom = f.read()
        if len(rom) % 0x4000 == 512:                # copier header
            rom = rom[512:]
        ramps = ramp_bytes(rom)
    dropped = 0

    if args.system == "msx":
        args.bank_shift = 13
    header = f"# m0boy.{args.system}.coverage.v2"

    out = [header, f"# bank-shift {args.bank_shift}", f"# frames {args.frames}"]
    entries, n = [], 0
    with open(args.cov) as f:
        for line in f:
            p = line.split()
            if not p or p[0].startswith("#"):
                continue
            if len(p) < 3:
                continue
            bank, off, flags = int(p[0], 16), int(p[1], 16), int(p[2], 16)
            hits = int(p[3]) if len(p) > 3 else 1
            if not flags & 1:                      # executed instruction heads only
                continue
            if ramps and (bank << 14 | (off & 0x3FFF)) in ramps:
                dropped += 1
                continue
            loc = (bank << 16) | off
            out.append(f"{loc:06x} {hits}")
            if hits >= args.min_hits:
                entries.append((hits, loc))
            n += 1
    entries.sort(reverse=True)
    seeds = [loc for _, loc in entries[:args.entries]]

    fed = 0
    for path in args.feedback:                     # the harness's interpreted pcs
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p or p[0].startswith("#"):
                    continue
                if len(p) >= 2:
                    seeds.append(int(p[0], 16))
                    fed += 1
    seen = set()
    uniq = [loc for loc in seeds if not (loc in seen or seen.add(loc))]
    out = out[:3] + [f"# entry {loc:06x}" for loc in uniq] + out[3:]
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"{args.out}: {n} executed pcs, {len(uniq)} entry seeds "
          f"({min(len(entries), args.entries)} from hits, {fed} from feedback)"
          + (f", {dropped} padding-ramp heads dropped" if args.rom else ""))


if __name__ == "__main__":
    main()
