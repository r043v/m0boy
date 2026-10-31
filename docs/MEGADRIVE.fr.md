# Mega Drive : recompilation 68000 expérimentale

`m0recomp --system megadrive --target pc` génère un projet C jouable qui ne
dépend que de SDL2 pour sa fenêtre et ses entrées. Le CPU 68000, la mémoire,
le VDP et le rendu sont dans m0boy ; aucun cœur d'émulation externe n'est
chargé. Le générateur émet en C les opcodes couverts par un profil, puis les
exécute par le décodeur 68000 interne si une adresse n'est pas encore couverte.
Les ROMs restent dans `output/` et ne sont pas ajoutées au dépôt.

Pour jouer sur PC :

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
./build/bin/m0recomp --system megadrive \
  'roms/md/Sonic The Hedgehog (USA, Europe).md' --target pc -o output/sonic_md
cmake -G Ninja -S output/sonic_md -B output/sonic_md/build
ninja -C output/sonic_md/build
output/sonic_md/build/md_pc
```

Pour fabriquer directement les exécutables Linux et Windows à partir de la
ROM locale, utilisez `tools/megadrive/build_pc_binaries.sh [ROM] [DOSSIER]`.
Le profil du C natif vient de la recherche de parties de
`tools/profile/m0search.py` : la graine Start/droite/saut, puis des sessions
aléatoires rejouées sur l'interpréteur, et les enregistrements passés avec
`EXTRA_INPUT` (parties capturées au moyen de `--record-input`, même dialecte
que `--input`) ; les couvertures sont fusionnées avant la génération :

```bash
tools/megadrive/build_pc_binaries.sh \
  'roms/md/Sonic The Hedgehog (USA, Europe).md'
