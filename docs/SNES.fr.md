# SNES (`--system snes`)

Le système `--system snes` produit un projet `--target desktop` (le harness
headless) ou `--target pc` (la fenêtre SDL2). Le cœur : le 65C816 interprété
ou recompilé en C devant le bus LoROM/HiROM, le PPU rendu ligne par ligne,
le S-SMP (SPC700) et le S-DSP interprétés. Ce n'est pas un firmware pour les
consoles : la matrice de capacités tient le SNES hors du META et du Pokitto
(le runtime demande ~436 Kio de RAM). Les fenêtres, la mosaïque, la math de
couleur, l'écho du DSP et les modes 5/6 ne sont pas implémentés. Le timing
CPU, DMA et vidéo est approximatif et n'a pas été comparé à une SNES ; les
per-diagnostiques ci-dessous ne sont ni un émulateur de référence ni une
preuve de compatibilité.

```sh
cmake -G Ninja -B build . -DBUILD_TESTS=ON && ninja -C build
build/bin/snes_host chemin/vers/cartouche.sfc --frames 120 --capture logs/snes.ppm --wav logs/snes.wav
```

La capture est une image PPM 256 × 224, le son un WAV 32000 Hz stéréo. Les
ROMs restent hors du dépôt. `--input fichier.txt` accepte des lignes
`frame boutons` (`0 start`, `5 right+b`) ; les noms sont `b`, `y`, `select`,
`start`, `up`, `down`, `left`, `right`, `a`, `x`, `l` et `r`. `--interp` force
le chemin interprété d'un projet natif, `--coverage` écrit le profil du
recompilateur.

## La machine

