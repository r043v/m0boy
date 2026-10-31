# PC Engine (`--system pce`)

`--system pce` (extension `.pce`) génère deux formes de projets : le
harness hôte (`--target desktop`, images PPM et WAV hors écran) et la
**fenêtre PC** (`--target pc`) — un shell SDL2 (`targets/pce/platform/
pce_pc.c`) : fenêtre ×3 redimensionnable, clavier (flèches, X = I,
Z = II, Entrée = RUN, Maj = SELECT, Échap quitte), manette SDL2 (croix ou
stick gauche, B = I, A = II, Start = RUN, Back = SELECT) ou joystick générique,
le son du PSG en file
SDL, `--frames`/`--ppm` pour une passe hors écran. La HuCard est
embarquée dans l'exécutable. `tools/pce/build_pc_binaries.sh` construit
le binaire Linux et un **.exe Windows** (MinGW croisé, `SDL2.dll` à
côté), sur le modèle du Mega Drive : les sessions de jeu de
`tools/profile/m0search.py` (la graine écran titre puis RUN, des
sessions aléatoires, les rejeux manuels passés avec `EXTRA_INPUT`)
tournent sur le harnais et stockent leur couverture HuC6280 fusionnée
dans `search/` — le profil que le recompilateur HuC6280 → C attend
(voir plus bas). Le shell et le harness impriment le
même `run_hash` (la recette du fingerprint est la même), et le .exe
produit l'image du binaire Linux **octet pour octet** — le cœur est
déterministe d'une plateforme à l'autre.

Le frontend SDL commun à toutes les cibles `--target pc` montre l'image sans
filtre au démarrage. F8 parcourt pixel, DMG, LCD RGB, scanlines et grille ;
F10 change l'échelle et F11 bascule en plein écran. Les manettes sont
détectées au démarrage et lors de leur branchement. Les captures `--ppm` et
`run_hash` restent calculés sur l'image du cœur, indépendamment du filtre.

```bash
tools/pce/build_pc_binaries.sh                       # raiden-linux + raiden.exe + SDL2.dll
build/bin/gbrecomp --system pce "Raiden (USA).pce" --target pc -o out/raiden_pc
cmake -G Ninja -S out/raiden_pc -B out/raiden_pc/build && ninja -C out/raiden_pc/build
out/raiden_pc/build/Raiden__USA__pc --frames 300 --ppm shot.ppm
# frames=300 pc=4e2f run_hash=14af7372  (le harness donne le même)
```

Le harness imprime le `run_hash` (CPU, WRAM, VRAM, SATB, palette, frame),
deterministe — la même loi que les autres runtimes : un projet précis
devra l'imprimer aussi, et le e2e (`tests/test_pce_e2e.py`) tient un
cartouche synthétique dont il vérifie hash et pixels.

## La machine

