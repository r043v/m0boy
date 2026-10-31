# CPU, console d’origine et destination

La sélection d’un jeu comporte désormais trois identités indépendantes :

Les images de firmware Cortex-M0+ sont également reconnues comme **sources**
explicites : `--system meta` pour une application SAMD21 liée à `0x4000`,
`--system pokitto` pour une application LPC11U68 liée à `0x0000`. Les deux
partagent `--cpu m0plus`. Un `.bin` brut ne permet pas de déduire sa console :
il faut donner `--system`. `--info` vérifie les vecteurs de pile et de reset ;
`--disasm` liste les instructions Thumb depuis le reset, avec `--limit` pour
borner la lecture. Le désassemblage est linéaire : il peut traiter des données
comme du code. Les encodages non reconnus sont affichés en `.hword`.

```bash
build/bin/m0recomp firmware.bin --system meta --cpu m0plus --info
build/bin/m0recomp firmware.bin --system pokitto --disasm --limit 40
```

`--system meta --target pc` (et `--target aka`) écrit un projet **sans émulateur** :
le code Thumb de `firmware.bin` atteint par le parcours statique et par
`--coverage` est traduit en C, une fonction par Ko de flash
(`src/m0_native_page_*.c`). Dans la fonction, chaque instruction a un label,
les branchements directs de la page sont des `goto`, les cycles d'une chaîne
d'instructions s'additionnent dans une variable locale, et la machine (timers,
DMA, NVIC) n'est consultée qu'aux endroits où elle peut avoir changé : au début
d'un bloc, où la marge avant la prochaine échéance (`samd21_room`) doit couvrir
le plus long chemin du bloc — sinon l'instruction passe par l'interpréteur
`m0_cpu.c`, qui avance l'horloge instruction par instruction —, et après une
instruction qui a touché la machine (accès hors SRAM, saut d'attente, PRIMASK).
Mêmes cycles et mêmes effets de bord que l'interpréteur, pas de décodage ni de
dispatch par instruction (`m0_native_glue.h` est le vocabulaire des corps).
`m0_machine.c` fournit SysTick et le NVIC, `samd21.c` la carte Gamebuino META
(TC4/TC5, DMAC, SERCOM4 avec l'écran ST7735 et le pavé, PORT et IOBUS, DAC, ADC,
SYSCTRL/GCLK). gm0 n'est pas dans le projet. Les périphériques sont paresseux :
ils rattrapent l'horloge du CPU quand le guest touche un registre ou à la
prochaine échéance ; le flux DMA vers l'écran est déroulé en bloc quand il va
d'une mémoire SRAM à la fenêtre RAMWR ; les boucles d'attente (chargement,
comparaison, branchement en arrière) dont la valeur ne change qu'à un événement
sont avancées d'un coup jusqu'à cet événement.

```bash
build/bin/m0recomp firmware.bin --system meta --target pc --coverage p1.cov -o output/firmware-pc
cmake -G Ninja -S output/firmware-pc -B output/firmware-pc/build
ninja -C output/firmware-pc/build
output/firmware-pc/build/m0_firmware_pc          # fenêtre SDL2 : flèches, J=A K=B U=menu I=home
output/firmware-pc/build/m0_headless --frames 600 --ppm out.ppm --wav out.wav
```

Règle : `m0_headless --frames N` imprime `run_hash`, `image_hash`, `audio_hash` ;
`--interp` (interpréteur seul) doit imprimer la même ligne (hors `idle_skipped`).
Le profil est standard : `m0_headless --frames N --random SEED --profile p`
(ou `--input script`, lignes « image masque-hexa ») écrit `p.cov` (instructions
exécutées, relu par la régénération), `p.mmio` (accès aux registres ; une lecture
`UNMODELLED-READ` est un registre dont la carte répond 0 sans modèle) et `p.hot`
(cycles par instruction). Non modélisés pour l'instant : la carte SD (lue comme
absente), l'écriture de la flash, les attentes du cache NVM (le décompte de
cycles diffère donc légèrement de gm0 : Lapinou, 300 images, 241 094 733 contre
241 095 159 cycles ; l'image est identique hors compteur de fps, et les valeurs
écrites dans le DAC sont identiques à 99,99 %).

