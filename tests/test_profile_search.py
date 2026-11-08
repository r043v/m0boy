"""m0search.py (tools/profile) : la recherche de profil générique.

Sans ROM : les fuseurs de couverture de chaque format du dépôt (adresse
seule, « adresse compteur » v2, « adresse compteur d » du NES, Game Boy v1
« banc adresse drapeaux compteur »), la main random (déterminisme, dialectes
d'entrées), puis un harnais factice qui découvre des adresses d'après les
entrées reçues : sessions random, rejeu manuel, fusion, rapport.
"""
import json
import sys
from pathlib import Path
import subprocess
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "profile"))
import m0search  # noqa: E402

SEARCH = str(Path(__file__).resolve().parents[1] / "tools" / "profile" / "m0search.py")

FAKE_HARNESS = r"""#!/usr/bin/env python3
# harnais factice : découvre des adresses déterministes tirées du contenu
# des entrées (ou de la graine de l'explorateur), les compte en v2.
import hashlib, sys
from pathlib import Path
args = sys.argv[1:]
rom, cov, inp, frames, explore, seed = None, None, None, 600, 0, 1
for i, a in enumerate(args):
    if a == "--coverage": cov = args[i + 1]
    elif a == "--input": inp = args[i + 1]
    elif a == "--frames": frames = int(args[i + 1])
    elif a == "--explore": explore = int(args[i + 1])
    elif a == "--random": seed = int(args[i + 1])
text = Path(inp).read_text() if inp else "boot"
if explore:
    text = "explore %d %d" % (explore, seed)
digest = hashlib.sha1(((rom or "") + text).encode()).digest()
pool = [int.from_bytes(digest[i:i + 2], "big") % 0x10000 for i in range(0, 12, 2)]
hits = {("%04x" % a): 1 + (a % 7) for a in pool[:max(1, len(pool) - len(pool) // 3)]}
if cov:
    with open(cov, "w") as f:
        for a, n in sorted(hits.items()):
            f.write("%s %d\n" % (a, n))
print("frames=%d run_hash=%08x" % (frames, int(hashlib.sha1(text.encode()).hexdigest()[:8], 16)))
"""


def write(path, text):
    Path(path).write_text(text, encoding="utf-8")
    return str(path)


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def test_merge():
    with tempfile.TemporaryDirectory() as tmp:
        a = write(Path(tmp) / "a.cov", "# m0boy NES coverage\n8000 5 d\n8001 2\n")
        b = write(Path(tmp) / "b.cov", "8000 7\n8002 1 d\n")
        merged = m0search.merge_coverage([a, b])
        check(merged == "8000 12 d\n8001 2\n8002 1 d\n", "v2 + note NES : %r" % merged)

        md1 = write(Path(tmp) / "md1.cov", "00955a\n123456\n")
        md2 = write(Path(tmp) / "md2.cov", "123456\nabcdef\n")
        merged = m0search.merge_coverage([md1, md2])
        check(merged == "00955a\n123456\nabcdef\n", "adresse seule : %r" % merged)

        g1 = write(Path(tmp) / "g1.cov", "# gbl coverage v1\n00 1234 1 10\n01 0040 3 4\n")
        g2 = write(Path(tmp) / "g2.cov", "00 1234 2 5\n")
        merged = m0search.merge_coverage([g1, g2])
        check(merged == "00 1234 3 15\n01 0040 3 4\n", "Game Boy v1 : %r" % merged)


