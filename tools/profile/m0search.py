#!/usr/bin/env python3
r"""m0search.py : la recherche de profil par parties, sur tous les coeurs.

Un seul point d'entrée pour ce que le harnais Game Boy faisait à la main :
des sessions de jeu qui font découvrir du code, leur couverture collectée
puis fusionnée en un profil prêt pour le générateur.  Deux moteurs selon
le coeur :

  - les coeurs à couverture (Game Boy, SG-1000, MSX, Game Gear, NES,
    Vectrex, Mega Drive, PC Engine) : une longue partie semée (--random
    GRAINE ; l'explorateur du harnais est retiré, deepeel explore) et
    la couverture se écrit toute seule ;
  - PICO-8 reçoit des scripts d'entrées générés ici, même politique que
    m0_explore : une direction tenue 8 à 63 trames, les boutons tenus ou
    tapotés, START de temps en temps, dans le dialecte d'entrées du
    coeur (son Lua recompilé n'a pas de consommateur de profil).

Une session « idle » complète le plan : --idle-frames TRAMES (6000 par
défaut, 0 la retire) lance la cartouche depuis la mise sous tension sans
toucher à rien — les écrans titre passés seuls et les modes démo qui
attendent une inaction entrent dans la couverture, ce qu'aucune main
aléatoire (la nôtre presse quelque chose ~80 % du temps) n'obtient.

Les enregistrements manuels (--input d'une partie jouée : gm0 converti par
m0profile.py, --record-input du Mega Drive ou du Vectrex, ou toute main)
se rejouent comme des sessions de plus ; leur couverture entre dans la
fusion.  La fusion est générique : elle additionne les compteurs de tous
les formats de couverture du dépôt (adresse seule du Mega Drive, « adresse
compteur » v2, « adresse compteur d » du NES, « banc adresse drapeaux
compteur » du Game Boy).

Exemples :

  m0search.py run --system megadrive --harness output/x/build/md_pc \\
      -o output/x/search --sessions 4 --frames 1500 --seed 7
  m0search.py run --system gamegear --harness build/bin/gg_host --rom jeu.gg \\
      -o output/x/search --sessions 2 --frames 2400
  m0search.py run --system vectrex --harness out/vx/host/vectrex_host \\
      --bios roms/vectrex/System\ Boot\ BIOS.vec --input partie.input
  m0search.py merge session-*.cov -o coverage.merged

Tout ce qui sort : session-NN.input/.cov, manual-NN.cov, coverage.merged,
search.json.  Les harnais des projets générés (Mega Drive, NES, Vectrex,
PC Engine, PICO-8, Game Boy) embarquent la ROM ; ne passez pas --rom pour
eux.  PICO-8 : pas de couverture côté harnais (le Lua recompilé n'a pas de
consommateur de profil), les sessions tournent et le rapport ne garde que
les empreintes ; les enregistrements manuels n'y sont pas lus.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Le masque de chaque coeur : les bits que --input lit (et --record-input
# écrit).  bits d'un nom -> 1 << position dans la table du harnais.
SYSTEMS = {
    "gb": dict(
        explore=True, coverage=True, interp=["--interp"], rom_arg=False,
        input="held-named",
        names={"a": 1, "b": 2, "select": 4, "start": 8,
               "right": 16, "left": 32, "up": 64, "down": 128},
        b1="a", b2="b",
    ),
    "sg1000": dict(
        explore=True, coverage=True, interp=[], rom_arg=True,
        input="held-named",
        names={"up": 1, "down": 2, "left": 4, "right": 8,
               "1": 16, "2": 32, "pause": 64},
        b1="1", b2="2",
    ),
    "msx": dict(
        explore=True, coverage=True, interp=[], rom_arg=True,
        input="held-named",
        names={"up": 1, "down": 2, "left": 4, "right": 8,
               "1": 16, "2": 32, "pause": 64},
        b1="1", b2="2",
    ),
    "gamegear": dict(
        explore=True, coverage=True, interp=[], rom_arg=True,
        input="held-named",
        names={"up": 1, "down": 2, "left": 4, "right": 8,
               "1": 16, "2": 32, "start": 64, "pause": 128},
        b1="1", b2="2",
    ),
    "megadrive": dict(
        explore=True, coverage=True, interp=["--interp"], rom_arg=False,
        input="mask",
        names={"up": 1, "down": 2, "left": 4, "right": 8,
               "a": 16, "b": 32, "c": 64, "start": 128},
        b1="a", b2="b",
    ),
    "nes": dict(
        explore=True, coverage=True, interp=["--interp"], rom_arg=False,
        input="momentary-named",
        names={"a": 1, "b": 2, "select": 4, "start": 8,
               "up": 16, "down": 32, "left": 64, "right": 128},
        b1="a", b2="b",
    ),
    "vectrex": dict(
        explore=True, coverage=True, interp=["--interp"], rom_arg=False,
        input="held-named", bios=True,
        names={"up": 1, "down": 2, "left": 4, "right": 8,
               "a": 16, "b": 32, "c": 64, "d": 512},
        b1="a", b2="b",
    ),
    "pce": dict(
        explore=True, coverage=True, interp=["--interp"], rom_arg=False,
        input="momentary-named",
        names={"i": 1, "ii": 2, "select": 4, "run": 8,
               "up": 16, "right": 32, "down": 64, "left": 128},
        b1="i", b2="ii",
    ),
    "pico8": dict(
        explore=False, coverage=False, interp=[], rom_arg=False,
        input="colon",
        names={"left": 1, "right": 2, "up": 4, "down": 8, "o": 16, "x": 32},
        b1="o", b2="x",
    ),
}

RUN_HASH = re.compile(r"run_hash=([0-9a-fA-F]+)")


# ---------------------------------------------------------------- entrées

def mask_of(names_on, table):
    m = 0
    for n in names_on:
        m |= table[n]
    return m


def random_events(seed, frames, sys_):
    """La main plausible de m0_explore.h, traduite en événements
    (trame, masque) trame par trame ; les rendeurs compriment ensuite."""
    rng = __import__("random").Random((seed or 1) * 2654435761 % (1 << 32))
    table, b1, b2 = sys_["names"], sys_["b1"], sys_["b2"]
    # le bouton qui lance ou met en pause : Start, ou RUN sur PC Engine
    start = next((n for n in ("start", "run") if n in table), None)
    dirs = [(), ("right",), ("left",), ("up",), ("down",),
            ("right", "up"), ("right", "down"), ("left", "up"), ("left", "down")]
    weights = [20, 30, 15, 8, 8, 5, 5, 5, 4]
    events, frame, held, tap_left = [], 0, 0, 0
    while frame < frames:
        r, keys = rng.randrange(100), ()
        for k, w in zip(dirs, weights):
            if r < w:
                keys = k
                break
            r -= w
        hold = 8 + rng.randrange(56)
        pressed = set()
        if rng.randrange(100) < 45:
            pressed.add(b1)
        if rng.randrange(100) < 25:
            pressed.add(b2)
        if start and rng.randrange(100) < 2:
            pressed.add(start)
        tap_left = 4 + rng.randrange(8) if pressed and rng.randrange(2) else 0
        for _ in range(min(hold, frames - frame)):
            held = mask_of(keys, table) | mask_of(pressed, table)
            events.append((frame, held))
            frame += 1
            if tap_left:
                tap_left -= 1
                if not tap_left:
                    pressed.discard(b1)
                    pressed.discard(b2)
    return events


def render_input(dialect, events, table):
    """Les événements dans le dialecte du coeur : un texte de fichier,
    ou la chaîne « at:mask:len,... » du PICO-8."""
    if dialect == "colon":
        parts, i = [], 0
        while i < len(events):
            j = i
            while j < len(events) and events[j][1] == events[i][1]:
                j += 1
            if events[i][1]:
                parts.append("%d:%d:%d" % (events[i][0], events[i][1], j - i))
            i = j
        return ",".join(parts) + "\n"

    def names(mask):
        return "none" if not mask else "+".join(
            n for n, bit in sorted(table.items(), key=lambda kv: kv[1])
            if mask & bit)

    lines = []
    if dialect == "momentary-named":
        for frame, mask in events:
            if mask:
                lines.append("%d %s" % (frame, names(mask)))
    else:  # held-named et mask : une ligne quand l'état change
        last = None
        for frame, mask in events:
            if mask != last:
                last = mask
                lines.append("%d %s" % (frame, names(mask) if dialect == "held-named"
                                        else str(mask)))
    return "".join(l + "\n" for l in lines)


# ------------------------------------------------------- fusion couverture

def is_int(token):
    try:
        int(token, 10)
        return True
    except ValueError:
        return False


def parse_coverage(path):
    """(adresse | « adresse compteur » | « adresse compteur note » |
    « banc adresse drapeaux compteur ») -> ({clé: [compteur, notes,
    drapeaux]}, adresse_seule ?).  Le format Game Boy porte ses drapeaux
    (1=exécuté, 2=entrée) en colonne 3 : ils fusionnent par OU, la clé
    reste « banc adresse »."""
    entries, single = {}, True
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            toks = line.split()
            if len(toks) == 1:  # adresse seule (Mega Drive)
                old = entries.get(toks[0])
                entries[toks[0]] = [(old[0] + 1) if old else 1, set(), 0]
                continue
            single = False
            if len(toks) == 4 and all(is_int(t) for t in toks):  # Game Boy v1
                key = toks[0] + " " + toks[1]
                e = entries.setdefault(key, [0, set(), 0])
                e[0] += int(toks[3], 10)
                e[2] |= int(toks[2], 10)
                continue
            notes, end = set(), len(toks)
            while end > 2 and not is_int(toks[end - 1]):
                end -= 1
                notes.add(toks[end])
            key, count = " ".join(toks[:end - 1]), int(toks[end - 1], 10)
            if key in entries:
                entries[key][0] += count
                entries[key][1] |= notes
            else:
                entries[key] = [count, notes, 0]
    return entries, single


def merge_coverage(paths):
    merged, single = {}, True
    for p in paths:
        entries, alone = parse_coverage(p)
        single = single and alone
        for key, (count, notes, flags) in entries.items():
            if key in merged:
                merged[key][0] += count
                merged[key][1] |= notes
                merged[key][2] |= flags
            else:
                merged[key] = [count, set(notes), flags]
    if single:  # dialecte adresse seule : adresse par ligne, en ordre
        return "".join(k + "\n" for k in sorted(merged))
    rows = sorted(merged.items(), key=lambda kv: (-kv[1][0], kv[0]))
    return "".join(
        ("%s %d %d" % (k, v[2], v[0]) if v[2] else "%s %d" % (k, v[0]))
        + "".join(" " + n for n in sorted(v[1])) + "\n"
        for k, v in rows)


# ------------------------------------------------------------------ runs

def harness_argv(sys_, a, cov, input_path):
    argv = [a.harness]
    if getattr(a, "bios", None) and sys_.get("bios"):
        argv += ["--bios", a.bios]
    if sys_["rom_arg"] and a.rom:
        argv += [a.rom]
    argv += sys_["interp"] if a.interp else []
    if input_path:
        argv += ["--input", str(input_path)]
    if cov:
        argv += ["--coverage", str(cov)]
    return argv


def run_session(argv, frames, timeout):
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-2000:] + proc.stderr[-2000:])
        raise SystemExit("m0search : le harnais a échoué (%d) : %s"
                         % (proc.returncode, " ".join(map(str, argv))))
    return RUN_HASH.search(proc.stdout)


def cmd_run(a):
    sys_ = SYSTEMS[a.system]
    outdir = Path(a.out)
    outdir.mkdir(parents=True, exist_ok=True)
    if sys_.get("bios") and not a.bios:
        raise SystemExit("m0search : --bios est requis pour le Vectrex")
    if sys_["rom_arg"] and not a.rom:
        raise SystemExit("m0search : --rom est requis pour %s" % a.system)

    # le plan : une longue partie semée sur les coeurs à couverture
    # (l'explorateur du harnais est retiré : deepeel explore), sinon N
    # sessions scriptées, puis la session
    # idle (aucune touche : les écrans titre et les modes démo), puis les
    # rejeux manuels ; (nom, fichier d'entrées ou None, budget de trames)
    total_frames = a.frames * a.sessions
    plan = []
    if sys_["explore"] and not a.no_explore:
        plan.append(("explore", None, total_frames))
    else:
        for i in range(a.sessions):
            inp = outdir / ("session-%02d.input" % (i + 1))
            inp.write_text(render_input(sys_["input"],
                                        random_events(a.seed + i, a.frames, sys_),
                                        sys_["names"]))
            plan.append((inp.stem, inp, a.frames))
    if a.idle_frames:
        plan.append(("idle", None, a.idle_frames))
    for i, manual in enumerate(a.input):
        plan.append(("manual-%02d" % (i + 1), Path(manual), a.manual_frames))

    report = {"system": a.system, "harness": a.harness, "seed": a.seed,
              "frames_per_session": a.frames, "idle_frames": a.idle_frames,
              "sessions": [], "union": 0}
    covs, union = [], set()
    for name, inp, frames in plan:
        cov = outdir / (name + ".cov") if sys_["coverage"] else None
        argv = harness_argv(sys_, a, cov, inp)
        if sys_["explore"] and name == "explore":
            # the coverage-guided explorer is retired (deepeel explores):
            # the explore session is a long seeded play
            argv += ["--frames", str(frames), "--random", str(a.seed)]
        else:
            argv += ["--frames", str(frames)]
        row = {"name": name, "argv": [str(x) for x in argv], "frames": frames}
        h = run_session(argv, frames, a.timeout)
        if h:
            row["run_hash"] = h.group(1)
        if cov:
            covs.append(cov)
            keys = set(parse_coverage(cov)[0])
            row["locations"] = len(keys)
            row["new"] = len(keys - union)
            union |= keys
        report["sessions"].append(row)
        print("%-12s locations=%-6s new=%-6s %s" % (
            name, row.get("locations", "-"), row.get("new", "-"),
            "run_hash=" + row["run_hash"] if "run_hash" in row else ""),
            flush=True)

    if covs:
        (outdir / "coverage.merged").write_text(merge_coverage(covs))
        report["union"] = len(union)
        report["merged"] = str(outdir / "coverage.merged")
        print("fusion : %d adresses -> %s" % (len(union), report["merged"]))
    (outdir / "search.json").write_text(json.dumps(report, indent=1))
    print("rapport : %s" % (outdir / "search.json"))


def cmd_merge(a):
    lines = merge_coverage(a.files)
    if a.out:
        Path(a.out).write_text(lines)
        print("%s : %d lignes" % (a.out, len(lines.splitlines())))
    else:
        sys.stdout.write(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="sessions random et manuelles, fusion")
    r.add_argument("--system", choices=sorted(SYSTEMS), required=True)
    r.add_argument("--harness", required=True, help="binaire du harnais")
    r.add_argument("--rom", help="cartouche (harnais qui la lisent)")
    r.add_argument("--bios", help="BIOS (Vectrex)")
    r.add_argument("-o", "--out", default="output/search")
    r.add_argument("--sessions", type=int, default=4,
                   help="sessions random (1 long explorateur sur les "
                        "coeurs à --explore : total = sessions x frames)")
    r.add_argument("--frames", type=int, default=1500)
    r.add_argument("--no-explore", action="store_true",
                   help="sessions scriptées au hasard au lieu de l'explorateur "
                        "guidé par la couverture")
    r.add_argument("--idle-frames", type=int, default=6000,
                   help="la session sans touches (0 : aucune) ; les modes "
                        "démo n'entrent dans la couverture que là")
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--input", action="append", default=[],
                   help="enregistrement manuel à rejouer (répétable)")
    r.add_argument("--manual-frames", type=int, default=6000,
                   help="budget du rejeu manuel")
    r.add_argument("--interp", action="store_true",
                   help="passe le drapeau interpréteur du coeur")
    r.add_argument("--timeout", type=int, default=1800)
    r.set_defaults(func=cmd_run)

    m = sub.add_parser("merge", help="fusionner des fichiers de couverture")
    m.add_argument("files", nargs="+")
    m.add_argument("-o", "--out")
    m.set_defaults(func=cmd_merge)

    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