`--system pokitto --target pc` garde l'ancien chemin : une copie intégrée du
modèle de [gm0](https://github.com/r043v/gm0), remise sous MIT par son auteur
pour m0boy (`targets/m0plus/gm0/LICENSE` conserve les notices tierces MIT),
avec `run.py` et `m0_native_pc` (harnais CPU : flash, SRAM, SysTick, NVIC).

`--system meta --target aka` écrit le projet PlatformIO du même runtime
(`targets/m0plus/platform/m0_aka_native.cpp` : la carte, l'écran par le pilote
i80 de l'AKA, l'audio du cœur ESP32 partagé). Il se construit (`pio run`) ; il
n'est pas validé sur la console, la carte SD n'y est pas modélisée et la
vitesse sur l'ESP32-S3 n'est pas mesurée. Pokitto n'a pas de cible AKA. Un firmware META ne tourne pas sur Pokitto, ni
l'inverse.

| Axe | Game Boy | SG-1000 | Game Gear | Master System | PICO-8 |
| --- | --- | --- | --- | --- | --- |
| Console d’origine (`--system`) | `gameboy` | `sg1000` | `gg` | `sms` | `pico8` |
| CPU requis (`--cpu`, contrôle facultatif) | `sm83` | `z80` | `z80` | `z80` | `lua` |
| Destination (`--target`) | `desktop`, `meta`, `pokitto` | `desktop` (harness), `meta`, `pokitto` | idem | idem | `desktop` (harness), `meta`, `pokitto`, `espboy` |
| État actuel | Pipeline existant | Code Z80 recompilé, runtime TMS9918A/SN76489, cartouches sans mapper | Code Z80 recompilé par banques, runtime VDP mode 4/PSG, mappers Sega et Codemasters, ROM et RAM de cartouche sur la carte SD | Runtime Game Gear, image entière réduite 256 → 160 | Lua compilé en C sur le runtime PICO-8 (notes : `docs/PICO8.fr.md`) |

`--target sg1000` n’est pas une destination : la SG-1000 est la machine
dont on traduit le programme. META et Pokitto restent les machines sur
lesquelles les firmwares Game Boy s’exécutent.

## Organisation

- `recompiler/include/m0boy/memory.h` : interface `ByteReader`. Un décodeur
  reçoit des octets et une adresse, sans connaître le format de cartouche.
- `recompiler/include/m0boy/system.h`, `recompiler/src/system.cpp` : registre
  des CPU, des consoles d’origine et de leurs capacités de génération.
- `recompiler/include/m0boy/cpu/encoding.h` : codes de registres, champs
  `x/y/z/p/q`, lecture little-endian et calcul des branches relatives communs.
- `recompiler/src/cpu/z80/`, `include/m0boy/cpu/z80/` : désassemblage Z80,
  préfixes `CB/ED/DD/FD`, registres indexés, timings et métadonnées de contrôle
  de flux. La bibliothèque `m0boy_cpu_z80` ne dépend pas des cartouches.
- `recompiler/src/cpu/sm83/`, `include/recompiler/cpu/sm83/` : décodage SM83.
  La bibliothèque CMake `m0boy_cpu_sm83` se construit sans le parseur Nintendo.
- `recompiler/src/system/gameboy/` : cartouche Game Boy, analyse des banques,
  suivi des mappers et balayage du code par banque.
- `recompiler/src/system/sg1000/`, `include/m0boy/sg1000*.h` : cartouche
  SG-1000, recompilation Z80 → C (`emitter.cpp`) et génération des projets.
- `m0boy_frontends` : chargement et métadonnées des consoles d’origine.
- `gbrecomp_core` : analyse, IR et générateurs Game Boy existants.
- `targets/meta/` : cœur Game Boy (runtime `gbl`, frontend `platform/gb_main.cpp`). `targets/common/platform/` : frontend commun à toutes les consoles (`m0_*`). `targets/consoles/<console>/` : pilotes de chaque console.
- `targets/sg1000/runtime/` : runtime SG-1000 commun au code recompilé et à
  l’interpréteur Z80 de repli (bus, événements, TMS9918A, SN76489,
  `sg_gen.h` pour le code généré). `targets/sg1000/platform/` : frontend des
  consoles, qui réutilise les pilotes écran, son, horloge, démarrage et
  boutons de META/Pokitto. `targets/sg1000/host/` : harness et lecteur SDL.
 `targets/nes/platform/`
  : frontend META et Pokitto ; l’image vient du PPU lui-même, 160×128 ou
  160×176 (voir « Rendu NES sur le panneau »).
  `targets/nes/host/` : harness (PPM, WAV, `--input`, `--interp`,
  `--coverage`, `--skip`, `--rows 176`). Génération : `recompiler/src/system/nes/generator.cpp`
  (META, Pokitto et harness hôte), code 6502 recompilé par
  `recompiler/src/system/nes/emitter.cpp` (voir « Recompilation 6502 »).
- `targets/gamegear/` : runtime Game Gear (`gg.c` bus, mapper, ports et
  moteur ; `gg_vdp.c` VDP mode 4 ; `gg_psg.c` ; `gg_render_asm.S` ;
  `sg_gen.h` pour le code généré), frontend des consoles
  (`platform/gg_main.cpp`, `gg_stream.c` pour la carte SD) et harness
  (`host/gg_host.c`). Le noyau Z80 et le recompilateur sont ceux du
  SG-1000.

Les anciens includes `recompiler/rom.h` et `recompiler/decoder.h`, le namespace
`gbrecomp` et l’exécutable `gbrecomp` restent utilisables. Le nouvel exécutable
`m0recomp` utilise exactement la même CLI. `compile.sh` traite aussi les
`.sg` ; le frontend Node reste limité au Game Boy.

L’émetteur Z80 reprend la structure de l’émetteur Game Boy (régions,
répartition, vivacité des drapeaux, cycles par lots, synchronisation aux
têtes de boucle) sans la partager encore : une couche commune (instruction
décodée, flux, cibles, cycles, drapeaux lus et écrits) est l’étape suivante
pour unifier les deux. Le runtime `gbl` reste propre à la Game Boy.

## Consoles de destination et matrice de capacités

Le registre des cibles (`system.h`/`system.cpp`) décrit chaque console de
destination par ses faits matériels (`TargetFacts` : RAM contiguë, PSRAM,
flash d'application, écran, carte SD) ; chaque console d'origine déclare
ce que son runtime exige (`min_ram_bytes` : RAM invitée + VRAM + tampons).
Une génération est possible quand un pipeline est **câblé** (une ligne
`codegen_targets`) **et** que le matériel tient le runtime
(`target_fits`) : `--list-systems` affiche le résultat, `--list-targets`
la table des faits, et un refus dit lequel des deux manque et pourquoi
(`target_misfit_reason`, avec les chiffres).

| système | câblé sur | hors hardware |
|---|---|---|
| gameboy, sg1000, gg, sms | desktop, meta, pokitto, espboy, pc, odroid, aka | — |
| msx | desktop, meta, pokitto, pc | (espboy non câblé) |
| pico8 | desktop, meta, pokitto, espboy, pc, aka | — |
| nes | desktop, meta, pokitto, pc (6502 recompilé, interpréteur de repli) | — |
| pce | desktop, pc (interpréteur HuC6280 ; voir `docs/PCE.fr.md`) | consoles > 32 Kio RAM, VRAM 64 Kio |
| vectrex | meta, pc, aka (6809 recompilé, interpréteur de repli ; voir `docs/VECTREX.fr.md`) | (pokitto et espboy non câblés) |
| megadrive | desktop, pc, aka (68000 recompilé, interpréteur de repli ; voir `docs/MEGADRIVE.fr.md`) | (meta retirée de la matrice ; pokitto et espboy non câblés) |

Les cibles pc (fenêtre SDL2, la carte en fichier `sd.img` ; les save states
sans carte — les emplacements d'un même fichier à plat dans le répertoire
courant, `m0_state.cpp` — et les sauvegardes de cartouche Game Boy via
l'image), odroid
(ODROID-GO : ESP32, ILI9341 320×240, carte SD, DAC) et aka (Gamebuino
AKA CORE v1.4 : ESP32-S3, 8 Mio flash, écran i80 320×240, SDMMC,
expandeurs I²C, codec TAS2505 — voir `docs/AKA.fr.md`) partagent le noyau ESP32 de
`targets/common/consoles/esp32/` et les frontends communs
(`targets/common/platform/`) ; chaque console n'apporte que ses pilotes
propres dans `targets/consoles/<nom>/` (une ligne dans
`recompiler/src/target_info.cpp` et les listes de copie des générateurs).

Ajouter une console de destination : `targets/consoles/<nom>/` (pilotes
`m0_lcd/m0_audio/m0_clock/pad` + templates `*.in` + `VERSION`), une
branche dans `m0_target.h`, une ligne dans `target_info.cpp`, les
branches des générateurs (GB : `meta_emitter.cpp` ; Sega :
`sg1000/generator.cpp`), l'entrée `recompiler/CMakeLists.txt` pour le
VERSION, la ligne dans `system.cpp` (`target_table` + listes câblées), un
`tests/test_<nom>_target.py` sur le modèle de l'ESPboy. Ajouter une
console d'origine : une ligne CPU si nécessaire (le 6502 sert le NES,
l'HuC6280 l'étend pour la PC Engine — table à recouper sur matériel avant
qu'un émetteur lui fasse confiance), `inspect()` dans
`recompiler/src/system/<nom>/`, la ligne `system_table` avec
`min_ram_bytes` honnête, et la matrice décide du reste.

## Utilisation

```bash
build/bin/m0recomp --list-systems
build/bin/m0recomp jeu.sg --info
build/bin/m0recomp jeu.sg --disasm --limit 32
build/bin/m0recomp jeu.bin --system sg1000 --cpu z80 --info
build/bin/m0recomp jeu.gb --system gameboy --target meta -o output/jeu
build/bin/sg1000_host jeu.sg --frames 300 --coverage output/jeu-sg.cov
build/bin/m0recomp jeu.sg --target pokitto --coverage output/jeu-sg.cov -o output/jeu-pokitto
cmake -G Ninja -S output/jeu-pokitto/host -B output/jeu-pokitto/host/build
ninja -C output/jeu-pokitto/host/build
cmake -G Ninja -S output/jeu-pokitto -B output/jeu-pokitto/build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake
ninja -C output/jeu-pokitto/build
```

Le mode `auto` reconnaît `.sg` et `.gb/.gbc/.sgb`, sans distinction de casse.
Les extensions inconnues et les dossiers conservent le choix Game Boy
historique. Pour un fichier `.bin` SG-1000, il faut donc indiquer `--system
sg1000`. `.sgb` désigne toujours une cartouche Game Boy, pas une SG-1000.
La sélection du CPU contrôle la cohérence avec la console, elle ne change
pas son matériel.

Une cartouche SG-1000 n’a pas besoin d’un en-tête Nintendo. Le profil
accepte de 1 à 48 Kio de ROM brute, sans mapper. Les tailles supérieures sont
refusées avec un diagnostic explicite. Les cartes en puissance de deux sont
reflétées dans `0000–BFFF` ; les dumps de 40 et 48 Kio sont lus tels quels,
`FF` au-delà. Une cartouche de 32 Kio au plus peut porter 8 Kio de RAM en
`8000–BFFF` (The Castle) : le profilage la détecte (écriture dans cette zone,
ligne `# cart-ram` de la couverture) et le projet l’active alors.

Le profil déclare le reset Z80 à `0000` et 1 Kio de RAM système. La
[carte mémoire SG-1000](https://www.smspower.org/Development/MemoryMap)
place la cartouche dans `0000–BFFF` et la RAM dans `C000–C3FF`, avec des
miroirs jusqu’à `FFFF`.

Le désassemblage (`--disasm`) est un balayage linéaire : des données peuvent
apparaître comme des instructions. `--limit` limite le nombre d’instructions
affichées.

## Recompilation Z80

Le code SG-1000 est recompilé, comme celui de la Game Boy : le générateur
(`recompiler/src/system/sg1000/emitter.cpp`) reprend l’architecture de
l’émetteur META.

- **Sélection** : les instructions jouées pendant le profilage (les plus
  exécutées d’abord), puis celles qu’elles atteignent statiquement (suites,
  sauts, appels, `RST`, vecteurs `0000/0038/0066`), dans la limite de la
  flash (32 octets estimés par instruction, `--meta-code-budget` pour la
  fixer). L’exploration statique s’arrête sur le remplissage (quatre octets
  `00` ou `FF`). Le code en RAM et le reste passent par l’interpréteur.
  Depuis la version v2 des fichiers de couverture, chaque adresse y porte
  son nombre d’exécutions : la fusion des sessions additionne ces compteurs
  pour guider les optimisations des sites chauds. Le classement conserve
  la pondération des sessions par leurs taux d’exécution ; les fichiers v1
  sans compteur comptent une exécution par adresse.
- **Régions** : les instructions voisines forment des fonctions C, avec des
  `goto` internes et un `switch` sur `pc` à l’entrée. `CALL` devient un appel
  C suivi d’un contrôle du retour ; les autres sorties posent `pc` et
  enchaînent sur la région cible ou rendent la main au répartiteur, qui
  cherche `pc` dans la table d’entrées triée (cache direct devant).
- **Drapeaux** : une analyse de vivacité par région ne calcule les drapeaux
  que lorsqu’une instruction suivante peut les lire (sinon, l’opération seule ;
  après `ADD HL,rr`, la seule retenue quand elle suffit).
- **Temps** : cycles et rafraîchissement `R` sont ajoutés par lots, avant tout
  ce qui les observe (ports, `LD A,R`, synchronisation, transferts). `SYNC`
  aux têtes de boucle, à l’entrée des régions et entre les itérations des
  instructions de bloc traite les événements et prend les interruptions.
- **Traduction** : les instructions courantes sont écrites en C (chargements,
  ALU, `INC/DEC`, `ADD HL`, rotations, `BIT/SET/RES`, piles, sauts, ports,
  `LDIR/LDDR/OTIR/OTDR` en boucles) ; les rares passent par la sémantique de
  l’interpréteur (`z80_ops.h`, opérandes constants). Accès mémoire,
  pile et drapeaux complets sont des fonctions hors ligne (taille de flash),
  les adresses constantes sont résolues à la génération (ROM repliée, RAM
  indexée directement).
- **EI** : l’instruction qui suit `EI` est émise en ligne, puis le délai prend
  fin et une synchronisation prend l’interruption en attente (la fenêtre
  `EI / NOP / DI` ne coûte rien de plus). Une reprise pendant un délai passe
  par l’interpréteur.

Le runtime (`targets/sg1000/runtime/`) compte le temps en T-states avec deux
événements par trame (VBlank en ligne 192, fin de trame en ligne 262). Les
lignes sont rendues à la demande, quand le programme touche le VDP ou au
VBlank, avec l’état VDP de ce moment : aucun test par ligne dans le code
généré, et une ligne voit toujours les écritures faites avant sa fin. Le PSG
est rattrapé de la même façon, avant chaque écriture et en fin de trame.

Le rendu des consoles réduit 256×192 en 160×120 (META) par un filtre boîte
exact, poids en huitièmes : chaque pixel couvre 1,6 pixel source, soit, pour
les cinq pixels d'une tuile, (5,3) (2,5,1) (4,4) (1,5,2) (3,5). Une tuile n'a
que deux couleurs : chaque pixel vaut le mélange encre/fond au poids des bits
d'encre, lu dans une table de 9 mélanges par octet de couleur, au format du
panneau (`mix`, reconstruite quand la couleur de fond change ; la palette du
TMS9918 est fixe). Le format du panneau META : RGB565 natif, octets du poids
fort d'abord (`SG_PANEL_SWAP`/`GG_PANEL_SWAP` à 1 sur META, 0 sur Pokitto
dont le bus est parallèle 16 bits natif) ; le câblage BGR de la dalle est
corrigé par le bit BGR du registre MADCTL, posé par l'init de l'écran — les
couleurs du firmware restent natives, aucune permutation logicielle. Verticalement, deux lignes source qui tombent sur la même
ligne d'écran sont rendues ensemble : poids moyen quand leurs couleurs sont
égales (une seule lecture), sinon moyenne des deux mélanges. Les tuiles sous
sprites prennent la boîte exacte 8×2 (couleurs étalées, une table de 16 avec
le fond) ; les modes texte/multicolore moyennent deux lignes filtrées. Les
boucles de tuiles des modes Graphics I et II sont en Thumb-1, exécutées en
SRAM (`sg_render_asm.S`) ; `SG_NO_RENDER_ASM` garde le C, et un firmware
construit avec `SG_SELFTEST` compare les deux sur VRAM aléatoire
(`sg_selftest_result` = 1 + lignes différentes, lu par gm0 ou un débogueur).
Les masques de sprites par ligne suivent chaque écriture d'un Y de sprite
(déplacement de ses bits) ; terminateur, taille ou table les reconstruisent.
Le rendu de référence (256 pixels) sert aux captures et aux tests de l'hôte.
Les projets avec RAM de cartouche (`SG_LOW_RAM`) remplacent la table des
mélanges par un cache de 16 et les masques par ligne par des masques par bande
de 8 lignes, sans assembleur. Le cadenceur peut sauter jusqu'à deux
présentations de suite et rattrape jusqu'à six images de retard : le jeu garde
sa vitesse.

Le PSG produit ses échantillons par blocs : un passage de Bresenham par bloc
donne les ticks de chaque échantillon, partagés par les quatre canaux ; un
canal muet n'avance que son compteur (une division). La sortie est identique
au calcul échantillon par échantillon.

## Recompilation 6502 (NES)

Le code NES suit le même plan que le Z80, avec un runtime plus simple :
NROM ne change jamais de banque, la PRG est une constante.

- **Sélection** : les vecteurs (RESET, NMI, IRQ), les adresses du profil
  (harness `--interp --coverage f`, lignes `adresse exécutions [d]`, `d`
  pour une adresse atteinte par RTS, RTI, `JMP (ind)` ou une interruption),
  puis ce qu'elles atteignent statiquement ; sur console, les jouées
  d'abord, dans la limite de la flash (24 octets estimés par instruction,
  `--meta-code-budget`, `--meta-bytes-per-instruction`). Les opcodes non
  documentés, BRK et le code en RAM restent à l'interpréteur.
- **Régions** : instructions voisines en fonctions C, `goto` internes,
  `switch` sur l'entrée. Toute sortie revient au répartiteur (GCC Thumb-1
  ne fait jamais d'appel terminal : une chaîne d'appels ferait grandir la
  pile C) ; une cible connue d'une autre région est laissée en indice,
  vérifié contre `pc` avant la table de hachage.
- **État** : A, X, Y, S en variables locales, P éclaté (N et Z du dernier
  résultat, C, V, I/D) et recomposé à la sortie.
- **Sémantique** : celle de l'interpréteur (`6502.c`), cycle pour cycle :
  mêmes comptes, mêmes pénalités de page, mêmes accès au bus dans le même
  ordre ; les lectures de PRG à adresse constante sont repliées, la RAM est
  indexée directement, le reste passe par `nes_bus_read`/`nes_bus_write`.
- **Temps** : `--meta-precise` vérifie le budget après chaque instruction
  (la tranche finit sur la même instruction que l'interpréteur, même
  `run_hash`) ; le mode normal ne le vérifie qu'aux étiquettes et au plus
  tous les 48 cycles, une tranche débordant d'autant. Une écriture dans
  `$4014` (DMA OAM), CLI et PLP (IRQ) rendent la main au répartiteur.

Mesures (SMB, 400 images avec entrées, estimateur m0bench, estimations) :
2,55 M → 1,64 M cycles/image, la part 6502 passant d'environ 1,12 M à
0,2 M ; le PPU (1,3 M) domine désormais. Dans gm0 (temps estimé), SMB en
jeu passe d'environ 14 à 22 images/s, l'APU ayant été rendu en forme close
(sortie identique, `--wav`).

### Rendu NES sur le panneau

Le PPU (`ppu.c`) produit directement les 128 lignes de 160 pixels du META,
dans l'ordre d'octets du panneau. Une ligne NES devient une rangée
d'indices de palette (fond par une table d'expansion et une rangée de tuiles
décodée une fois pour ses huit lignes ; sprites par masques par ligne de
l'OAM, refaits quand l'OAM ou la taille change). Horizontalement,
`m0_box58_fast` (256 → 160) par tables de paires de pixels ; verticalement,
15 lignes → 8 rangées : sept paires de lignes moyennées (`m0_avg2_panel`)
puis une ligne seule. Le plan vertical est la règle commune de
`m0_scale.h` (`m0_row_of`, voir « Plans verticaux » plus bas) :
`nes_ppu_set_rows(ppu, n)` en fait une table de 240 bits (la ligne y ferme
sa rangée) pour n de 120 à 240 ; 128 sur META, 176 sur Pokitto, où
`pokitto_lcd_row_zoom` élargit les rangées à 200 (200×176, comme les cœurs
Sega). Raccourcis exacts : un groupe d'une seule couleur, un groupe égal
sur les deux lignes ; les groupes d'une seule couleur qui se suivent avec
le même mot (le ciel) ne sont plus que des comparaisons de mots et des
écritures de mots, sans table. Les boucles sont en Thumb-1 (`ppu_asm.S`,
en flash : le code en SRAM a fauté sur META surcadencé), la version C de
`ppu.c` leur sert de référence. Une image que le cadenceur saute ne rend
rien : seules les lignes où le sprite 0 peut toucher calculent leur fond.

