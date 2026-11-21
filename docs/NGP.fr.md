# Neo Geo Pocket / Neo Geo Pocket Color (`--system ngp`)

`--system ngp` (extensions `.ngp`, `.ngc`, `.ngd`) génère deux formes de
projets : le harness hôte (`--target desktop`, images PPM et WAV hors
écran) et la **fenêtre PC** (`--target pc`) — le shell SDL2 commun
(`targets/ngp/platform/ngp_pc.c`) : clavier (flèches, X = A, Z = B,
Entrée = Option, Échap quitte), manette SDL2, son T6W28 en file SDL,
`--frames`/`--ppm` pour une passe hors écran, `--input`/`--record-input`
pour les sessions (le dialecte du harness). La cartouche est embarquée
dans l'exécutable, ou chargée au démarrage avec `--rom-external` (le
projet ne porte alors aucun octet de cartouche ; `--rom FILE` ou le
fichier à côté de l'exécutable, reconnu par taille et CRC-32).

Les deux formes impriment le même `run_hash` (la recette du fingerprint
est la même). Le e2e (`tests/test_ngp_e2e.py`) tient deux cartouches
synthétiques (mono et couleur, `tests/ngp_fixture.py`) : déterminisme du
hash, compteur V-INT, image évolutive, palettes couleur, aller-retour
d'état, hash identiques entre l'interpréteur du dépôt, le harness généré
et la fenêtre PC.

```bash
build/bin/m0recomp --system ngp "game.ngc" --target pc -o out/game_pc
cmake -G Ninja -S out/game_pc -B out/game_pc/build && ninja -C out/game_pc/build
out/game_pc/build/game_pc --frames 300 --ppm shot.ppm
# frames=300 pc=200039 run_hash=…   (le harness donne le même)
```

## La machine

- **TLCS-900H** (`targets/ngp/runtime/ngp_cpu.c`) : l'interpréteur est la
  référence et reste dans chaque projet ; l'émetteur TLCS-900H → C
  (section plus bas) traduit ce que la marche statique prouve, tout le
  reste reste interprété. L'espace d'adressage est plat sur 24
  bits, avec les banques de registres XWA/XBC/XDE/XHL et les index
  XIX/XIY/XIZ/XSP ; les 16 conditions portent sur les drapeaux S/Z/V/C et
  DJNZ apporte sa propre condition. Le décodeur statique de deepeel
  (`src/cpu/tlcs900/` côté deepeel) a été réconcilié instruction par
  instruction avec cet interpréteur : les octets d'opérande `(mem)`
  précèdent l'octet d'opération, les décades ALU suivent `SRC_ALU`,
  l'ordre des registres 8 bits est celui du runtime (`reg_b`).
- **K2GE** (`ngp_gfx.c`) : le mode suit l'octet 0x23 de l'en-tête
  (0x00 mono, 0x10 couleur — la cartouche sait sur quelle console elle
  tourne, et une cartouche couleur-aware mono comme Samurai Shodown!
  lit le tampon BIOS 0x6F92). Palette, sprites, VRAM et copy tunnel
  tracés ; `ngp_trace_dma` (build `NGP_TRACE`) expose les copies pour le
  pont deepeel.
- **T6W28 + Z80 son** (`ngp_psg.c`, `ngp_z80.c`) : le PSG du pocket et
  le Z80 partagé avec le SG-1000.
- **BIOS synthétisé** (`ngp_bios.c`) : l'amorce n'exécute pas le code
  d'allumage réel ; le runtime écrit lui-même l'espace de travail que le
  BIOS aurait rempli — l'entrée de l'en-tête (0x1C, 24 bits), le
  catalogue, le nom, le mode, la table des 18 vecteurs en RAM $6FB8
  (repli RETI), le tampon de la manette $6F82, le tampon « machine
  couleur » 0x6F91/0x6F92, le type de flash sondé 0x6C58.
- **Manette** : 9 sondes de valeur dans le pont deepeel (`--pad-full`
  pour les 256 masques, hors défaut).

## L'en-tête et les variants d'image

L'en-tête fait 0x30 octets : licence (28 octets), entrée 24 bits (0x1C),
catalogue (0x20), sous-catalogue (0x22), mode couleur (0x23), nom (12
octets à 0x24, ASCII ou UTF-16 côté japonais). Certains dumps portent un
préfixe de copieur : l'en-tête est alors à 0x20100 et le préfixe est
retiré (même règle côté deepeel), l'image se mappe ensuite à plat depuis
$200000. `m0recomp --info` imprime `system=ngp`, le titre, la taille et
`colour=yes|no`.

## Les sauvegardes