def test_random_and_dialects():
    md = m0search.SYSTEMS["megadrive"]
    ev1 = m0search.random_events(3, 2000, md)
    ev2 = m0search.random_events(3, 2000, md)
    check(ev1 == ev2, "même graine, même partie")
    ev3 = m0search.random_events(4, 2000, md)
    check(ev1 != ev3, "graines différentes, parties différentes")
    check([f for f, _ in ev1] == list(range(2000)), "chaque trame a ses touches")
    check(all(0 <= m <= 255 for _, m in ev1), "masques du Mega Drive sur 8 bits")

    # PC Engine : le bouton de la partie s'appelle RUN, les sessions
    # aléatoires l'appuient et le rendent en un nom que le harnais lit
    pce = m0search.SYSTEMS["pce"]
    ev = m0search.random_events(5, 4000, pce)
    check(any(m & pce["names"]["run"] for _, m in ev),
          "PCE : les sessions appuient sur RUN")
    text = m0search.render_input("momentary-named", ev, pce["names"])
    check(all(t in pce["names"] for l in text.splitlines()
              for t in l.split(None, 1)[1].split("+")),
          "PCE : des noms que le harnais lit tout entiers")

    text = m0search.render_input("mask", ev1, md["names"])
    lines = [l.split() for l in text.splitlines()]
    frames = [int(l[0]) for l in lines]
    check(frames == sorted(set(frames)), "mask : trames strictement croissantes")
    check(all(0 <= int(l[1]) <= 255 for l in lines), "mask : masques 8 bits")

    nes = m0search.SYSTEMS["nes"]
    ev = m0search.random_events(1, 300, nes)
    text = m0search.render_input("momentary-named", ev, nes["names"])
    held = set()
    replay = {}
    for line in text.splitlines():
        at, names = line.split(None, 1)
        mask = 0
        for tok in names.split("+"):
            for n, bit in nes["names"].items():
                if tok == n:
                    mask |= bit
        replay[int(at)] = mask
    for f, m in ev:
        if m:
            check(replay.get(f) == m, "momentary : la trame %d garde ses touches" % f)

    p8 = m0search.SYSTEMS["pico8"]
    ev = m0search.random_events(2, 500, p8)
    text = m0search.render_input("colon", ev, p8["names"])
    back = {}
    for part in text.strip().split(","):
        at, mask, length = (int(x) for x in part.split(":"))
        for f in range(at, at + length):
            back[f] = mask
    for f, m in ev:
        check(back.get(f, 0) == m, "colon : la trame %d rejoue %d" % (f, m))


def test_run_and_merge():
    with tempfile.TemporaryDirectory() as tmp:
        harness = write(Path(tmp) / "fake_harness.py", FAKE_HARNESS)
        Path(harness).chmod(0o755)
        manual = write(Path(tmp) / "partie.input", "0 8\n100 0\n200 16\n260 0\n")
        out = str(Path(tmp) / "search")
        subprocess.run(
            [sys.executable, SEARCH, "run", "--system", "megadrive",
             "--harness", harness, "-o", out, "--sessions", "3",
             "--frames", "400", "--seed", "9", "--input", manual],
            check=True, capture_output=True, text=True)

        check((Path(out) / "explore.cov").exists(), "l'explorateur a sa couverture")
        check((Path(out) / "manual-01.cov").exists(), "le rejeu manuel a sa couverture")
        check((Path(out) / "idle.cov").exists(), "la session idle a sa couverture")
        check(not list(Path(out).glob("session-*.input")),
              "pas de sessions scriptées sur un coeur à explorateur")

        report = json.loads((Path(out) / "search.json").read_text())
        sessions = report["sessions"]
        check([s["name"] for s in sessions] == ["explore", "idle", "manual-01"],
              "le plan : l'explorateur, l'idle sans touches, le rejeu manuel")
        check(sessions[1]["frames"] == 6000, "idle : le budget par défaut")
        check("--frames" in sessions[0]["argv"] and "--random" in sessions[0]["argv"],
              "la longue partie semée part de la graine")
        check(sessions[0]["new"] == sessions[0]["locations"], "première session : tout est nouveau")
        check(report["union"] > 0 and report["merged"].endswith("coverage.merged"), "fusion annoncée")

        merged = m0search.parse_coverage(str(Path(out) / "coverage.merged"))[0]
        all_keys = set()
        for cov in [Path(out) / "explore.cov", Path(out) / "idle.cov", Path(out) / "manual-01.cov"]:
            all_keys |= set(m0search.parse_coverage(str(cov))[0])
        check(set(merged) == all_keys, "la fusion garde toutes les adresses découvertes")

        # NES : le même plan à l'explorateur ; le dialecte momentary-named
        # ne sert plus qu'aux rejeux manuels (test_random_and_dialects)
        out_nes = str(Path(tmp) / "search-nes")
        subprocess.run(
            [sys.executable, SEARCH, "run", "--system", "nes", "--harness", harness,
             "-o", out_nes, "--sessions", "2", "--frames", "200"],
            check=True, capture_output=True, text=True)
        report = json.loads((Path(out_nes) / "search.json").read_text())
        check([s["name"] for s in report["sessions"]] == ["explore", "idle"],
              "NES : l'explorateur puis l'idle sans touches")
        check("--frames" in report["sessions"][0]["argv"] and "--random" in report["sessions"][0]["argv"],
              "NES : le harnais reçoit la partie semée")


def main():
    test_merge()
    test_random_and_dialects()
    test_run_and_merge()
    print("profile_search: OK")


if __name__ == "__main__":
    main()