Le harness compile aussi l'ancien rendu 256 pixels (`NES_PPU_REFERENCE`) et
compare chaque rangée du panneau au filtre de ses lignes : `out_mismatch`
(rangées différentes) et `hit_mismatch` (lignes dont le sprite 0 diffère)
doivent valoir 0, aussi avec `--skip n` (une image sur n non affichée) et
`--rows n` (176 : le plan du Pokitto ; vérifié aussi en 120, 144 et 240).
L'asm se compare au C sur l'estimateur (`--cflag=-DBENCH_PIXEL_HASH`, puis
`--cflag=-DNES_NO_PPU_ASM`) ; le banc rend aussi le son comme le pilote
META (22049 Hz).

Mesures du 25 octobre 2026 (SMB, profil des sessions enregistrées) :

| | avant | après |
| --- | --- | --- |
| estimateur, 48 MHz, 1 attente, sans son (cycles/image) | 1,74 M (PPU 1,40 M) | 0,83 M (PPU 0,61 M) |
| estimateur, 70 MHz, 2 attentes, son compris | — | 1,00 M (PPU 0,65 M, APU 0,09 M) |
| gm0, 1-1 en courant (images de jeu/s, temps estimé) | 20,6 | 60 (≈11 % d'attente) |
| META réel, 1-1 en courant (compteur du jeu lu par USB) | — | 59,9 |

Les chiffres de l'estimateur sont des estimations ; seule la dernière ligne
est une mesure sur console (vitesse du jeu, pas de profil). Le META tourne
par défaut à 70 MHz (`META_OVERCLOCK`).

Le son : le pilote META joue à 22049 Hz mais le runtime rendait toujours à
11025 Hz (son deux fois trop rapide) ; les pulses avançaient d'un pas par
`timer + 1` cycles CPU au lieu de `2 × (timer + 1)` (une octave trop haut).
Le LFSR du bruit saute jusqu'à 14 pas d'un coup, la table de réciproques de
`timer_run` arrondit enfin au supérieur (elle débordait en non signé et la
boucle de correction rattrapait le quotient, jusqu'à ~16 000 tours), et les
voix sont rendues l'une après l'autre par blocs, une voix muette n'avançant
que de marque en marque du séquenceur.

Pokitto (25 octobre 2026, SMB, même profil) : même runtime, plan 176, les
rangées poussées par `pokitto_push200` (Thumb-1 en SRAM, comme le Game
Gear), son par `pokitto_audio_asm.S`. Manette du frontend Game Boy : C tapé
= Start, C + B = Select, C tenu = menu. Firmware de 229 Kio sous le loader
(0x39000), 14,6 Kio de `.bss`. Estimateur (modèle META, 48 MHz, son
compris) : 1,01 M cycles/image contre 0,96 M en 128 rangées, plus environ
0,3 M par image affichée pour les 200×176 pixels (compté à la main, ~9
cycles par pixel) ; à 48 comme à 72 MHz (`POKITTO_HC`), le cadenceur saute
donc des images. Dans gm0, la session META enregistrée (`--play-guest`)
atteint 2000 images de jeu en 33,8 s émulées, 59 images de jeu/s, aux deux
fréquences ; images affichées non comptées, pas encore essayé sur console.

Suite (25 octobre 2026, mêmes entrées, estimations) : `m0_estimate.py
--target pokitto` compte désormais aussi l'envoi des pixels d'un projet NES
Pokitto (son propre `pokitto_lcd.c`), avec 2 attentes de flash à 48 MHz ;
il donne 36,4 images/s toutes affichées, gm0 37 (`NES_MAX_SKIP=0`).

| | cycles/image |
| --- | --- |
| META 70 MHz, 2 attentes : avant / groupes uniformes en série | 1,045 M / 0,992 M |
| META, et `JMP` sur soi-même consommé d'un coup | 0,912 M |
| Pokitto 48 MHz, boucles PPU en flash | 1,356 M |
| Pokitto, boucles PPU en SRAM (`NES_PPU_RAMFUNC`, 2,6 Kio) | 1,318 M |
| Pokitto, et `pokitto_push200` qui saute les groupes déjà sur le bus | 1,292 M (envoi 0,330 → 0,304 M) |
| Pokitto, et `JMP` sur soi-même | 1,213 M (39,6 images/s toutes affichées) |

La boucle d'attente du NMI (`JMP` vers sa propre adresse, `$8057` dans SMB)
tournait jusqu'au bout de chaque tranche de ligne : le code recompilé et le
répartiteur (`6502.c`) la consomment d'un coup, ⌈b/3⌉ sauts par une
multiplication (`NES_SPIN3`, `nes_gen.h`). Mêmes cycles : rien de ce qui
peut couper une tranche (NMI, IRQ, vol de cycles du DMA) ne change au cours
d'une tranche ; SMB précis = interpréteur (run_hash 7c236ae4, 1500 images
avec entrées).

gm0 ne modélise pas les attentes de flash du LPC11U68 : il ne voit pas la
différence entre boucles en SRAM et en flash (40,3 s contre 40,2 s pour 1500
images). `pokitto_push200` est partagé : écrans fixes identiques à l'octet
dans gm0 pour NES, Game Boy et Game Gear (un fondu diffère à mi-image, le
point d'arrêt de gm0 tombant ailleurs dans l'image).

### Plans verticaux

Tous les cœurs qui réduisent ou étirent leurs lignes suivent la même règle
(`m0_scale.h`) : n lignes sur m rangées, la ligne y sur la rangée
`m0_row_of(y, n, m)` = ⌊y·m/n⌋ (réduction, une rangée prend une ou deux
lignes), ou sur les rangées de `m0_row_first(y, n, m)` = ⌈y·m/n⌉ à celle de
y + 1 (agrandissement). SG-1000 (192 → 120, 192 → 150), SMS (192 → 120,
120 → 150), Game Gear (144 → 128, 144 → 176, en tables en flash vérifiées
par `test_gg_runtime.c`), NES (240 → n). L'envoi des rangées est commun aussi
(`targets/common/platform/m0_rows.h`) : deux tampons en alternance, l'un
envoyé par DMA sur META pendant que l'autre se remplit, et sur Pokitto la
rangée poussée sur chaque rangée d'écran que le plan lui donne
(`m0_rows_zoom`). SG-1000, Game Gear, PICO-8 et NES s'en servent ; captures
gm0 identiques à l'octet sur META et Pokitto pour les trois premiers.

### Communs aux cœurs : boucles d'attente et écritures Pokitto

- Boucle d'attente (`targets/common/runtime/m0_idle.h`) : un saut sur
  lui-même (`JMP` du 6502, `JR $` / `JP $` du Z80) tourne jusqu'au prochain
  événement ; `m0_idle_loops` donne d'un coup le nombre de tours qu'il
  aurait faits, ⌈reste / durée⌉, par une multiplication (exact, vérifié de
  1 à 2 millions de cycles pour toutes les durées de 2 à 30). Le NES s'en
  sert (`NES_SPIN3`), le Z80 aussi : l'émetteur SG-1000/GG/SMS/MSX écrit
  `SG_IDLE` (`sg_gen.h`) avant un tel saut, et la boucle s'arrête sur le
  même `SYNC` qu'en tournant (précis = interpréteur, tests e2e). `HALT`
  avait déjà le sien (`sg_halt`).
- Écritures de pixels du Pokitto (`pokitto_lcd_asm.S`, en SRAM) : une seule
  macro `PUSH4` pour `pokitto_push200` (élargi), `pokitto_push_n` (n
  pixels) et `pokitto_push160` (son entrée) ; quatre pixels déjà sur le bus
  ne sont que des impulsions WR. Le PICO-8 (`pokitto_lcd_row128`, une
  boucle C en flash jusqu'ici), le SG-1000 et l'MSX (jusqu'ici en C eux
  aussi) passent par elles ; le SG-1000 et l'MSX prennent aussi les bancs
  de 2 Kio du Pokitto (la file audio dans RAM1), sans quoi Gradius 2
  n'avait plus de place pour la pile. Écrans fixes identiques à l'octet
  dans gm0 pour Game Boy (1:1, `push160`), NES et Game Gear (`push200`),
  PICO-8 (`push_n`), SG-1000 et MSX ; les images en mouvement diffèrent là
  où la vitesse change l'image affichée à un instant donné.

## Vérification

Le harness généré (`host/`) accepte `--interp`, `--frames`, `--coverage`,
`--buttons`, `--start-frame`, `--pause-frame`, `--shot` et `--wav` (PCM mono
8 bits, 22050 Hz). Les bits de `--buttons` sont Haut, Bas, Gauche, Droite,
bouton 1 et bouton 2 (bits 0 à 5) ; `--start-frame` tient le bouton 1 pendant
dix images. Le `run_hash` couvre registres, cycles, RAM, VRAM, état VDP/PSG,
RAM de cartouche et image ; chaque ligne du rendu des consoles est comparée
au rendu de référence (`out_mismatch`).

Chaque cœur à couverture jouée (gameboy, sg1000, msx, gamegear, nes,
vectrex, megadrive, pce) propose aussi `--explore FRAMES --random SEED` :
des segments d'entrées aléatoires, un instantané de la machine entre
segments, et des segments qui s'arrêtent dès qu'ils découvrent une adresse
interprétée nouvelle — l'explorateur privilégie les reprises qui ouvrent du
code jamais vu. L'explorateur est partagé
(`targets/common/host/m0_explore.h`, branché par
`targets/common/host/m0_explore_host.h` : analyse des arguments, traduction
des touches, boucle à segments) ; un cœur nouveau le raccorde en quelques
lignes. Le Game Boy garde son propre câblage : ses instantanés ne se posent
qu'aux points sûrs du runtime (longjmp). Le SG-1000 joue chaque segment
entier sans reprise — `sg1000_t` doit rester identique à l'octet pour la
chaîne firmware/référence — et compte la nouveauté en scannant les adresses
touchées à la fin des segments. `tools/profile/m0search.py` enchaîne une
session explore, une partie sans touche (`m0_idle.h` : logos, titres,
démos) et les parties scriptées, puis fusionne la couverture pour le
générateur ; seul le PICO-8 reste entièrement scripté (pas de couverture à
consommer).

Cinq drapeaux étendent la marche statique aux cibles d'appel et de saut
que l'atteignabilité ne prouve pas (gestionnaires derrière une table de
sauts, appelants derrière une table de bancs) : `--pce-sweep` (HuC6280),
`--nes-sweep` (6502), `--vectrex-sweep` (6809), `--megadrive-sweep`
(68000) et `--meta-sweep` (SM83, cible console). Chacun balaye la ROM
pour les encodages d'appel/saut absolu de son processeur et sème les
cibles comme entrées de région. La discipline est partagée
(`recompiler/include/m0boy/sweep.h`) : la marche de la couverture n'a
jamais de plafond ; le balayage et son expansion partagent un budget —
sur le PCE, le NES et le Vectrex, ce qui reste du budget code
(`--meta-code-budget`), 16384 instructions sinon ; sur le Mega Drive et
le Game Boy, 16384 instructions. Rien ne s'exécute tant que le jeu n'y
va pas : une entrée n'est prise que sous son propre contexte (octets MPR
du PCE, banque vérifiée du Game Boy, PC ailleurs) et la sémantique
compilée est celle de l'interpréteur. Le code généré grossit, le
`run_hash` ne doit pas bouger — chaque e2e (PCE, NES, Vectrex, Mega
Drive, cible META) vérifie les deux.