- **65C816** : les 256 opcodes interprétés (la matrice WDC : pénalités de
  page, de largeur et de page directe ; le mode décimal dans les deux
  largeurs, la même formule BCD des deux chemins ; l'émulation) et
  recompilés — l'émetteur pose le MDR du fetch à chaque instruction, si bien
  que les lectures en bus ouvert donnent la même valeur des deux chemins.
  Les vecteurs suivent le mode du processeur (BRK/COP/NMI/IRQ prennent la
  table d'émulation $FFE4-$FFEE ou la native $FFF4-$FFFE), et les JMP
  indirects (abs), (abs,X) et [abs] lisent leur pointeur dans le banc
  programme K, jamais DBR.
- **Unité de multiplication/division** ($4202-$4217) : WRMPYA (8×8 signé →
  produit $4216/7) et WRDIV (16/8 non signé → quotient $4214/5, reste dans
  $4216 ; division par zéro = $FFFF/$FF), les opérandes verrouillées à
  l'écriture comme sur la puce.
  La SRAM de la cartouche ($6000-$7FFF, taille de l'en-tête) et le port WRAM
  sont mappés.
- **PPU** : VRAM/CGRAM/OAM et leurs ports, le DMA général (modes 0-7) et le
  HDMA (tables directes et indirectes, une unité par ligne pendant le
  HBlank), les timers d'IRQ H/V ($4207-$420B, le niveau de TIMEUP), les
  limites d'objets (32 sprites, 34 tuiles par ligne, les drapeaux de $213F)
  et le rendu par ligne des modes 0-4 (offset-per-tile, direct color) et du
  mode 7 (matrice affine, hors EXBG). Les fenêtres ($2123-$212F : deux
  fenêtres par couche, inversions, ET/OU/XOR/XNOR, TMW/TSW), la math de
  couleur ($2130-$2132 : clip noir et prévention par la fenêtre de couleur,
  addend fixe ou sous-écran, addition/soustraction, half) et la mosaïque
  ($2106) entrent dans le compositing : chaque ligne compose un écran
  principal et un sous-écran. Les lignes gardent les registres tels
  que le code les a laissés : une écriture pendant le HBlank d'une ligne
  atteint la suivante.
- **APU** : le SPC700 interprété (les 256 opcodes, les timers à 8/64 kHz,
  leurs sorties à source/(cible+1)), la RAM A de 64 Kio et l'amorce IPL
  simulée en C suivant le protocole réel de la ROM IPL (pas de ROM exécutée) :
  à la remise à zéro le S-SMP répond $AA/$BB sur les ports 0/1 ; $CC sur le
  port 0 avec le mot de destination dans $2142/3 ouvre un transfert (octet
  haut non nul = blocs, nul = saut) ; chaque octet porte son compteur sur
  $2140, que le S-SMP renvoie en guise d'accusé ; un compteur signé en avance
  termine le transfert (port 1 nul = sauter à l'adresse, sinon bloc suivant,
  compteur reparti à zéro). Le S-DSP rend huit voix BRR
  (filtres, bouclage, drapeaux de fin) avec ADSR ou GAIN, le pitch compteur
  et les volumes, dans un anneau que le harness et la fenêtre drainent à
  32000 Hz.
- **Pad** : un contrôleur en lecture série ($4016) et automatique ($4218,
  temps approximé à trois lignes).
- **Timing du bus** : le DMA général et le HDMA volent leur temps de bus au
  CPU (modèle : 16 cycles de démarrage par canal, 8 par octet ; la tranche
  d'atterrissage d'un DMA lancé en plein milieu est approximative, le total
  sur la trame est exact) ; le S-SMP et le S-DSP gardent le temps complet de
  leur ligne (ils tournent en parallèle sur la machine). Le H-IRQ en
  dessous du dot 256 coupe la partie visible à son dot ; au-delà, il reste
  sur la frontière après le HDMA.

### Ce qui n'est pas modélisé

Le son de l'écho (la zone RAM, le feedback, les filtres), le bruit du DSP,
la modulation de pitch, l'interpolation gaussienne (les voix lisent leur
plus proche échantillon), les tables ADSR exactes ; les modes PPU 5 et 6,
l'EXBG du mode 7 ; le half sur l'addend fixe (le modèle halve toujours le
second opérande, la quirk matérielle du fixed color n'est pas tranchée) ;
la tranche où atterrit un DMA lancé en plein milieu d'une
tranche (le total sur la trame est exact) ; le HTIME au-delà du dot 256
(la comparaison tombe sur la frontière après le HDMA) ; le résultat exact
du DIV en dépassement du SPC700 ; les jeux qui remettent le S-SMP à zéro
en cours de jeu ; le temps de lecture automatique exact. Les jeux dont les
régions recompilées s'étendent au-delà d'une frontière de ligne s'arrêtent
à l'instruction suivante du mode rapide : le contrat native == interpréteur
est celui du mode précis.

## Le recompilateur 65C816 → C

Comme le HuC6280 de la PCE : une instance d'instruction est une adresse
24 bits sous un contexte (K, DBR, page directe, drapeaux M/X/E), la marche
statique part du vecteur de reset et suit ce qu'elle prouve, et le
dispatcher n'entre dans une région que si le contexte de la machine
correspond encore. Le mode précis vérifie le budget après chaque
instruction et sort avant toute accès possible à l'I/O ; le mode rapide ne
vérifie qu'aux étiquettes — c'est lui qui exécute les formes longues et
les lectures en bus ouvert à l'intérieur d'une région.

```sh
build/bin/m0recomp --system snes cart.sfc --target desktop -o projet --meta-precise
host/build/cart_host --interp --coverage cov.txt   # le profil, 120 images de jeu
build/bin/m0recomp --system snes cart.sfc --target desktop -o projet --meta-precise --coverage cov.txt
```

Le balayage de la collection (`tools/snes_sweep.py`) enchaîne les deux pour
chaque `.sfc`/`.smc` d'un répertoire : le harnais interpréteur joue les
trames (le profil), le convertisseur repasse avec, et l'hôte du projet
construit doit imprimer le run_hash et les cycles de son `--interp` —
`python3 tools/snes_sweep.py roms/snes --frames 300`, le bilan va dans
`logs/snes_sweep.txt`. Un répertoire sans ROM se déclare et sort à zéro.

## Ce qui a été vérifié

- Les tests C (`ctest -R snes_runtime`) : le bus et ses miroirs, la SRAM,
  les interruptions (les deux voies, l'émulation et le natif), les appels
  longs, WAI/STP, le DMA et le HDMA (table qui peint la CGRAM), les timers
  d'IRQ (H chaque ligne, V une ligne, le niveau sans lecture de $4211), le
  DMA vidéo, les ports OAM/CGRAM, les limites d'objets, les modes 2/3/7,
  l'amorce APU complète (un programme uploadé par le protocole IPL réel arme
  une voix, l'échantillon attendu arrive dans le ring ; la signature $AA/$BB
  de la remise à zéro vérifiée), la multiplication et la division ($4202-
  $4217, division par zéro comprise), les fenêtres (les deux fenêtres, leurs
  inversions, ET/OU/XOR/XNOR par couche, TMW/TSW), la math de couleur
  ($2130-$2132 : le clip noir, la prévention, l'addend fixe ou sous-écran,
  l'addition et la soustraction, le half), la mosaïque et les périodes des
  timers SPC700.
- Le e2e (`snes_native_end_to_end`) : un ROM synthétique dont la queue
  charge par LDA long,X, stocke en STA long et lit un bus ouvert — le
  projet `--meta-precise` imprime le run_hash de son `--interp` (marche
  statique puis profil), la capture est le fond rouge attendu, et la même
  égalité tient en mode rapide, seul mode où ces instructions restent dans
  une région.
- **Dr. Mario (Japan)** (LoROM rapide, 512 Kio, hors dépôt) démarre : écran
  titre, menu d'options puis partie jouable avec l'input scripté de
  `logs/dm_play.txt` (Start au titre, Start pour lancer). À 900 images le
  projet `--meta-precise` imprime
  `run_hash=ac1e4b48 image_hash=f2a41fc5 audio_hash=e6f1d555`, identiques à
  son `--interp`, cycles compris. Le jeu sert son programme APU par le
  protocole IPL et se sert de l'unité de multiplication/division — les deux
  sont venues avec lui. Avec le profil du balayage (`tools/snes_sweep.py`,
  300 images), le contrat tient aussi : `run_hash=e2b29de1` des deux chemins,
  5 399 instructions compilées (1 803 jouées).
- **Bucket** (homebrew, 256 Kio LoROM) démarre : après 120 images l'écran
  montre son fond pointillé animé (l'empreinte d'image change d'une image à
  l'autre) et le seau — les sprites du bas — suit la croix ; sur le harnais,
  le chemin natif imprime `run_hash=9dd56c31 image_hash=b295c090
  audio_hash=10d3e0a5` à 120 images, identiques à son `--interp`. Bucket ne
  boote pas le S-SMP (A-RAM vide, pas de poignée de main) : son silence est
  le silence du jeu, pas un manque du cœur. Ces observations restent une
  comparaison avec rien : aucun émulateur de référence n'a tourné ici.
- Les cartouches de diagnostic locales (`output/snes/`, hors dépôt) : le
  projet `--meta-precise` imprime le run_hash de son `--interp` sur les
  trois, cycles compris — CPUADC `run_hash=8d8e6d5c image_hash=a713d1ab` (le
  mode décimal, l'écart qui a révélé le BCD incomplet du code généré),
  CPUMOV `run_hash=a94f066f image_hash=53b0775f`, CPUMSC
  `run_hash=aecf6017 image_hash=78729dc5`, à 120 images.

## La cible PC

`--target pc` écrit le projet SDL2 : la fenêtre joue à la résolution native
256 × 224, le son est le ring du DSP à 32000 Hz, la cartouche est embarquée.
Clavier : flèches, Z = B, X = A, A = Y, S = X, Q/E = L/R, Entrée = Start,
Maj = Select, Échap quitte ; la manette mappe A/B/X/Y et ses épaules.
L'empreinte vient de `snes_hash.h`, partagé avec le harness : une session
jouée dans la fenêtre imprime le `run_hash` et l'`audio_hash` du harness —
`tests/test_pc_target.py` le vérifie avec un script rejoué des deux côtés,
et la fenêtre tourne tête-bêche sur la CI (SDL2 requis).

## Chantiers suivants

L'écho du DSP (le bruit, la gaussienne, la modulation de pitch, les
tables ADSR), les modes PPU 5/6 et l'EXBG du mode 7, les coprocesseurs de cartouche (DSP-1,
SuperFX, SA-1), une comparaison contre un émulateur de référence (le bilan
mednafen de la PCE), et — si un jour les budgets de RAM le permettent — le
streaming des ROMs pour les consoles.