- **HuC6280** (`huc6280.c`) : le jeu d'instructions complet, comptes de
  cycles et états de reset tirés du cœur mednafen (beetle-pce-libretro,
  `huc6280_ops.inc`), recoupés sur le stub de boot de Raiden lui-même.
  Les opcodes 65C02 (BRA, STZ, TSB/TRB, (zp), BIT #, PHX/PHY…), les siens
  (TAM/TMA en masque de bits, BSR 8 bits — il pousse l'adresse de l'octet
  de displacement, le RTS +1 retombe après l'instruction : une erreur
  d'un octet ici faisait sauter l'instruction suivant chaque BSR et
  gelait le démarrage de Raiden —, ST0/1/2, RMB/SMB, BBR/BBS, TST, les
  cinq block moves TII/TDD/TIN/TIA/TAI à $73/$C3/$D3/$E3/$F3, SET,
  CLA/CLX/CLY, SXY/SAX/SAY, CSL/CSH), le mode T (ADC/AND/ORA/EOR sur
  zp[X]), le décimal, les vecteurs ($FFF6 BRK/IRQ2 — le BRK du 6280
  vecte bien par $FFF6, vérifié contre mednafen —, $FFF8 IRQ1=VDC,
  $FFFA timer). Les opcodes illégaux sont des NOP d'un octet et deux
  cycles — aucun jeu du périmètre n'en dépend. CSH/CSL posent le drapeau
  de vitesse sans changer le décompte (le premier jeu du périmètre passe
  en CSH tôt).
- **Mémoire** : les huit registres MPR fenêtrent l'espace 2 Mio par 8 Kio ;
  TAM bit 0 = MPR0. La WRAM de 8 Kio est le page $F8 **et ses miroirs
  $F9-$FB** (le hardware les décode sur quatre pages — Raiden mappe $FB).
  La page $FF est la page matérielle ($0000-$1FFF au boot, MPR0=$FF) ;
  le reste est bus ouvert ($FF). Au reset : MPR7=$00 (les vecteurs dans la
  page 0 de la cartouche — vérifié sur Raiden : RESET=$E000 y pointe), P=I,
  tous les IRQ sources armées.
- **VDC (HuC6270)** (`vdc.c`) : 64 Kio de VRAM, les 18 registres, le port
  VRAM (MAWR/MARR avec auto-incrément de CR bits 11-12), l'écriture des
  registres **par octet** (le LSB s'applique à l'octet bas, le MSB à
  l'octet haut — le port VRAM attend lui le MSB, comme le cœur mednafen),
  le DMA VRAM→VRAM complet à l'écriture de LENR (SOUR/DESR avec les sens
  de DCR), la SATB DMA (DVSSR ou DCR bit 4, copiée à la première ligne
  après la zone active), le BAT (largeur 32/64/128, hauteur 32/64 par
  MWR), les caractères 4bpp, 16 sprites de ligne (overflow → IRQ si CR
  bit 1), la priorité sprite/fond et le bit de priorité, la collision du
  sprite 0. La machine verticale suit les phases VSW/VDS/VDW/VCR du cœur
  mednafen, un pas par ligne : latches BYR/BXR, RCRCount (comparé à
  RCR-0x40 pour l'IRQ raster), IRQ VBlank et SATB à la première ligne
  après VDW.
  La VRAM est un tableau de 32 768 mots de 16 bits. Les adresses de port
  et de DMA restent sur 16 bits : au-delà de $7FFF, les lectures donnent
  zéro et les écritures sont ignorées ; une copie SATB en fin de VRAM
  s'arrête à la limite. Le fond lit le BAT et les deux mots de motif une
  fois par tuile de huit pixels, y compris avec un décalage horizontal
  (la première tuile partielle rejette ses `BXR & 7` pixels de gauche —
  un décalage de nibbles dans le mauvais sens y affichait un pixel noir
  et la tuile décalée d'autant, visible en scrollant).
- **VCE** : la table de 512 couleurs (B bits 0-2, R 3-5, G 6-8) et sa
  LUT RGB565. Un pixel de fond dont le nibble de couleur vaut 0 montre
  l'entrée 0 (le fond) — la substitution se fait à la lecture du pixel
  (`(couleur & 0xF) ? pixel : 0`, comme la VCE de mednafen), pas dans la
  LUT : un jeu peut réécrire n'importe quelle entrée, l'entrée 0 comprise,
  après avoir posé sa palette (l'ancien aliasing écrit par FixPCache
  rendait fausses les entrées couleur 0 réécrites après coup — les zones
  transparentes de la carte sortaient dans la couleur de la palette au
  lieu du fond).
- **Timer** : un tick tous les 1024 cycles CPU, IRQ TIQ calée jusqu'à
  l'acquittement ($1402 en lecture ou $1403 en écriture), le rechargement
  au tick suivant. Les ticks tombent aux frontières de ligne (le jitter
  reste sous une ligne).
- **Pad** ($1000) : SEL choisit le nibble (0 : I/II/SELECT/RUN, 1 :
  HAUT/DROITE/BAS/GAUCHE — l'ordre des bits de mednafen, recoupé sur la
  routine de Raiden : son test `CMP #$0C` = SELECT+RUN dans le nibble
  SEL=0), bouton pressé = 0, bits 4-5 toujours à 1, bit 6 à 0 (une PC
  Engine — la sonde CD du menu HuCard de Raiden en dépend), bit 7 à 1
  (pas de CD).
- **PSG** (`psg.c`) : six canaux wavetable 32 échantillons, fréquence
  12 bits (période = 2×F horloge maître), volume 5 bits, mode DDA (le
  dernier échantillon écrit), bruit en LFSR 18 bits (le polynôme du chip
  n'est pas publié : l'approximation est assumée). Ni LFO ni balance.

## Ce qui n'est pas modélisé

Les états d'attente CPU sur accès VDC/VCE, le vol de cycles du DMA (un
DMA finit d'un coup, déterministe), le bit BSY du statut, la fenêtre de
fetch CG des sprites, le mode CG « one word », l'horloge point de la VCE
(la largeur vient de HDR), la BSRAM et le CD-ROM², le multitap. Un jeu qui
sonde BSY pendant un DMA verra le DMA déjà fini.

## Le harness

`targets/pce/host/pce_host.c`, le contrat des autres runtimes :
`--frames`, `--ppm` (la fenêtre active : largeur = (HDW+1)×8, hauteur =
VDW+1), `--wav`, `--input` (script « frame boutons », noms entiers `i
ii select run up down left right`, joints par `+`, tenus le temps de
leur seule trame ; lignes `#` en commentaire, taille libre),
`--coverage` (les adresses HuC6280 démarrées, sous
`PCE_TRACE` — le défini des projets hôte générés), `run_hash=` sur la
sortie. La ligne de lecture
du pad et la sonde `m0input` suivent le modèle commun (une session gm0
rejouera sur le harness quand l'application gm0 saura la lancer).

`--explore FRAMES --random GRAINE` (qui demande `--coverage`) joue la
cartouche comme l'explorateur des autres cœurs (`m0_explore.h`, copié
dans `src/` des projets générés) : des segments de 8 à 40 s avec une main
plausible (direction tenue, I et II tenus ou tapotés, RUN de temps en
temps), depuis la mise sous tension ou depuis un instantané de la
machine — la structure entière, les pointeurs du bus y compris, qui
visent toujours les mêmes adresses — quand un segment atteint des
adresses jamais vues. La nouveauté qui l'arrête est le compte d'adresses
différentes de la trace. Rien n'est lu de `--input` sur ce chemin, et le
son n'est pas rendu. Deux exécutions de même graine découvrent
exactement les mêmes adresses (vérifié par `tests/test_pce_e2e.py`).

La fenêtre PC (`pce_pc.c`) enregistre le même dialecte :
`--record-input fichier` écrit une ligne pour chaque trame où le jeu
reçoit des boutons (fermeture propre à la sortie, sur Échap comme sur
`--frames`) et `--input fichier` rejoue un script dans la fenêtre ; le
`run_hash` qu'elle imprime est celui du harnais sur la même entrée —
`tests/test_pc_target.py` rejoue une session enregistrée et compare.

## Ce qui a été vérifié

- **Raiden (USA)**, 768 Kio : boot complet (le stub du menu HuCard, la
  sonde CD, l'init des MPR, l'effacement RAM, la programmation VDC depuis
  la table du stub, la routine de palette **générée en WRAM** par le jeu
  — un TIA autogénéré vers le port VCE), IRQ VBlank + RCR, SATB DMA,
  écran titre exact : kanji 雷電, logo RAIDEN™, texte « FROM NEC / © 1990
  SEIBU KAIHATSU INC. / ALL RIGHTS RESERVED / © 1991 HUDSON SOFT ».
  RUN sur le titre lance la partie et la partie se joue : le vaisseau
  décolle du porte-avions, la stage 1 défile (océan, côte, forêt, chars),
  les escadrilles attaquent avec leurs tirs visibles, les entrées
  répondent (déplacements, tir). Le gel « avant de vraiment jouer »
  d'avant le correctif BSR est diagnostiqué et compris : la boucle objet
  du jeu (banque 8, $4900-$4912) sautait le PLA qui suit son `BSR`, un
  octet mort restait dans la pile, le RTS final partait dans la page
  matérielle et la tempête de BRK→RTI qui suit laissait le programme
  principal attendre le drapeau vblank $207E pour toujours.
  run_hash stable d'une exécution à l'autre (14af7372 à l'image 300,
  harness desktop = binaire PC).
- **Le budget CPU par ligne suit CSL/CSH** : Raiden démarre en CSL
  (1,79 MHz) et passe tôt en CSH (7,16 MHz) ; une ligne VDC fait 1365
  horloges maîtresses, soit 455 cycles CPU en CSH mais ~114 en CSL. Le
  cœur donnait 455 cycles quoi qu'il arrive : tout le boot CSL tournait
  4× trop vite, l'état calibré au démarrage divergeait, et RUN lançait
  une partie restée en mode scripté — plus aucun tir, aucun ennemi,
  aucune entrée prise en compte. Le budget compte maintenant des
  horloges maîtresses (1365 par ligne, divisées par 3 en CSH, 12 en
  CSL), comme le cœur mednafen ; l'attract suit de nouveau la vraie
  timeline (écran PUSH RUN vers l'image 300 des deux côtés) et RUN
  démarre une vraie partie.
- **`TST #imm,zp/abs` lisait son opérande une octave trop tôt** : les
  quatre formes (83/A3/93/B3) reprenaient `A_ZP()`/`A_ABS()`, qui
  cherchent l'opérande en `pc+1` — l'octet immédiat — au lieu de
  `pc+2`/`pc+2..3` (l'encodage est `83 imm zp`, comme le décode mednafen).
  Raiden teste `TST #$08,$8C` (battement de 8 trames de sa machine à
  états) : le cœur testait la case zéro-page $08, le saut de fin de
  démo partait à la première occasion, l'attract rebondissait au titre
  vers l'image 673 (écran noir de charge à la clé) et un RUN pendant la
  démo thus démarrait par le mauvais chemin — plus de prise en compte
  du pad, des bombes qui partent seules (le vaisseau suivait le script
  de démo). Corrigé contre le décode mednafen (mêmes cycles) : la démo
  d'attract joue désormais comme la référence (le drapeau démo $92 suit
  les mêmes transitions trame à trame, 940 → ~2100), et RUN sur l'écran
  titre (avant ~620 ; pendant la démo il ne se passe rien, côté
  référence comme côté m0boy) lance une vraie partie où le vaisseau
  obéit au pad — vérifié au score et à la capture (LEFT tenu : le
  vaisseau se déplace, l'écran diverge de la partie sans entrée).
- **Comparaison de référence** : le cœur mednafen (beetle-pce-libretro,
  reconstruit de ses sources en octobre 2026) piloté par un mini frontend
  libretro (`logs/pce_mednafen_ref.c` : un script d'entrées « trame
  boutons », la dernière trame en PPM, `retro_set_controller_port_device`
  — sans lui le pad de la référence reste mort, et la démo auto-jouée
  masquait tout écart d'entrée). Le titre colle image pour image, RUN y
  démarre une vraie partie des deux côtés, et les deux parties réelles
  montrent le même déroulé (lancement porte-avions, océan, côte, combat,
  score). La démo de l'attract joue désormais des deux côtés (même
  fenêtre temporelle) ; sur la même entrée alignée sur RUN au titre,
  les deux parties montrent la même section au même moment avec le même
  ennemi. Restent des écarts fins non tranchés : un décalage de scroll
  de quelques pixels (sur le titre comme en niveau, la référence est
  légèrement en avance) et le rythme exact de ce décalage.
- **Cartouche de bord** (`logs/pce_edge_cart.py`, octobre 2026) : BAT
  64×64 de tuiles distinctes, BXR=5 et BYR=$0A (décalages non alignés),
  un sprite par la SATB auto, VRAM remplie par paires $0002/$0003 avec
  select=2. Sur la même trame, m0boy et mednafen sortent **image
  identique** — c'est elle qui a tranché les deux bugs d'octobre (le
  décalage de nibbles de la première tuile partielle, la transparence du
  fond) et validé le pas de sprite (`RCRCount - 1`).  Attention en
  écrivant une telle cartouche : le BAT est à l'adresse fixe $0000, le
  port VRAM n'écrit que si select=2, et TII incrémente sa destination
  (inutilisable vers un port) — TIN n'écrit que les octets bas.
- **Ouvert** : à trame égale après le RUN du titre, la progression de la
  partie chez mednafen va plus vite que la m0boy (la référence atteint
  la rivière vers la trame 800, m0boy vers 1500) alors que le titre est
  image pour image — probablement la prise en compte du RUN (fenêtre de
  scrutation différente) à trancher avec une trace d'entrées.
- **e2e synthétique** (`tests/test_pce_e2e.py`) : cartouche générée en
  Python (vecteurs, TAM, VDC, VCE), deux exécutions même hash, pixels du
  fond et du caractère vérifiés ; la même cartouche en projet précis
  imprime le hash de l'interpréteur.

## Le recompilateur HuC6280 → C

Le modèle NES (`emitter.cpp` + `nes_gen.h` → `pce_gen.h`) : les
instructions atteintes sont groupées par adresse en régions, une fonction
C chacune, avec le budget signé de l'interpréteur, et l'interpréteur en
repli (`huc6280.c` sous `PCE_NATIVE`, `pce_native = 0` ou `--interp`).
La spécificité PCE est le mapping : une instance d'instruction est une
adresse **sous un contexte MPR** (les huit registres de page, empaquetés).
La marche statique part des vecteurs sous le contexte du reset (tous les
MPR à $00, la page 0 de la cartouche) et suit les paires `LDA #imm` /
`TAM #mask` vers les contextes que le boot mapped réellement ; un TAM
qu'elle ne peut pas prouver (A inconnu, une table de bancs) laisse ce
chemin à l'interpréteur. Le dispatcher n'entre dans une entrée que si les
huit octets MPR de la machine sont encore ceux du contexte — une fenêtre
remappée retombe dans l'interpréteur au lieu d'exécuter les mauvais
octets. Les lectures ROM dont le contexte fait des constantes sont
plouflées ; tout le reste passe par les macros de l'interpréteur
(`PCE_RD`/`PCE_WR` sur les copies locales de rbase/wbase), cycle pour
cycle : même compte (décimal +1, mode T +2, déplacements de blocs
17+6n), mêmes accès bus dans le même ordre, T rendu après chaque
instruction comme l'interpréteur (SET, RTI et PLP exceptés). Les sorties
d'une région retournent toutes au dispatcher (RTS, RTI, sauts par
pointeur, budget épuisé) ; les boucles `BRA`/`JMP` sur elles-mêmes sont
passées d'un coup (`m0_idle.h`, 4 cycles par tour). Le mode précis
(`--meta-precise`) vérifie le budget après chaque instruction et sort de
la région après toute instruction pouvant toucher le bus d'E/S ou le
contrôleur d'interruption, pour échantillonner la ligne IRQ exactement où
l'interpréteur le fait.

`--pce-sweep` ajoute à la marche un balayage préliminaire de la
cartouche : chaque fenêtre (les huit) mappant chaque page, un octet `$20`
(JSR) ou `$4C` (JMP abs) dont la cible se décode sous ce contexte sème
la marche. Le but : le code que l'atteignabilité ne prouve pas — les
gestionnaires de tables de sauts, les appelants derrière une table de
bancs — compilé comme le reste, pour les cartes où la couverture jouée
reste mince. Rien ne s'exécute tant que le jeu n'y va pas (une entrée
n'est prise que sous ses propres octets MPR, et la sémantique compilée
est celle de l'interpréteur), mais le `pce_gen.c` grossit : le balayage
est borné — ce qui reste du budget code (`--meta-code-budget`), 16384
instructions sinon — et ses entrées rangent après les adresses jouées.
La marche de la couverture, elle, n'a jamais de plafond. `tests/test_pce_e2e.py` vérifie sur la carte d'exercice que
le balayage compile plus d'instructions que la marche seule sans bouger
le `run_hash`. Le balayage est une famille générique — `--nes-sweep`,
`--vectrex-sweep`, `--megadrive-sweep`, `--meta-sweep` — avec la même
discipline (`recompiler/include/m0boy/sweep.h`) ; voir
`docs/MULTI_SYSTEM.fr.md`. Le harness PCE et les autres cœurs à
couverture partagent aussi l'explorateur `--explore`/`--random`
(`targets/common/host/m0_explore.h`, câblé par
`targets/common/host/m0_explore_host.h`).

Le coût de dispatch a été cherché au profil (Raiden, profil complet) :
le VDC prend ~43 % du temps (le coût fixe du runtime), et la mécanique de
dispatch pesait 24 % contre ~10 % au code compilé lui-même.  Trois
réductions sont passées : les octets MPR empaquetés vivent dans
`huc6280_t` (`mpr_key`, tenu par `huc6280_map` — le dispatcher ne recompte
plus huit décalages par dispatch), les fenêtres bus sont lues sur place
par un pointeur vers les tableaux du CPU (une région ne copie plus
16 pointeurs à l'entrée, et le TAM prouvé n'a plus de rebase à payer), et
le repli interprété court **la tranche entière** (son propre code, son
propre échantillonnage d'IRQ) au lieu d'une instruction par appel — le
dépassement de tranche qu'il laisse dans `cpu->cycles` est préservé.  Les
hooks de traçage ne comptent que sur `--coverage`.  Bilan : le natif
rejoint la vitesse de l'interpréteur sur le titre de Raiden (0.165 s contre
0.165 s pour 300 images, l'interpréteur seul) là où il était deux fois
plus lent, et le profilé complet reste l'item ouvert du paragraphe
précédent.  L'expérience suivante, non retenue : planifier les régions
**par contexte** (le tri (pc, contexte) du map entrelace les instances du
même span sous ses plusieurs contextes et fait éclater les régions en
~1 instruction, d'où un appel de fonction par instruction exécutée) —
5 153 régions au lieu de 58 702 et 0.122 s (25 % sous l'interpréteur),
mais le run_hash précis profilé de Raiden diverge, et la cause exacte
(échantillonnage d'IRQ à travers les enchaînements internes à une région
longue ?) n'est pas tranchée ; le code est revenu à l'ordre du map et
l'outillage (diff d'écritures WRAM horodatées, flux de dispatch, états
par dispatch) est en place pour la reprise.

Le rendu du fond lit maintenant BAT et plans 4 bpp par tuile au lieu de
les relire à chaque pixel. Sur le harness Raiden en partie, 3 000 images
avec le même script d'entrée et `--interp`, cinq mesures `perf stat`
passent de 1 617 à 1 451 ms de temps CPU (−10 %, sur cet hôte). Les
captures PPM du titre (300 images) et de la partie (3 000 images) sont
identiques avant et après. La correction du tableau VRAM de 128 à 64 Kio
change le `run_hash` parce que celui-ci hache toute la VRAM : à 300
images il devient `14af7372`, et à 3 000 images avec le script de partie
`76637455`. Le natif et l'interpréteur donnent le même nouveau hash.

Le passage de composition des sprites est sauté sur les lignes sans sprite
actif. Sur les autres lignes, son tampon temporaire et la fusion avec le
fond sont limités à la plage horizontale couverte par les sprites visibles.
Sur le même parcours Raiden et cinq mesures `perf stat`, le temps CPU passe
de 1 456 à 1 348 ms par rapport au rendu précédent (−7 % supplémentaires,
environ −17 % depuis le rendu par pixel initial). Le `run_hash` reste
`76637455` et la capture PPM à 3 000 images est identique.

La vivacité des drapeaux N/Z/C/V traverse les régions en mode rapide (le
modèle NES) : une marche arrière par région, tous les drapeaux vivants
aux étiquettes et aux sorties, et les mises à jour émises sous masque
immédiat — `PCE_ZN` tué quand N/Z sont morts, ADC/SBC/CMP/BIT/TST et les
shifts ne calculent plus que leurs drapeaux vivants.  Le T/I/D restent
hors du modèle.  Le mode rapide garde le `run_hash` de l'interpréteur
sur Raiden avec cette élagage.

La couverture du harness (`--coverage`) porte les contextes : le tracer
enregistre, par adresse, les octets MPR sous lesquels elle a tourné
(quatre au plus) et les atterrissages dynamiques (RTS, RTI, sauts par
pointeur, interruptions — drapeau `d`) ; la fusion de `m0search.py`
rassemble le tout par (adresse, contexte). Le générateur sème la marche
avec ces paires et classe les instances par compte d'exécutions — ce que
le jeu a prouvé joué sous un contexte que la marche statique ne voyait
pas se compile au tour suivant. Règle : un projet `--meta-precise`
imprime le `run_hash` de `--interp`, de la marche statique seule puis
avec le profil (`tests/test_pce_e2e.py`, cartouche d'exercice : TAM entre
pages, mode T, décimal, déplacements de blocs, BSR, BBR/BBS, TST,
routine copiée en WRAM, interruption timer avec RTI, opcode non
documenté).  **Raiden (USA)**, 768 Kio : le projet précis de la marche
statique seule imprime le hash de l'interpréteur (`14af7372` à l'image
300, la même valeur que l'interpréteur seul et que les binaires livrés),
et le profilé le garde jusqu'à ~1 300 des 9 096 adresses du profil de
jeu (le mode rapide est en plus ×30 sur 300 images du titre).  Le profil complet (~21 000 instructions jouées, ~71 000 instances sous
leurs contextes MPR) a fini par diverger pour quatre causes, toutes dans
le générateur et corrigées : les tables `entry_region`/`entry_slot` en
`uint16_t` tronquaient les index au-delà de 65 534 régions ou entrées ;
le masque de vivacité des flags passait aux macros dans son codage
compact (N=1, Z=2, C=4, V=8) au lieu des bits de P (BIT ne mettait plus
N ni V à jour, ADC/SBC plus V) ; l'adresse d'écriture de `STA (zp),Y`
(et des autres écritures `(zp),Y`) oubliait Y ; en mode précis, une
branche ou un saut pris vers une étiquette de la même région ne
contrôlait pas le budget (une instruction de trop au bord de la
tranche).  Avec un profil de sessions aléatoires et d'un idle long
(`m0search.py run --no-explore`, 12 s), le projet `--meta-precise` de
Raiden imprime le `run_hash` de `--interp` à 60, 300, 1 500 et
3 000 images (`9288e16e`, `66b29e2e`, `6ae87821`, `23a4547f`).  Le mode
rapide ne vérifie le budget qu'aux étiquettes : son hash diffère de
l'interpréteur par construction, l'image reste celle du jeu.
L'outil de la chasse : un journal par tranche (registres et hash de la
WRAM) puis par instruction sur la fenêtre de la première tranche qui
diverge, comparés entre `--interp` et le natif.

## La cible AKA (ESP32-S3)

`--target aka` ajoute la Gamebuino AKA : le frontend console
(`targets/pce/platform/pce_main.cpp`) planne la fenêtre active du VDC sur
120 rangées (le panneau les zoome ×2, `M0_PANEL_SRC_ROWS`), échantillonne
la largeur 256 -> 160 au filtre 5-in-8 (`m0_box58_fast` ; une autre
largeur : pixel le plus proche) et mélange les deux lignes sources d'une
même rangée (`m0_avg2_panel`) ; le menu d'options, le repli au launcher
(redémarrage) et la vitesse du pad suivent le contrat commun (`sg_menu.h`,
`sg_frontend.h`, `esp32_hw`).  Avec `PCE_RENDER_THREAD` (posé par le
générateur dans `pce_config.h`, motif du `MD_RENDER_THREAD` du Mega Drive),
l'émulation ne fait plus la chaîne pixels : son rappel de ligne pousse les
lignes brutes du VDC (indices de palette 9 bits, deux contextes de huit
lignes sous sémaphores) et la tâche `pce_rend` du cœur 0 applique la VCE,
le filtre 5-in-8, le mélange des paires et les soumissions LCD du chemin
wide asynchrone (quatre lots de huit rangées en vol).  Le harnais et la
fenêtre PC restent sur le `rowbuf` RGB565 (`machine.row_raw` à 0) ; le
chemin mono-cœur synchrone se reconstruit avec `-DAKA_EMU_SYNC=1` pour
aka_headless, qui ne termine jamais les DMA.  La HuCard voyage entière
dans la partition application (0x370000, rejet au-delà de 2,5 Mio — la
fenêtrage en flash des plus grandes suit) ; `pio run` construit le
firmware (Raiden : succès de la chaîne ESP-IDF/Arduino, aucune exécution
sur console physique).  Le pad : A/B = I/II, RUN ou D = Start, MENU ou
C = options, joystick complet.

## Chantiers suivants

### La chaîne pixels et les limites du SIMD S3 (30 octobre 2026)

Quatre changements à **sortie identique** (les hashes e2e et les PPM ne
bougent pas, `pce_end_to_end` le vérifie) :

- **LUT VCE finale** (`vce_final`, `pce.c`) : la transparence (indice de
  couleur 0 → entrée 0) est posée à l'écriture de la palette — l'entrée e
  garde sa couleur, les 32 entrées de couleur 0 portent celle de
  l'entrée 0, reconstruites quand l'entrée 0 est réécrite, comme le
  FixPCache de mednafen. Le chemin par pixel n'a plus de test, ni dans le
  `rowbuf` du harnais ni dans la tâche de rendu du cœur 0.
- **Cache de lignes de motif** (`vdc.c`) : la même rangée de tuile se
  rend sur beaucoup de lignes ; les 8 nibbles d'une rangée de motif sont
  gardés dans un cache direct de 16 entrées indexé par l'adresse (le même
  choix que le `bg_tile_cache` de mednafen). Une écriture VRAM (port ou
  DMA) retire l'entrée correspondante.
- **PSG** : un canal à volume 0 sort de la somme après son avance de
  phase (le produit par zéro tombait de toute façon).
- La tâche de rendu du cœur 0 passe en `-O2` (le projet construit `-Os`
  partout ailleurs).

**Pourquoi pas d'assembleur SIMD** : l'extension vectorielle S3 de cette
toolchain (binutils 2.35, esp-2021r2 — répertoire relevé par sonde de
l'assembleur et croisé avec esp-nn/esp-dsp) n'a **ni décalage par lane
16 bits, ni multiplication haute, ni compactage de lanes** — seulement
des add/sub saturants, `vmul.s16` (moitié basse), `vzip/vunzip`, `src.q`,
les logiques et des décalages 32 bits. Le filtre 5-in-8 a besoin du
`>>3` par canal et l'expansion 4bpp d'un renversement de bits par
nibble : les deux tombent. La route « canaux séparés en lanes 32 bits »
aboutit à ~60 opérations vectorielles par 8 pixels contre ~60 scalaires
au code C actuel — aucun gain. Les tables (VCE, `spread4`, BAT) sont des
gathers, absents de l'ISA. Le goulot mesuré (phase A) est de toute façon
l'exécution guest du cœur 1, pas la chaîne pixels du cœur 0 : les
leviers restent la partition ROM + `-O2` (phase D) et des régions plus
longues (E2).

**Le triage fidélité/vitesse, façon émulateurs classiques** : le port a
déjà les coupes standard — chronométrage mémoire par instruction (pas
par accès), PSG 11025 Hz mono, 16 sprites par ligne, rendu VDC borné à
la fenêtre active, palette reconstruite à l'écriture. Ce qui reste
coûteux est visible s'il est coupé : le pas de ligne (les raster IRQ de
Raiden), le budget par bloc (frontière de tranche), l'entrée/sortie de
région (5 registres + les fenêtres, par bloc — la région générée type
fait 1 à 5 instructions). Le mode « rapide mais moins fidèle » n'a donc
pas de prise ici sans casser le pas de ligne ; la marge se gagne en
qualité de code (D, E2), pas en fidélité.

### Passe VDP du 27 octobre 2026

Le VDC conserve les indices de palette sur 9 bits et la VRAM de 64 Kio.
Le rendu des sprites décode maintenant les quatre plans par groupes de
quatre pixels, sans cache de VRAM. Le tampon sprite est effacé et combiné
sur l'étendue effectivement couverte. Le marqueur interne du sprite 0 a
désormais 32 bits : auparavant son bit 16 était tronqué, donc la collision
ne pouvait pas lever `VDCS_CR`. Le test VDC couvre les bords gauche et
droit, les deux moitiés des sprites de 32 pixels, le retournement, les
plans multicolores, la priorité, la collision, la limite de 16 sprites,
le scroll et une écriture VRAM entre deux lignes. Les cas de sprites sont
recoupés avec le décodage bit à bit du motif. La correction de collision
peut changer le statut VDC et les hashes des jeux qui lisent ce bit.
La sélection verticale des sprites utilise le compteur de ligne précédent :
le compteur RCR a déjà avancé quand le modèle effectue le fetch. Au titre
de Raiden, cela corrige le placement du texte sprite d'une ligne ; un test
reproduit ce cas.

Profil synthétique hôte avant la correction du fetch vertical (`-O3`,
1 million de lignes par scène, même checksum) : ligne vide 79–83 ms,
fond dense 635–643 ms, fond et douze
sprites 1 505 ms avant le décodage groupé contre 1 420 ms après. Ce sont
des temps sur le PC de développement, pas des mesures AKA. Le coût VCE,
la copie des lignes et le DMA restent à mesurer séparément sur AKA ;
aucun changement de leur algorithme n'est retenu sans gain constaté.
Un microbanc hôte des mêmes opérations mesure 194 ms pour un million de
conversions de 256 indices VCE, 6 ms pour un million de copies de ces
lignes et 20 ms pour 100 000 DMA de 256 mots (ports compris). Ces boucles
isolées indiquent le coût relatif des postes sur ce CPU, pas celui d'une
trame Raiden ni d'un ESP32-S3.
Le projet Raiden précis pour AKA construit avec PlatformIO : 141 096
octets de RAM utilisés sur 327 680 (le ring DMA de quatre lots, les deux
contextes de lignes du cœur 0, la LUT VCE finale et le cache de motifs
ajoutent ~38 Kio) et 3 022 429 octets de flash sur 3 072 000 (rapport de
compilation, aucune mesure sur console).
Dans gm0 AKA (`4b4e145`), ce firmware Raiden s'arrête dès l'initialisation
sur `AKA memset 00000000 + 1070170304 pc=400011e8` ; le compteur
`--frames 24` et son débit affiché ne mesurent donc aucun rendu VDP.
Raiden depuis le même projet précis : au titre (300 trames), interpréteur,
natif et fenêtre PC impriment `run_hash=66b29e2e` et les trois PPM sont
identiques ; avec `output/raiden-pc/seed.input` (1 100 trames), ils
impriment `run_hash=6c7927fa` et les PPM sont encore identiques. Ces
captures restent dans `logs/`, hors suivi Git.
Le balayage optionnel `--pce-sweep` marche depuis une seconde file,
plafonnée (ce qui reste du budget code, 16384 instructions sinon) : la
marche de la couverture et des vecteurs reste illimitée et prioritaire,
les instances balayées s'ajoutent après elle, et le budget explicite
retire les instances excédentaires avant de planifier les régions. Cela
évite que des octets de données ressemblant à `JSR` ou `JMP` génèrent
un fichier C géant ; le repli interprété couvre les entrées écartées.
`pce_end_to_end` passe à nouveau (104 s sur cet hôte) après ce bornage.

La [référence mednafen](https://github.com/libretro/beetle-pce-libretro/blob/master/mednafen/hw_video/huc6270/vdc.cpp)
produit au titre, à la trame 300, exactement les mêmes 61 440 pixels : sa
capture de 256 × 243 est recadrée de trois lignes en haut pour comparer
la zone 256 × 240. En partie à la trame 1 100, avec RUN sur les trames
450 à 480 dans les deux pilotes, 65,52 % des pixels concordent ; décaler
RUN d'une trame dans le pilote libretro porte cette proportion à 98,35 %.
La convention d'entrée influe donc sur la comparaison ; les différences
restantes ne démontrent pas à elles seules une erreur de scroll.

Mednafen place le latch BXR dans la phase horizontale HDS, après le latch
BYR et avant HDW. Le modèle ici rend une ligne après son budget CPU entier :
un traçage comparé des écritures et de leur position horizontale reste
nécessaire pour les changements de scroll pendant l'affichage. Les cas de
bord synthétiques et le titre ne prouvent pas la fidélité de toutes les
scènes de sprites ni de tous les jeux.

- La cible META du recompilateur — la VRAM de 64 Kio ne tient pas dans
  les 32 Kio de SRAM : il faudra la fenêtrer en flash comme la ROM Sega.
- Les contextes que la marche ne prouve pas (les jeux qui changent de
  banc par une table) ne se compilent qu'avec un profil ; une extension
  du tracer qui compterait par contexte permettrait des profils plus
  complets encore.
- Le PSG complet (LFO, balance, polynôme de bruit) et l'entrée audio du
  harness comparée à la référence.