`--meta-precise` synchronise avant chaque instruction et calcule tous les
drapeaux : le code recompilé doit alors donner exactement le `run_hash` de
l’interpréteur. Le 13 octobre 2026, c’était le cas pour les 68 cartouches
d’une collection SG-1000 (900 images, pause à l’image 500), et le rendu
160 pixels égalait le rendu de référence sur toutes les lignes. Les tests
CTest le vérifient sur un programme synthétique (repli sur un budget presque
vide compris) ; le noyau Z80 passe ZEXDOC et 66 tests ZEXALL sur 67 (seul
`BIT n,(HL)` diffère : ses bits X/Y viennent de MEMPTR, non modélisé).

Le build normal (synchronisation aux têtes de boucle, drapeaux utiles
seulement) peut placer une interruption quelques instructions plus tard ; il
n’est donc comparé qu’à l’exécution (images, son, absence de faute).

Chaque projet possède un seul système de destination. Pour META, la
génération produit `platformio.ini` : `pio run -d output/jeu-meta`. Pour
Pokitto, `cmake -G Ninja -S output/jeu-pokitto -B output/jeu-pokitto/build
-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake` puis Ninja, avec le checksum de
vecteurs LPC vérifié après liaison ; `-DSG_COMPILER=clang` compile le C/C++ avec
Clang (`toolchain/arm_clang.py`). `compile.sh` fait tout cela, avec
`FIRMWARE_COMPILER=gcc|clang`. L’image garde le rapport 4:3 : 160×120 au
centre du META, 200×150 au centre du Pokitto. MENU/C fournit l’impulsion NMI
de la touche Pause.