```

Les fichiers livrés sont `output/<slug>-pc/<slug>-linux` et
`output/<slug>-pc/<slug>.exe` (le slug vient du nom de la ROM), avec
`SDL2.dll` à côté du `.exe`. Le binaire
Linux nécessite SDL2 installé sur le système. Le cross-build Windows demande
MinGW x86-64 et son paquet SDL2. Le script attend le recompileur déjà construit
dans `build/bin/m0recomp` (ou le chemin défini par `M0RECOMP`). Les programmes
intègrent la ROM passée au script ; gardez-les dans `output/`, conformément à
la règle qui exclut les exécutables dérivés de ROMs du dépôt et des archives.
La cartouche peut avoir un pointeur de pile initial nul (Sonic 3 installe le
sien) ; son registre SRAM `$A130F1` (bit 0 : activation, bit 1 : protection en
écriture) mappe la SRAM de `$200000` à `$20FFFF`, volatile au remise à zéro.
Sur PC la SRAM persiste : `--save FICHIER` choisit la pile (par défaut
`<nom de cartouche>.sav` dans le répertoire courant) — relue au démarrage,
réécrite environ toutes les deux secondes de jeu où la cartouche écrit, et à
la sortie.

Flèches pour bouger, X/Z/C pour A/B/C, Entrée pour Start, Échap pour quitter.
Une manette SDL2 utilise la croix ou le stick gauche, B/A/X pour A/B/C et Start.
Un joystick non reconnu comme manette utilise son chapeau ou ses axes et ses
boutons 0–3 pour A/B/C/Start. Le branchement à chaud est pris en charge.
L'image est sans filtre par défaut. F8 parcourt les filtres de gm0 (pixel,
DMG, LCD RGB, scanlines, grille, puis aucun),
F10 change l'échelle et F11 bascule en plein écran. Les filtres ne modifient
pas les captures ni le hachage de l'image native.

gm0 reste un programme voisin facultatif pour exécuter les firmwares des
consoles portables. Son interface SDL2 est intégrée à son émulation matérielle
et n'expose pas de bibliothèque de rendu ou de manette. En faire une dépendance
du projet Mega Drive généré importerait les machines META/Pokitto/ESPboy et
Zlib sans servir le cœur 68000. Le module SDL commun à toutes les cibles PC
reprend donc ses commandes et la géométrie 3× de ses filtres, avec SDL2 comme
seule dépendance.

Le PC exécute le Z80 de m0boy pour le pilote sonore, décode ses 8 KiB de
RAM, sa fenêtre ROM, le bus et les ports YM2612/PSG. Le 68000, le Z80, le
YM2612 et le PSG partagent l'horloge maître NTSC : avant chaque accès du
68000 à l'espace Z80, au PSG ou aux lignes BUSREQ/RESET, le Z80 et la
synthèse rattrapent l'instant de cet accès, et chaque écriture YM/PSG du Z80
prend effet à son cycle. Les échantillons DAC (le « SEGA » et les percussions
de Sonic) sont donc rendus à leur cadence. L'interruption du Z80 est tenue
une ligne à partir du début du blanking vertical. Le YM2612 est remis à zéro
avec le Z80, comme sur la console.

Le YM2612 (`md_ym2612.c`) produit un échantillon toutes les 144 horloges
(53,3 kHz) : générateur de phase 20 bits avec détune et multiplicateur, tables
log-sinus et exponentielle, enveloppes à compteur global (taux, échelle de
touche, niveau de sustain, seuil de silence), SSG-EG, huit algorithmes,
rétroaction, mode spécial du canal 3 et CSM, LFO AM/PM, minuteries A/B,
drapeau busy (32 horloges internes après une écriture de données),
panoramique, DAC, et le DAC 9 bits du YM2612 discret avec sa discontinuité
autour de zéro. L'état des touches est lu une fois par échantillon, comme sur
la puce. Les valeurs tirées des registres (taux d'enveloppe avec l'échelle de
touche, niveaux, détune, multiplicateur, pas de phase par bloc/F-num) sont
préparées à l'écriture, et un opérateur sans touche au-delà du seuil de
silence ne garde que son enveloppe, jusqu'à son plancher 0x3ff : le son reste
le même, l'oracle ymfm et les `audio_hash` le pincent. Le PSG (SN76489 de Sega) compte à 223,7 kHz, avec son bruit à
registre 16 bits. Le mélange est rééchantillonné à 44,1 kHz stéréo, puis le
continu est retiré ; `--wav FILE` l'enregistre même sans fenêtre
(`--frames N`), `--ym-log FILE` garde chaque écriture YM2612 avec son
échantillon, et `audio_hash` permet de comparer le code natif et
l'interpréteur.

ymfm (Aaron Giles, BSD-3, recoupé avec Nuked-OPN2) sert d'oracle de test et
n'entre jamais dans un projet généré : clonez-le dans `output/deps/ymfm` (ou
passez `-DYMFM_DIR=`) et ctest construit `md_ym_compare`, qui rejoue un
journal d'écritures dans les deux. `megadrive_ym2612_matches_ymfm` exige les
mêmes échantillons, une fois mis à l'échelle de ymfm, sur des journaux
aléatoires qui parcourent algorithmes, SSG-EG, LFO, canal 3 spécial et DAC ;
avec la ROM locale, le test Sonic rejoue aussi les 26 s de sa séquence. Le
CSM n'est pas couvert (les minuteries de ymfm dépendent de son hôte). Les
filtres analogiques de sortie, qui changent selon le modèle de console, ne
sont pas reproduits, et le son n'a pas été comparé à une Mega Drive.

Le 68000 suit les temps du manuel MC68000 (`md_m68k_time.h`) : temps de
chaque instruction et de ses modes d'adressage, branchements pris ou non,
DBcc, Scc, décalages par registre, MOVEM par registre, MULU/MULS selon les
bits et DIVU/DIVS selon les étapes de la division, 44 cycles par
interruption. États d'attente, préchargement fin et blocage du CPU pendant le
DMA du VDP ne sont pas modélisés. Le code généré plie ces temps en
constantes par instruction (`fc_`, l'ombre de cycles de la région) et garde
la limite de tranche dans un registre : un seul test par instruction, sans
relecture mémoire — la limite est rechargée aux seuls points où une aide
peut la bouger (`md_set_sr`, écriture VDP, fault). Le jeu d'instructions comprend ADDX/SUBX,
CMPM, ABCD/SBCD/NBCD, TAS, CHK, TRAP/TRAPV, RTR, MOVEP, les exceptions
(division par zéro, instructions illégales, lignes A et F) et le changement
de pile superviseur/utilisateur.

Le CPU tourne par tranches d'une ligne (488 cycles, 262 lignes) : le VDP
rend chaque ligne à la fin de la sienne, l'interruption verticale part à la
ligne 224 et l'interruption horizontale suit le compteur du registre 10, si
bien qu'un gestionnaire H-int qui change palette ou défilement touche la
ligne suivante. Les sprites sont lus par ligne (plans de 32, 64 ou 128 cellules, plan Window), avec les limites de 20
sprites et 320 pixels (16 et 256 en H32) et le masquage par x = 0 après un sprite d'x non nul. Une
boucle d'attente (`TST`, `BTST #` ou `CMPI` sur une adresse absolue de RAM,
puis `Bcc.s` vers elle) est consommée d'un coup jusqu'à la fin de sa
tranche, à l'endroit exact où l'exécution pas à pas se serait arrêtée.