La cartouche sauvegarde dans sa propre flash ; le runtime écrit les
fenêtres flash dans sa copie et le harness/la fenêtre persiste le tout :
fichier `<cartouche>.ngpflash` à côté de l'image passée en argument, sinon
`<programme>.ngpflash` dans le répertoire courant ; `--ram-out`/`--ram-in`
forcent le chemin. Le fichier porte le nombre d'octets attendus en tête,
un fichier d'une autre cartouche est ignoré au chargement.

## Le pont deepeel

`m0boy ROM.ngc` lance deepeel (intégré au build ou `DEEPEEL=` sur le
chemin) quand aucune couverture ne se trouve à côté de la ROM
(`<nom>.deepeel/`), et garde les sorties (`.cov`, carte PNG, `.rtcode`)
à côté de la cartouche. Le profil ne pilote pas encore la génération :
la marche de l'émetteur est purement statique (elle part de l'entrée de
l'en-tête), la couverture deepeel servira à y ajouter ce que la marche
ne voit pas (tables de sauts, code copié en WRAM).

## L'émetteur TLCS-900H → C

Par défaut (`--interp` pour s'en passer), la génération produit
`src/ngp_gen.c` : la marche statique part de l'entrée de l'en-tête et
suit ce qu'elle prouve (JP/JR/JRL/CALL/CALR/DJNZ, `JP cc,(abs)` à
adresse constante) ; chaque instruction prouvée devient une petite
fonction C qui plie la lecture d'octets, l'adressage `(mem)` et le
dispatch primaire en constantes, puis appelle le gestionnaire de
l'interpréteur lui-même (`ngp_gen_exec` / `ngp_gen_exec1` dans
`ngp_cpu.c`) — les mêmes expressions, cycle pour cycle. Le dispatcher
(`ngp_step` sous `NGP_NATIVE`) entre une instruction traduite par appel,
avec le pointeur d'entrée précédemment exécuté comme indice (un hachage
par changement de flux, pas par instruction) ; là où la recherche
échoue, l'interpréteur prend l'instruction. Comme les interruptions sont
acceptées dans `ngp_tick`, entre deux `ngp_step`, le pas d'entrée reste
l'instruction : les frontières de cycle — minuteries, points
d'acceptation d'interruption, front de trame — tombent sur les mêmes
ticks qu'en interprétation, et le `run_hash` est identique par
construction (le e2e le vérifie sur les deux cartouches synthétiques).

Restent hors de portée de la marche (et donc à l'interpréteur, sans
aucun effet de bord) : le code copié en WRAM, les tables de sauts
lues en RAM, les opcodes non définis, SWI et les sorties de
procédure (RET/RETI), les gestionnaires d'interruption installés par
écriture dans la table de vecteurs $6FB8 — le gestionnaire V-INT d'une
cartouche réelle n'est atteint que par ce chemin.

Options : `--interp` désactive la traduction (projet interprétant tout,
même runtime) ; `--meta-code-budget N` plafonne le poids de `ngp_gen.c`
(la marche s'arrête, le reste reste interprété).

Perf, honnêtement : sur la cartouche synthétique du e2e (boucle
d'attente minuscule, ~260 instructions exécutées par trame), la version
traduite mesure ~10 % plus lentement que l'interpréteur — l'entrée par
instruction (appel indirect, état de décodage réinitialisé) coûte plus
que la lecture+décodage en ligne qu'elle remplace sur un si petit
programme. La limite est la contrepartie du contrat de hash : tant que
l'acceptation d'interruption se produit entre deux instructions, il
faut une entrée par instruction. L'étape suivante, comme le PCE l'a
fait, est l'entrée par bloc de base qui rappelle l'horloge à
l'intérieur du bloc — elle amortira ces appels.

## État et limites

- Interpréteur + code traduit sur desktop et pc (étiquette **vérifié**
  sur les cartouches synthétiques : hash identiques entre
  l'interpréteur pur, le projet traduit et la fenêtre PC) ; la marche
  est purement statique et part de l'entrée de l'en-tête — une
  cartouche réelle dont le flux passe par des tables de sauts verra
  l'essentiel de son code interprété (correct, pas encore accéléré).
- Les surfaces matérielles suivent le même inventaire que la couverture
  deepeel (voir `docs/COUVERTURE.fr.md` §3.7 côté deepeel) : le port de
  communication (SIO) n'est pas modélisé, la RTC est lue statique.
- Cibles : `desktop` et `pc` (les consoles attendent l'entrée par bloc
  et l'apport deepeel — l'interpréteur TLCS-900H est trop lourd pour un
  Cortex-M0+).