## Vitesse

Mesurée dans gm0 (émulation Cortex-M0+ au cycle, attentes flash et DMA LCD
comprises) : images SG-1000 produites pendant 30,1 s émulées, 59,92 images/s
étant la vitesse NTSC. Ce sont des mesures d’émulateur, pas des mesures sur
console.

Le 13 octobre 2026, sur les 68 cartouches d'une collection SG-1000, firmwares
META construits par `compile.sh` (GCC 7.2, profil automatique, rendu filtré,
1800 trames avec des appuis scriptés) :

| Console | Au plafond | Autres |
| --- | --- | --- |
| META (48 MHz) | 67 sur 68 à 98,8 % | H.E.R.O. 98,0 % |
| Pokitto (48 MHz) | 67 sur 68 à 99,0 % | H.E.R.O. 98,3 % |

Le plafond n'est pas 100 % : la mesure compte le démarrage du firmware, avant
la première trame SG-1000. H.E.R.O. ne perd que pendant ses 150 premières
images, puis tient la vitesse NTSC. Avant l'assembleur et ces profils (même
jour), la moyenne META était de 96,9 %, Space Slalom à 78 % ; avant la
recompilation, l'émulation du Z80 atteignait environ 32 % (Flicky, Wonder
Boy).

Le profilage de `compile.sh` joue une partie bouton 1 maintenu, une partie
sans touche (logos, titre, démo) et huit parties aléatoires. L'interpréteur y
note aussi les cibles des `RET`, `RETI`/`RETN` et `JP (HL)` (hors retours
d'interruption) en lignes `# entry XXXX` : le recompilateur en fait des
entrées de région (adresses de retour empilées, tables de sauts), sans quoi
ce code repasserait par l'interpréteur. `AUTO_REDO=1` refait les profils.

Le cœur Z80 gagne ses cycles à trois endroits (2026-10-15).  Les drapeaux
S/Z/X/Y/PV viennent d'une table de 256 octets (`z80.c`) au lieu de la chaîne
XOR de parité, dans l'interpréteur comme dans les fonctions générées.
L'interpréteur de repli court-circuite le décodeur pour les familles
courantes (`z80_fast.h`, partagé SG-1000/Game Gear : LD r,r', LD r,n, ALU,
INC/DEC, JR, DJNZ...) : le code interprété d'Aladdin exécute à 78 % le trio
LD A,(HL) / OR A / JR.  Enfin, les sites d'instructions chauds (dans un
facteur 64 du plus exécuté, compteurs v2) déploient `PUSH`/`POP`/`RD`/`WR`
en ligne au lieu des appels `sg_push`/`sg_pop`/`sg_rd`/`sg_wr`.  Mesuré par
attribution de cycles gm0 (`EMU_PROF`, 600 images, pas une mesure sur
appareil) : Galaga Pokitto, `sg_push`+`sg_pop` de 5,4 % à 3,0 % des cycles
et 0,9 point de marge de trame en plus ; Aladdin streamé, le chemin
interprété de 18 % à 14 % des cycles et 2,2 points de marge en plus, à
méthodologie de génération égale par ailleurs.