SDL2 est la seule dépendance du frontend PC. `--frames N --ppm FILE` permet
une capture sans fenêtre visible, et `--input FILE` rejoue des lignes
`trame masque` où haut=1, bas=2, gauche=4, droite=8, A=16, B=32, C=64 et
Start=128 ; `--record-input FILE` enregistre une partie jouée au même
format, rejouable telle quelle par `--input` et lisible par `m0search.py`
comme session manuelle du profil.

## Profil natif

Le générateur traduit le 68000 en C (`recompiler/src/system/megadrive/emitter.cpp`,
`src/md_full_code.c`) sur le modèle des cœurs NES et Vectrex : les
instructions atteintes depuis les vecteurs et le profil sont regroupées par
adresses contiguës en régions, une fonction C chacune, avec des `goto`
internes. Chaque sortie revient au dispatcher (`md_full_step_native`), qui
cherche l'entrée suivante ; RTS, RTE, sauts par registre et exceptions
laissent `pc` à chercher, et ce qui n'a pas d'entrée (code en RAM, adresse
jamais atteinte) passe par l'interpréteur. Le C généré appelle les mêmes
aides que l'interpréteur (`md_m68k_ops.h`), plie les mêmes temps
(`md_m68k_time.h`), lit les opérandes ROM constants comme constantes et
vérifie la fin de tranche après chaque instruction : un projet doit donner le
`run_hash`, l'`image_hash` et l'`audio_hash` de `--interp`.

La couverture des adresses 68000 exécutées se collecte avec `md_pc --interp
--coverage FILE`, puis se passe au générateur avec `--coverage FILE` (une ou
plusieurs fois) ; `tools/profile/m0search.py` enchaîne les sessions aléatoires
et les rejeux manuels et rend la couverture fusionnée, pour ce cœur comme
pour tous les autres. Sans profil, le parcours statique depuis les vecteurs suffit
souvent ; `native_hits` (entrées de région) et `fallback_hits` (instructions
interprétées) rendent le partage visible. `megadrive_m68k_end_to_end`
compare l'interpréteur, le C du seul parcours statique et le C profilé sur
des programmes 68000 aléatoires (`tests/md_fixture.py` : presque tout le jeu
d'instructions, exceptions, interruptions V et H, boucle d'attente), et
vérifie les bandes de couleur d'une H-int.

Le test facultatif `megadrive_sonic_pc_native` utilise la ROM Sonic locale,
collecte le profil du titre et d'une séquence Start/jump, puis vérifie que la
génération C donne les mêmes états et pixels que l'interpréteur, sans fallback
sur cette séquence. Ne pas confondre ce résultat avec une compatibilité
générale : Sonic 1 est jouable dans ce frontend, mais le plan fenêtre, le
mode ombre/surbrillance et l'entrelacement du VDP manquent, et d'autres
cartouches n'ont pas été essayées. La
META n'est plus une cible du Mega Drive ; la jouabilité sur matériel embarqué
(AKA) n'est pas établie.