En retard, le firmware saute jusqu'à deux présentations de suite ; le Z80, le
PSG et l'état des sprites restent exacts. Clang 23 produit des
firmwares un peu plus petits et de 1 à 5 % plus lents que GCC 7.2 sur ces jeux.

## Game Gear

Les cartouches `.gg` passent par le même recompilateur Z80 et la même
organisation de runtime que le SG-1000 (événements par trame, lignes
dessinées à la demande, PSG rattrapé avant chaque écriture, interpréteur de
repli), avec ce qui distingue la Game Gear :

- **Mapper Sega** : `0000–03FF` fixe, trois fenêtres de 16 Kio paginées par
  `FFFD–FFFF`, RAM de cartouche (fenêtre de 16 Kio en `8000–BFFF`, deux
  banques par le bit 2 de `FFFC`). **Mapper Codemasters** (C.J. Elephant
  Fugitive, Cosmic Spacehead, Micro Machines 1 et 2) : pages écrites en
  `0000`, `4000` et `8000`, sans Kio fixe ; détecté par la somme de contrôle
  Codemasters en `7FE6`.
- **Recompilation par banques** : une instruction est identifiée par sa page
  et son adresse (`page << 16 | pc`). Une région ne couvre qu'une fenêtre et
  une page ; un saut vers la même fenêtre garde la page, un saut vers une
  autre fenêtre suppose la page la plus jouée de celle-ci et la vérifie
  (`machine.page[s] == p`) avant l'appel direct, sinon le répartiteur cherche
  la région de la page courante. Après un appel, le code d'une fenêtre
  paginée vérifie aussi que sa page n'a pas changé. Une écriture constante
  dans le registre de sa propre fenêtre rend la main au répartiteur ; tout
  autre changement de page (copie de bloc sur les registres, gestionnaire
  d'interruption) force un événement, et la synchronisation suivante revient
  au répartiteur. Le Kio fixe et les lectures de ROM constantes y sont
  repliées à la génération ; les autres passent par des pointeurs de
  fenêtre. Le code exécuté en RAM, ou dans la RAM de cartouche, reste
  interprété.
- **VDP mode 4** : tuiles de 4 plans, 512 motifs, retournements, palette de
  sprites, priorité, défilement horizontal et vertical (verrouillages des
  deux lignes du haut et des huit colonnes de droite), 64 sprites (8 par
  ligne, 8×16 et zoom, débordement et collision), CRAM de 32 couleurs sur
  12 bits, compteur de lignes (interruption de ligne en fin de ligne, avant
  celle de trame), compteurs V et H. Seule la fenêtre de 160×144 (colonnes
  48–207, lignes 24–167) est dessinée, au pixel près : pas de filtre
  horizontal. Un octet de plan donne, par une table, ses huit pixels en
  champs de 4 bits ; la boucle des tuiles est en Thumb-1 exécutée en SRAM
  (`gg_render_asm.S`, contrôlée contre le C par un firmware `GG_SELFTEST`).
  L'interruption de trame est levée à la ligne 193 (compteur V `C1`) : des
  jeux attendent `C0` avant elle.
- **PSG Sega** : bruit sur 16 bits (prises 0 et 3), période 0 ou 1 tenue
  haute (lecture d'échantillons par le volume). Le stéréo de la Game Gear
  (port 6) est ignoré : sortie mono.
- **Ports Game Gear** : START et région au port 0 ; port série et parallèle
  (1–5) mémorisés comme sur la console sans partenaire.

Sur META (160×128), deux lignes sur neuf sont moyennées : 144 → 128.
Sur Pokitto (220×176), l'image est agrandie en 200×176 (5:4 en largeur,
deux lignes sur neuf répétées en hauteur).

### ROM et RAM de cartouche sur la carte SD

La META a 240 Kio de flash et 32 Kio de RAM, dont 25 Kio pour la console
d'origine (16 Kio de VRAM, 8 Kio de RAM). Une ROM qui ne tient pas en flash
avec son code recompilé garde en flash la page 0 et les pages les plus lues
comme données au profilage (lignes `# read secteur images` de la
couverture) ; les autres sont lues dans le fichier ROM copié sur la carte
(`NOM.GG`, à côté du `.bin`) par un cache de deux secteurs de 512 octets.
Le code recompilé n'a pas besoin de ses propres octets : seules les données
et le code interprété lisent la carte. La RAM de cartouche (jeux à
sauvegarde) vit de même dans `NOM.SAV` (32 Kio, créé une fois par
`compile.sh`, jamais écrasé), par le même cache, réécrit quand le jeu
désactive la RAM et deux secondes après la dernière écriture. Erreurs au
démarrage : écran rouge (pas de carte), magenta (fichier ROM absent), jaune
(fichier fragmenté : le recopier), bleu (autre ROM).

`compile.sh` construit deux fois chaque firmware : la première mesure la
taille du code généré par instruction (`sg_gen.o`), la seconde remplit la
flash avec cette mesure (pages résidentes et budget de code compris). Le
profilage Game Gear joue trois fois plus longtemps que celui du SG-1000
(3 600 images par partie).

### Vérification et vitesse

Avec `--meta-precise`, le code recompilé reproduit exactement
l'interpréteur (registres, mémoire, mapper, VDP, CRAM, PSG, image) sur les
225 cartouches d'une collection Game Gear, et sur les trois cartouches
Master System essayées. `tests/test_gg_e2e.py` le vérifie sur une
cartouche synthétique en banques (`tests/gg_fixture.py`, assemblée par
`tests/z80asm.py`), firmwares compris.

Vitesse mesurée dans gm0 (pas sur console), à 48 MHz, sur les jeux essayés :
Columns, Pac-Man, GG Aleste, Fantasy Zone tournent à pleine vitesse sur
META comme sur Pokitto (98 à 99 %, le plafond de la mesure comptant le
démarrage) ; Aladdin, lu en grande partie sur la carte, atteint 94 % sur
META et 98 % sur Pokitto. Les 225 firmwares META et Pokitto se
construisent ; ils n'ont pas tous été mesurés.

## Master System

Les cartouches `.sms` (`--system sms`) utilisent le recompilateur, le mapper
(Sega ou Codemasters), la lecture sur carte et le PSG de la Game Gear ; le
runtime est commun (`GG_SMS`). Ce qui change :

- toute l'image de 256×192 est montrée, réduite à 160 pixels par groupes de
  8 → 5, comme le fait le VDP de la Game Gear en mode SMS (l'adaptateur
  Master Gear est passif) ; une ligne sur 8/5 est dessinée : 120 rangées sur
  META, les mêmes agrandies en 200×150 sur Pokitto ;
- CRAM de 32 octets (6 bits par couleur), colonne de gauche masquable ;
- ports de manette et registre d'E/S (lignes TH relues : machine export),
  PAUSE sur la NMI (MENU sur META, C sur Pokitto), pas de ports Game Gear ;
- l'en-tête de copieur de 512 octets de certains dumps est retiré.

Réduction horizontale : chaque groupe de 8 pixels donne 5 mélanges de deux
voisins, en huitièmes (5,3) (3,5) (4,4) (5,3) (3,5). Pour le fond, deux
pixels voisins de 4 bits forment un index de 8 bits dans une table de
mélanges par palette (1 Kio, refaite quand la CRAM change) : quatre
lectures de table et une moyenne par groupe, en Thumb-1 (`gg_sms_bg`,
`gg_render_asm.S`, contrôlé contre le C par `GG_SELFTEST`). Les groupes sous
sprites, à cheval sur deux palettes ou masqués passent par les indices de
couleur, avec les mêmes poids : les deux chemins donnent les mêmes pixels.

Vitesse (gm0, 48 MHz) : Alex Kidd in Miracle World, Shinobi et Wonder Boy
III tournent à 97–99 % sur META et 98–99 % sur Pokitto.


## Limites

- SG-1000 : cartouches sans mapper, au plus 48 Kio ; RAM de cartouche
  limitée au cas 8 Kio en `8000–BFFF`. Pas de SC-3000 (clavier), ni
  d’extension.
- Game Gear : pas de mode Master System (cartouches SMS jouées sur Game
  Gear), pas de câble de liaison, pas d'EEPROM série 93C46 (World Series
  Baseball, World Series Baseball '95 restent sur un écran noir) ; son mono ;
  ROM de 4 Mio au plus.
- Master System : pas de son FM (YM2413), pas de mode 224 lignes, pas de
  pistolet ni de second joueur ; pas de mélange vertical (une ligne sur
  8/5).
- Baseline NTSC : 262 lignes de 228 T-states. PAL n’est pas pris en charge.
- TMS9918A : rendu par ligne, pas au pixel près au moment du bus VRAM ; les
  combinaisons de modes non documentées ne sont pas validées.
- SN76489 : trois tons, bruit TI, atténuation 2 dB, sortie 8 bits à environ
  22 kHz, sans filtre analogique ni suréchantillonnage.
- Effets non documentés dépendant de MEMPTR/Q non modélisés ; pas de wait
  states de bus.

Les spécifications utilisées sont le
[manuel Zilog](https://www.zilog.com/docs/z80/z80cpu_um.pdf), les observations
[Z80 de Sean Young](https://www.smspower.org/uploads/Development/z80-documented.pdf),
le [manuel TI TMS9918A](https://www.smspower.org/uploads/Development/TMS9918.pdf)
et la [carte des ports SG-1000](https://www.smspower.org/Development/IOPortMapSG).

## Jouer sur PC

Le frontal commun `gm0`, sur sa [branche `z80`](https://github.com/r043v/gm0/tree/z80), charge directement les cartouches `.sg` dans le noyau
Z80/TMS9918A/PSG, sans seconde émulation Cortex-M0 sur PC. Il partage les
filtres F8, l’échelle F10, le plein écran F11, les manettes, F5, le titre et le
build WebAssembly avec META/Pokitto. Le petit lecteur `sg1000_play` reste un
harness SDL de référence pour les tests du noyau :

```bash
../gm0/build/bin/gm0 jeu.sg
```

Flèches : déplacement ; Espace : bouton 1 (A), Ctrl : bouton 2 (B).
Entrée : Start (bouton 1, utilisé pour démarrer Galaga) ; `*` : Select (Pause SG-1000,
également avec le `*` du pavé numérique) ;
Échap : quitter en natif. L’image est redimensionnable et le PSG produit ses
échantillons à 22050 Hz, rééchantillonnés par la sortie commune à 48000 Hz.
La cadence suit les 59736 T-states d’une trame NTSC à 3,579545 MHz.
`EMU_NOPACE=1` permet un essai sans attente ; `--frames N`, `--shot`, `--wav`
et `EMU_INPUT` sont disponibles pour les vérifications automatiques.

Le frontend des firmwares évite les divisions logicielles pour redimensionner
l’image, ne calcule le fond que pour les lignes réellement affichées, et saute
au plus une présentation consécutive lorsqu’il est en retard. Le Z80, le PSG
et les collisions de sprites continuent ; ce saut d’affichage ne garantit pas
la pleine vitesse du jeu sur une console physique. META utilise deux tampons
de lignes et le DMA LCD pour recouvrir le calcul et le transfert.

Le titre indique « actuel » (cadence émulée / temps mural, attente et affichage
inclus) et « possible » (cadence émulée / temps passé dans le noyau et le son,
hors présentation et synchronisation). 100 % représente la vitesse NTSC normale ;
la capacité possible n’est pas plafonnée, en natif comme dans le HUD WebAssembly.

Les distributions PC sont générées par `tools/sg1000/package.py` avec CMake et
Ninja, avec les sources de la branche `z80` de gm0 (`git switch z80` dans
le dépôt gm0). La branche `main` de gm0 exécute les firmwares META/Pokitto ;
la lecture directe des `.sg` est réservée à `z80`. Elles contiennent le lecteur, SDL2, les licences, une notice et un
manifeste SHA-256, sans cartouche. Linux fournit `sg1000-play` et `play.sh` ;
Windows fournit `sg1000_play.exe` et `SDL2.dll`. Sans argument, le lecteur
attend une cartouche déposée dans sa fenêtre.

```bash
python3 tools/sg1000/package.py \
  --frontend-root /chemin/gm0 \
  --sdl-linux-prefix /chemin/SDL2-linux \
  --sdl-mingw-root /chemin/SDL2/x86_64-w64-mingw32 \
  --mingw-root /chemin/llvm-mingw \
  --zlib-linux-prefix /chemin/zlib-linux \
  --zlib-mingw-prefix /chemin/zlib-mingw \
  --zlib-license /chemin/zlib/LICENSE \
  --linux-sysroot /chemin/sysroot-ubuntu22
```

Le sysroot Linux est facultatif. SDL2 doit aussi être construit contre la libc
choisie ; le manifeste et la notice indiquent la version minimale requise.
Les archives sont placées dans `output/sg1000-release/`.
L’option `--rom /chemin/jeu.sg --name galaga` intègre la cartouche fournie dans
les exécutables, sans l’ajouter aux sources suivies. Dans ce mode, le `.exe`
Windows inclut aussi SDL2 statiquement et démarre le jeu sans argument ni DLL
SDL2 externe. La distribution générique reste dépourvue de cartouche.
Le build web est celui de `gm0` : `emcmake cmake -G Ninja -S /chemin/gm0
-B build-wasm -DGM0_WEB_GAME=/chemin/Galaga.sg`, puis `cmake --build build-wasm`.
Il produit un seul `gm0.html` contenant le module et le jeu, sans réseau requis.

`./compile.sh` détecte par défaut les jeux de `roms/gb/` et `roms/sg/`. Un dossier
explicite accepte également des `.gb`, `.gbc` et `.sg`, ou les sous-dossiers
`gb/` et `sg/`. `TARGET=both ./compile.sh roms/sg` construit pour les deux
consoles. Les projets SG sont dans `out/sg1000/<cible>/<jeu>/`, les couvertures
sont conservées dans `<source>/profiles/<sha1>/sg1000/` et les jeux pour la carte
gardent le même inventaire, bilan et contrôle de version que les jeux GB.
Le profil SG automatique joue `AUTO_SECONDS` secondes émulées en tenant le
bouton 1 ; `SG_AUTO_FRAMES` permet de régler ce nombre d’images. `PLAY=1` ouvre
le lecteur natif. Le frontend firmware SG utilise GCC ; les options PACK,
les états et les sauvegardes propres à la Game Boy ne lui sont pas appliqués.