Une ROM synthétique de test vérifie aussi l'en-tête, les vecteurs, l'exécution
du décodeur, la génération C et le rendu d'une tuile. La ROM libre de
[blastcomp](https://github.com/staruwos/blastcomp) a servi à valider la
reconnaissance du format, mais n'est pas incluse dans le dépôt.

## Gamebuino AKA

`--target aka` produit le même projet natif pour l'ESP32-S3 de la Gamebuino
AKA (`docs/AKA.fr.md`), compilé en -O3. Machine `md_full` en SRAM interne
(64 Kio de work RAM, 64 Kio de VRAM, la SRAM de cartouche, le Z80 et le son),
et pas de framebuffer : le VDP rend chaque ligne RGB565, pendant la trame,
directement dans un anneau de quatre lots de 8 lignes en SRAM que le DMA du
bus i80 vide pendant que l'émulation continue (la fenêtre d'adressage du
panneau est posée une fois par trame, les lots partent en file ; un lot n'est
rempli qu'après la prise de son créneau DMA, dont le nombre égale l'anneau —
le transfert qui le lisait encore est alors fini) — rien ne va en PSRAM. La
palette est tenue à jour à chaque écriture CRAM (LUT ARGB pour le rendu de
référence, LUT RGB565 pour le panneau), le rendu trace des colonnes de tuiles
entières (entrée de nametable et priorité une fois par colonne, colonnes de
motif deux pixels par octet de motif) et les tranches de tuiles tournent en
assembleur Xtensa (`md_video_asm.S`, deux pixels par octet lu), comparé au C
par `md_span_selftest` au démarrage (écran rouge si le noyau diverge) ;
`MD_VIDEO_ASM=0` dans `md_config.h` garde le C. Le second cœur rend : la
tâche d'émulation ne capture par ligne que ce que le rendu lit hors VRAM
(`MdLineCtx` : registres VDP, VSRAM, LUT CRAM — 256 octets, mesuré à ~0,2 %
de la trame côté hôte) et une tâche sur le cœur 0 dessine chaque lot de 8
lignes un lot en retard, via `md_full_render_line565_ctx`, puis le donne au
DMA ; l'émulation ne peut passer devant que d'un lot. La VRAM elle-même est
lue au moment du rendu : une écriture 68000 pendant l'affichage actif peut
donc traverser une ligne (effet visible seulement si un jeu réécrit des
motifs ou la nametable à chaud) ; registres, VSRAM et palette sont ceux de la
ligne. Estimation (comptage d'instructions hôte, pas une mesure console) :
~6,0 M instructions par trame sur chaque cœur au lieu de ~12,0 M pour
émulation + rendu enchaînés sur un seul — chemin critique divisé par deux
par rapport au dernier estimé à 60-90 ms/trame, soit de l'ordre de 22 à 33
images par seconde attendues. Le moteur audio émet la ligne 44,1 kHz depuis
une position courante (plus de division 64 bits par échantillon) et la phase
d'interruption du Z80 avance d'un conditional-subtract par instruction (plus
de modulo 64 bits que le driver sonore paie à chaque pas). Le profil vient
des sessions PC (`--coverage` des projets `*-pc`). A = A, B = B, MENU/C = C,
RUN/D = Start, le joystick complète la croix. Pas de menu d'options, pas
d'états ; la SRAM de cartouche reste volatile. Sonic 1, Sonic 3, Castle of
Illusion et QuackShot se construisent ainsi (`pio run`) ; les firmwares n'ont
pas été exécutés sur la console — gm0, qui peint les firmwares Sega réels de
façon incomplète sur sa machine AKA, montre un écran noir. La construction du
YM2612 et du PSG ne contient plus de virgule flottante (tables log-sinus,
exponentielle et amplitudes précalculées en constantes, audio bit-exact).
