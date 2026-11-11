# gbrecomp pour Gamebuino META et Pokitto

`gbrecomp` transforme une ROM Game Boy (DMG) en firmware natif pour la
Gamebuino META. Le code du jeu est recompilé en C puis compilé pour la
console : ce n'est ni un JIT ni un émulateur classique. Le code déjà vu
pendant une partie de profilage est traduit, et un petit interpréteur
exécute le reste.

## Nouveautés de la version 0.10.27

- Game Gear / Master System sur la META : le décodeur des secteurs ROM
  compressés tourne en flash, plus en SRAM. Depuis la SRAM, processeur à
  70 MHz, il décodait certains secteurs de travers : écrans noirs, images
  cassées, plantages selon les parties (Gunstar Heroes divergeait de
  l'hôte avant la trame 600, différemment à chaque partie ; identique
  ensuite). Refaire les jeux GG / SMS.

## Nouveautés de la version 0.10.26

- Game Gear / Master System : le cache carte est celui de la Game Boy
  (blocs de 128 octets, 8 blocs sur la META, remplacement CLOCK) ; Ristar
  lit la carte bien moins souvent (trames sans lecture : 1 867 → 2 303 sur
  2 400). Une écriture de sauvegarde refusée par la carte n'est plus perdue.
- Game Gear / Master System : le code natif prend les interruptions au
  plus tard 40 cycles Z80 après leur échéance (auparavant : au prochain
  tour de boucle). Corrige la bande noire de 12 lignes dans l'intro de
  Ristar (scène de la bulle), pour moins de 1 % de temps.
- Son par interruption sur toutes les consoles (`AUDIO_DMA=1` pour le
  DMA) : le DMA chaîné tombait sur l'erratum 1.7.2 du SAMD21 et écrasait de
  la RAM (redémarrages de Ristar). La RAM gagnée va au cache.
- Écran : une fenêtre par suite de lignes au lieu d'une par bande (−4,6 %
  de temps par image sur Ristar).
- Mêmes couleurs d'erreur au démarrage sur toutes les consoles : bleu pas
  de carte, rouge ROM absente, orange fichier fragmenté, magenta autre
  ROM, jaune `.SAV` absent.
- Carte : tous les dossiers de premier niveau sont cherchés (avant : les
  32 premiers seulement, écran rouge au-delà).

## Nouveautés des versions 0.6.0 à 0.7.0

- Palettes du boot ROM Game Boy Color : comme sur une GBC, maintenir une
  direction (+ A ou B) au démarrage choisit l'une des 12 palettes
  génériques. Maintenir HOME (META) ou C (Pokitto) avec une direction en
  change pendant la partie. Quand le titre du jeu est reconnu, sa palette
  GBC est proposée dans le menu Palette (le Vert reste le défaut).
- Le menu d'options coupe le son.
- Pokitto : 48 MHz par défaut, la fréquence nominale de la console (72 MHz
  reste dans le menu Processeur) ; le tempo du son est corrigé, et la
  mesure du temps n'utilise plus de division (le M0+ n'en a pas en
  matériel). Une version par console
  (`targets/consoles/pokitto/VERSION`).
- `TARGET=both ./compile.sh` convertit pour les deux consoles en une
  seule passe ; un fichier `build_flags` à côté d'un jeu garde ses options
  de compilation d'une conversion à l'autre ; les reconstructions de
  version sont environ deux fois plus légères.
- META : TBLEND aussi dans les jeux lus sur la carte, option
  `-DMETA_LCD_12BPP=1` (écran en 12 bits, 25 % d'octets en moins par
  rangée). Les trois périphériques du bus SPI ont leur débit dans
  `gbl_board.h` : le maximum pour l'écran (48 MHz, 35 avec le processeur
  à 70 MHz : la PLL n'a qu'une fréquence ; 12 et 24 au menu « SPI ecran »),
  12 MHz pour la carte SD et pour les boutons : le SPI
  maître du SAMD21 ne lit pas MISO de façon fiable au-delà (à 24 MHz,
  toutes les lectures de secteur échouaient sur une META).

## Nouveautés de la version 0.5.0

- Pokitto : `gbrecomp --target pokitto` et `TARGET=pokitto ./compile.sh`
  (jeux dans `roms/pokitto/<Jeu>/`). Même runtime, même code généré et
  même menu que la META ; pilotes LPC11U68 écrits à la main (sans mbed ni
  PokittoLib), build `make` avec arm-none-eabi-gcc seul. Image 1:1 au
  centre de l'écran, ou agrandie en 200x176 comme gm0 (rognée ou entière,
  menu Écran) ; C court = Start, C + B = Select, C maintenu = menu, C au
  démarrage = loader. Testé dans l'émulateur seulement, pas sur la console.
- Pokitto plus rapide : écriture des pixels et interruption audio en
  assembleur depuis la RAM ; les deux bancs de 2 Ko servent à un cache de
  dispatch plus grand et à un cache de streaming de 8 secteurs (40 % de
  lectures SD en moins sur Zelda ; estimations).
- Un seul fichier de réglages des optimisations, par console :
  `targets/meta/runtime/gbl_profile.h` ; chaque optimisation est un
  `-D` indépendant.
- Firmwares META inchangés en comportement (code SD réorganisé, mêmes
  images et mêmes sauvegardes dans l'émulateur) ; le convertisseur propose
  de mettre à jour les jeux faits en 0.4.0.

## Nouveautés de la version 0.4.0

- Menu d'options : HOME maintenu une demi-seconde (le jeu est en pause).
  Reprendre, Redémarrer, sauvegardes d'état, Volume, Écran (mode
  d'affichage), Palette (Vert, Gris, Pocket, Ambre, Bleu), Images (saut
  d'images automatique, aucun, 1 sur 2/3/4), Vitesse (normale ou sans
  limite), Processeur (48 MHz, ou 72 MHz surcadencé : à essayer), Quitter
  vers le loader. Les choix ne sont pas gardés à l'extinction.
- Sauvegardes d'état : 10 emplacements dans le fichier `<NOM>.STA` que le
  convertisseur crée à côté du `.bin` ; copie-le sur la carte avec le jeu.
- Plus rapide : de 1 % à 10 % environ de temps en moins par image selon les jeux
  (interpréteur, dispatch du code compilé, divisions, drapeaux du code
  recompilé, joypad ; estimations), émulation identique. Le son passait par
  DMA (revenu à l'interruption par défaut en 0.10.26).
- 1,1 Ko de RAM rendus au firmware (objets série Arduino inutilisés).

## Nouveautés de la version 0.3.0

- Nouvel affichage par défaut, TBLEND : la dernière ligne de chaque rangée
  de tuiles est fusionnée avec la suivante au lieu d'être sautée, donc plus
  de texte coupé, pour quasiment le coût de l'ancien mode (+2 % par image
  en médiane, estimation). L'ancien mode (TILE) et SMOOTH (réduction
  complète, environ 15 % plus cher) restent disponibles avec HOME maintenu
  1 s (remplacé en 0.4.0 par le menu d'options). Les firmwares qui lisent le jeu sur la carte SD gardent TILE.
- Opérations arithmétiques du code recompilé et cache de dispatch en
  assembleur Thumb-1 : quelques pour cent de plus sur les jeux qui en
  dépendent (estimations), émulation identique.
- Les firmwares faits avec la 0.2.0 sont proposés à la reconstruction par
  `compile.sh` et le convertisseur Node (`gbrecomp --meta-version` donne
  la version courante).

## Nouveautés de la version 0.2.0

- Runtime plus rapide : les parties critiques (machine à états des lignes
  du PPU, calcul du prochain événement, accès mémoire et registres,
  rendu des lignes, OAM DMA, interpréteur) sont réécrites en assembleur
  Thumb-1 pour le Cortex-M0+ de la META. Sur 15 jeux, le temps
  d'émulation par image baisse de 15 % à 53 % (médiane −19 %), et les
  images trop lentes pour 60 Hz passent par exemple de 180 à 21 sur 1 600
  pour Prehistorik Man. Ce sont des **estimations** (modèle de cycles du
  M0+ sous émulateur), pas des mesures sur la console : dis-moi ce que tu
  observes.
- Le résultat de l'émulation est identique à la version 0.1.0 (vérifié
  image par image sur ces 15 jeux) ; la taille du firmware ne change
  presque pas (+0,2 Ko).
- Les firmwares faits avec la 0.1.0 sont proposés à la reconstruction par
  `compile.sh` (la version est inscrite dans chaque `.bin`).

## Prérequis

- CMake 3.20 ou plus récent, Ninja, un compilateur C11/C++20 (gcc, clang)
- [PlatformIO](https://platformio.org) : `pipx install platformio` ou
  `uv tool install platformio`. La plateforme `atmelsam` s'installe toute
  seule à la première compilation.
- SDL2 (paquet de développement), pour jouer sur PC pendant le profilage.
  Optionnel, mais c'est la façon pratique de couvrir tout le jeu.
- Python 3, seulement pour les tests.

## 1. Compiler l'outil (une seule fois)

```bash
cmake -G Ninja -B build .
ninja -C build
```

## 2. Transformer une ROM

Dans les exemples, remplace `tetris` et `roms/gb/tetris.gb` par ton jeu.

```bash
# Tout en une commande : ./compile.sh  (voir plus bas)
# a) Projet interpréteur seul et outils PC
./build/bin/gbrecomp roms/gb/tetris.gb --target meta --output-prefix tetris -o out/tetris
cmake -G Ninja -S out/tetris/host -B out/tetris/host/build
ninja -C out/tetris/host/build

# b) Jouer sur PC pour enregistrer le code réellement exécuté
#    Flèches, X = A, Z = B, Entrée = Start, Retour arrière = Select,
#    Échap = sauvegarder et quitter
./out/tetris/host/build/tetris_play --interp \
    --coverage out/tetris.cov --record-input out/tetris.input

# c) Régénérer avec la couverture (autant de --coverage que de parties)
./build/bin/gbrecomp roms/gb/tetris.gb --target meta --output-prefix tetris \
    --coverage out/tetris.cov -o out/tetris

# d) Compiler le firmware (vérifie les lignes RAM et Flash du rapport)
pio run -d out/tetris
```

Plus la partie de profilage explore le jeu (menus, niveaux, boss), plus le
jeu tourne en code compilé. Le reste passe par l'interpréteur, qui est plus
lent mais correct.

Sans SDL, rejoue un script d'entrées à la place de l'étape b). Chaque ligne
a la forme `<frame> <boutons>`, avec les boutons joints par `+` : `a`, `b`,
`select`, `start`, `right`, `left`, `up`, `down`, ou `none`.

```bash
./out/tetris/host/build/tetris_profile --interp --frames 3000 \
    --input mon_script.input --coverage out/tetris.cov
```

Même parcours sur les fenêtres PC des autres cœurs, chacun dans son dialecte
(`targets/consoles/pc/pc_input.h`) : NES et PICO-8 par variables
d'environnement (`M0_RECORD_INPUT=partie.input` enregistre,
`M0_INPUT=script.input` rejoue dans la fenêtre), PC Engine en ligne de
commande (`--record-input`, `--input`). Le harnais du projet relit ces
fichiers tels quels (`--input`, PICO-8 : `--input-file`).

## Conversion en lot : `compile.sh`

```bash
./compile.sh                        # ROMs de roms/gb/ -> roms/meta/<Jeu>/
./compile.sh mes_roms/ sortie/      # autres dossiers
PLAY=1 ./compile.sh                 # ouvre aussi une fenêtre pour jouer soi-même
PACK=0 ./compile.sh                 # sans compression de la ROM en flash (défaut : compressée si elle tient)
AUTO_SECONDS=600 AUTO_REDO=1 ./compile.sh   # refaire une partie automatique encore plus longue
REPLAY=1 ./compile.sh               # rejouer les parties à la main enregistrées
```

Dans un terminal, le script affiche la liste des ROMs avec leur état
(nouvelle, à jour, faite par une autre version, ROM modifiée) et te laisse
choisir les jeux. Hors terminal, il traite tout ce qui est à faire ;
`ALL=1`, `UPDATE=1` (reconstruit les firmwares d'une autre version) et
`FORCE=1` (refait tout) règlent ce choix.

Chaque jeu devient un dossier à copier tel quel à la racine de la carte SD :

| Fichier | Contenu |
|---|---|
| `<Jeu>/<Jeu>.bin` | le firmware (c'est ce nom que montre le loader) |
| `<Jeu>/ICON.BMP`, `<Jeu>/TITLESCREEN.BMP` | icône et écran titre du menu, tirés de thumbnails.libretro.com (`NO_ICONS=1` pour s'en passer) |
| `<Jeu>/<NOM>.SAV` | sauvegarde (jeux à pile ou à grande RAM de cartouche) |
| `<Jeu>/<NOM>.GB` | la ROM, pour les ROMs de 256 Ko et plus lues sur la carte |

Le `.elf` de débogage reste dans `out/<jeu>/.pio/build/meta/`.

Le script utilise tous les cœurs :
- 8 parties automatiques en parallèle par ROM (une de 30 min de jeu, sept de
  10 min), soit environ 11 s ;
- plusieurs ROMs à la fois (`JOBS`, par défaut cœurs / 4), chacune avec son
  journal dans `out/<jeu>/compile.log`.

Pour chaque ROM `.gb`/`.gbc` du dossier (sans limite de taille), le
script enchaîne les étapes a) à d) :

- une **partie automatique**, sans fenêtre ni son : touches aléatoires,
  Start de temps en temps ;
- l'**analyse statique** de la ROM (`--meta-static`) complète la couverture.
  Le code est gardé du plus exécuté au moins exécuté jusqu'à remplir la
  flash ;
- chaque partie est cumulée aux précédentes : rejouer un jeu (`PLAY=1`)
  étend le code compilé ;
- une ROM refusée (CGB seulement, mapper non géré) n'arrête pas le lot, et
  le bilan s'affiche à la fin.

### Parties enregistrées et réutilisées

Chaque partie, automatique ou jouée à la main (`PLAY=1`), est sauvée dans
le dossier de profil de la ROM, `profiles/<gb|sg|gg|sms>/<Jeu>/` à la racine
du dépôt, commun à la META, la Pokitto et l'ESPboy : touches (`.input`), code
exécuté (`.cov`) et lectures de données (`.prof`). Les lancements suivants
les réutilisent sans rejouer, et chaque partie à la main s'ajoute aux
précédentes. Garde ce dossier : il vaut toutes tes parties. Les anciens
dossiers `roms/<sys>/profiles/<sha1>/` y sont déplacés au premier passage.

Le même dossier contient :

- `profile.json` : chaque partie (images, instructions, instructions que
  seule cette partie a trouvées), la carte de la ROM (par blocs de 256
  octets : `C` code exécuté, `D` données lues, `B` les deux), les
  instructions les plus exécutées, le dernier build de chaque console
  (version, code compilé, RAM, flash) et les runs gm0 ;
- `play.sh` : `./play.sh` lance une session de jeu dans
  [gm0](https://github.com/r043v/gm0) avec le dernier firmware construit
  (`./play.sh pokitto` ou `espboy` pour une autre console ; `GM0=` si gm0
  n'est pas dans `../gm0/build/bin/`). Chaque session est enregistrée pour
  l'entraînement :
  - la carte est neuve (sauvegarde vierge, pas d'états), pour que la partie
    se rejoue depuis l'allumage ;
  - gm0 note les boutons que reçoit le jeu, avec le numéro d'image du jeu
    (`gm0 --record-input` ; le firmware porte un petit descripteur
    `m0input` qui dit où les lire) ;
  - la session devient `coverage-gm0-<date>.input`, rejouée aussitôt dans
    le harnais PC : son code exécuté entre dans le profil, le `./compile.sh`
    suivant voit le jeu « profil enrichi » et le compile, puis la rejoue
    chaque fois que le profil est refait (`REPLAY=1` : toutes) ;
  - à la sortie, les fonctions du firmware qui ont pris le plus de cycles
    (`EMU_PROF` : des estimations de gm0, pas des mesures sur la console).

  F2 prend un instantané, F4 y revient : gm0 repart à froid et rejoue en
  avance rapide les touches jusque-là, la partie reste rejouable depuis
  l'allumage. `./play.sh reprendre [<date>]` reprend une session à son
  dernier instantané (ou à sa fin) un autre jour. Redémarrer (F5) ou
  recharger un état du menu du firmware coupe en revanche la partie
  rejouable à cet endroit. `./play.sh rejouer [<date>]` rejoue une session
  dans gm0, touche pour touche ; `./play.sh --carte` joue librement sur une
  carte gardée (sauvegardes, états), sans enregistrement. `./play.sh record`
  (Game Boy) ouvre la fenêtre du harnais PC pour une partie à la main.
  `./play.sh report` refait `profile.json`.

### Ce qui reste en flash (Game Gear, Master System, Game Boy)

Une ROM plus grande que la flash laisse des pages sur la carte SD. Ce que
le firmware garde en flash vient d'une simulation de son cache carte sur
toutes les parties enregistrées (`tools/profile/m0profile.py storage`,
lancé par `compile.sh`, `STORAGE=0` pour s'en passer) :

- chaque partie (dix minutes au plus) est rejouée en trace d'accès, dans
  l'ordre, au grain de 16 octets : chaque lecture du jeu, répétitions
  comprises, octets d'instructions interprétées, RAM de cartouche ; le
  harnais du projet généré au build précédent la rejoue avec son propre
  code compilé, donc le trafic de ce firmware (gm0 donne les mêmes lectures
  SD, décodages et blocs chargés) ;
- `m0romsim` rejoue ces traces dans une copie exacte du cache partagé
  (blocs de 64 octets, secteur de 512 en attente, CLOCK, RAM de cartouche)
  et compte le coût de chaque accès : octet du pool en flash, bloc lu en
  dernier, secteur en attente, balayage des voies, défaut (copie), lecture
  SD, décodage (coûts mesurés dans gm0) ;
- les blocs sont choisis par cycles économisés par octet de flash, de 512
  octets à 16 Kio, bruts ou LZSS (Game Boy : blocs de 4 Kio bruts,
  secteurs LZSS) ; la taille minimale est validée en apprenant sans une
  partie et en testant sur elle ;
- les coûts d'une lecture SD et d'un décodage et la taille réelle du cache
  viennent de runs gm0 (`calibration-<console>.json`, plusieurs longueurs
  d'une session : régression ; cycles comptés par gm0, des estimations, pas
  des mesures sur console).

Blocs plus petits, petits blocs compressés ou sans leurs zéros, cache
séparé code/données : comparés dans
`docs/PROFILING_STOCKAGE_2026-10-22.fr.md`, tous moins bons sur Ristar.

`storage.json` (et la section `storage` de `profile.json`) donne la
validation croisée, le plan retenu, la sensibilité au nombre de voies et au
budget. Le générateur reçoit `rom.rank` et arbitre lui-même entre code
compilé et secteurs gardés.

### Sauvegardes

Pour les jeux à pile (sauvegarde) et ceux à grande RAM de cartouche, le
script crée un fichier `.SAV` dans le dossier du jeu (ex. `roms/meta/Mystic Quest/MQ.SAV`).
**Copie-le sur la carte une seule fois**, avec le `.bin`, puis ne l'écrase
plus : la console y écrit la progression.

La console écrit la sauvegarde sur la carte quand le jeu a fini de
sauvegarder, 2 s après la dernière modification, et en revenant au loader
(menu d'options, « Quitter »).

Si le `.SAV` manque :
- le jeu tourne quand même, mais sans garder la progression ;
- pour les jeux à grande RAM (8 Ko et plus, comme Zelda), l'écran reste
  jaune.

### Profils fournis

Des parties déjà enregistrées (adresses du code exécuté, lectures de
données ; aucun contenu de jeu) peuvent être déposées dans `profiles/`, à
l'ancien format, rangées par SHA-1 de la ROM (`profiles/<sha1>/`). Si ton
dump est identique, `compile.sh` les déplace dans le dossier de profil du jeu
et produit le même firmware que celui testé par l'auteur :

| Jeu | SHA-1 |
|---|---|
| Batman - The Video Game (World) | `ef7d6684a0f737c01a1f2cc2c6609e0f3ac1310e` |
| Burai Fighter Deluxe (USA, Europe) | `178e18b7e6e65e726b4e06f80d89c55332ea868b` |
| Dr. Mario (World) (Rev A) | `d31d67d0682515c7c85deaa1752b02231150e5bf` |
| Mystic Quest (World) (Fr) (Collection of Mana) | `d6265d74e0380a1656ad9b14971d5040c4cc20c5` |
| SolarStriker (World) | `a8d6acb026b0d0a8a2bcaea094951e915a3deb80` |
| Super Mario Land (World) (Rev 0) | `3a4ddb39b234a67ffb361ee7abc3d23e0a8b1c89` |
| Super Mario Land (World) (Rev A) | `418203621b887caa090215d97e3f509b79affd3e` |
| Super R.C. Pro-Am (USA, Europe) | `37c0a74532ed283f56e940bb5c38e757a0eee6e0` |
| Tetris (World) (Rev A) | `74591cc9501af93873f9a5d3eb12da12c0723bbc` |
| Tetris 2 (USA, Europe) (SGB Enhanced) | `1f88be9dea864eaa74c3b5ce3488e61139818e8d` |
Vérifie ton dump avec `sha1sum roms/gb/*.gb`. Une autre version (autre
révision, autre région) marche aussi, avec seulement la partie automatique.

### ROMs de 256 Ko et plus : lecture depuis la carte SD

Elles ne tiennent pas dans la flash (240 Ko, runtime compris). Le firmware
garde alors en flash la banque 0 et les données les plus lues pendant les
parties sur PC, et lit le reste sur la carte SD, par secteurs de 512 octets,
avec un petit cache. Le script copie la ROM à côté du binaire sous un nom
court affiché à la fin, par exemple `roms/meta/Mystic Quest/MYSTICQU.GB`.

**Copie sur la carte SD le dossier entier du jeu**, `.bin` et `.GB`
compris, sans renommer le `.GB`.

Si le fichier manque ou ne correspond pas, l'écran reste d'une couleur
unie ; HOME maintenu 3 s revient au loader.

| Couleur | Problème |
|---|---|
| Rouge | fichier introuvable |
| Bleu | carte illisible (FAT16/FAT32 attendu) |
| Orange | fichier trop fragmenté : recopie-le |
| Magenta | ce n'est pas la ROM de ce firmware |
| Jaune | fichier `.SAV` introuvable (jeu à grande RAM de cartouche) |

Pour ces jeux, une partie jouée sur PC (`PLAY=1 ./compile.sh`) aide
beaucoup : elle indique les données à garder en flash, donc moins de
lectures sur la carte en jeu. Les modes d'affichage BLEND, DARK et TBLEND sont
retirés de ces firmwares pour faire de la place en RAM.

Sur 5 jeux (SML, Batman, Burai Fighter, Solar Striker, R.C. Pro-Am), il ne
reste que 1 à 15 % d'instructions interprétées, mesuré avec d'autres entrées
que celles de la partie automatique. Une partie jouée à la main améliore
encore les niveaux que la partie automatique n'atteint pas.

## Compilateur multiplateforme (Node)

`tools/meta-compiler/compile.mjs` fait la même chose que `compile.sh`
(mêmes dossiers, mêmes profils, mêmes firmwares), aussi sous Windows et
macOS, avec une interface plein écran : liste des ROMs à cocher, puis
tableau de bord des jeux en cours.

```bash
cd tools/meta-compiler && npm install && cd ../..   # une fois (Node 22+)
node tools/meta-compiler/compile.mjs                  # comme ./compile.sh
node tools/meta-compiler/compile.mjs --all --no-icons # sans menu
```

Options : `--all`, `--update`, `--force`, `--no-icons`, `--play`, `--pack`
/ `--no-pack` (et les mêmes variables d'environnement que `compile.sh`).
`q` arrête de lancer de nouveaux jeux, Ctrl-C arrête tout. Sans `npm install`
(ou hors terminal), il se contente d'une liste numérotée ou traite tout.

## Émulateur (tester sans la console)

`tools/gb-emu/` est une version de gamebuino-emulator qui fait tourner ces
firmwares : exécution depuis la SRAM et carte SD émulée (voir son
`LISEZMOI.md`). Ouvre `tools/gb-emu/dist/index.html` et choisis un dossier
de `roms/meta/` : il sert de carte SD et son `.bin` est chargé. Pas de son,
et la vitesse n'est pas celle de la console.

Pour la META comme pour la Pokitto, avec le son et une vitesse calée sur
la console, utilise plutôt [gm0](https://github.com/r043v/gm0), l'émulateur
qui a servi au développement : `gm0 <firmware> <dossier du jeu>` (par
exemple `gm0 roms/pokitto/Tetris/Tetris.bin roms/pokitto/Tetris`, le
dossier servant de carte SD ; la console est reconnue d'après le
firmware).

## 3. Mettre le jeu sur la META

- Par USB : `pio run -d out/tetris -t upload`, la META déjà en mode
  bootloader (le firmware n'a pas de pile USB pour répondre à la demande de
  redémarrage de PlatformIO).
- Par carte SD : copie `out/tetris/.pio/build/meta/firmware.bin` sur la carte
  (renomme-le, par exemple `TETRIS.BIN`), puis lance-le depuis le loader.

## Commandes sur la META

| Bouton | Game Boy |
|---|---|
| Croix | Croix |
| A | B |
| B | A |
| MENU | Start |
| HOME (appui court) | Select |
| HOME maintenu ½ s | Menu d'options |

Le menu d'options met le jeu en pause : haut/bas choisit une ligne,
gauche/droite (ou A) change la valeur, B ou HOME referme.

| Ligne | Choix |
|---|---|
| Reprendre | retour au jeu |
| Redemarrer | relance le jeu (la sauvegarde est écrite avant) |
| Etat | emplacement 1 à 10 (« vide » s'il n'a pas d'état) |
| Sauver l'etat / Charger l'etat | sauve ou recharge l'état de l'emplacement choisi |
| Volume | 0 à 8 (4 par défaut) |
| Ecran | mode d'affichage (ci-dessous) |
| Palette | Vert, Gris, Pocket, Ambre, Bleu (SMOOTH reste en vert) |
| Images | saut d'images : Auto (n'affiche pas les images en retard), Aucun, 1 sur 2, 1 sur 3, 1 sur 4 |
| Vitesse | Normale, ou Rapide (sans limite d'images par seconde) |
| Processeur | 70 MHz (par défaut sur la META, surcadencé : hors spécification du SAMD21, mesuré stable sur une console) ou 48 MHz ; la Pokitto : 72 |
| SPI ecran | vitesse d'envoi de l'image à l'écran : 12, 24, 36 ou 48 MHz (24 par défaut, le débit connu de ce panneau ; 36 au plus avec le processeur à 72 MHz ; 36 et 48 viennent de la PLL et n'ont pas été essayés sur une META). Si l'écran devient illisible, le menu l'est aussi : éteindre la console remet le défaut de 24 MHz |
| Quitter | retour au loader Gamebuino |

Sous les lignes : images émulées et affichées pendant la dernière seconde,
ou le résultat de la dernière sauvegarde d'état.

Les états sont dans le fichier `<NOM>.STA` (Game Boy) ou `<NOM>.ST8`
(Game Gear, Master System, SG-1000) que le convertisseur crée à côté du
`.bin` (10 emplacements) : copie-le sur la carte avec le jeu. Sur l'ESPboy,
ils sont dans la flash de la console. Sans lui,
les lignes d'état n'apparaissent pas. Un état ne se recharge qu'avec le
firmware qui l'a écrit (une nouvelle version de gbrecomp le refuse).
Les choix ne sont pas gardés à l'extinction.

Modes d'affichage (les 144 lignes Game Boy sur les 128 de la META) :

| Mode | Méthode |
|---|---|
| SMOOTH | Vraie réduction 144 → 128 : chaque ligne de l'écran mélange deux lignes Game Boy au prorata de leur surface, aucune n'est sautée. Un peu plus coûteux ; une ligne peut garder une image de retard au tout début d'un changement |
| TILE | Retire la dernière ligne de chaque rangée de tuiles (souvent vide dans les polices) : le texte reste net |
| BLEND | Fusionne 2 lignes sur 9 avec des couleurs moyennées |
| DARK | Fusionne en gardant la teinte la plus sombre |
| CROP | Pas de réduction : lignes 8 à 135 (le haut et le bas sont coupés) |
| SKIP | Saute 1 ligne sur 9 |
| TBLEND (défaut) | Comme TILE, mais la dernière ligne de chaque rangée de tuiles est fusionnée (couleurs moyennées) avec la suivante au lieu d'être sautée : 7 lignes sur 8 restent nettes (repris du portage ESPboy de gm0) |

Pour voir l'écran exact de la META sur PC :
`out/<jeu>/host/build/<jeu>_metaview --mode tile --frames 300 --dump 200 ecran.ppm`.

Le menu (HOME maintenu) affiche les images émulées et dessinées de la
dernière seconde. Le firmware n'écrit rien sur le port USB.

## Limites

- Game Boy DMG uniquement. Les ROM réservées à la Game Boy Color sont
  refusées ; `--cgb --target pc` les accepte en mode couleur (palettes,
  banques VRAM/WRAM, GDMA, double vitesse) — la console META n'a pas la
  mémoire pour ce mode et le drapeau la refuse.
- Cartouches acceptées : sans MBC, MBC1, MBC2, MBC3 (avec ou sans horloge) et MBC5.
  L'horloge du MBC3 (types 0F/10) tourne avec l'horloge de la plateforme quand
  elle en a une, sinon une seconde par 60 trames ; elle est persistée dans le
  .SAV (secteur dédié de 512 octets derrière la RAM). La phase sous la seconde
  n'est pas modélisée (le tick tombe aux frontières de trame sur l'hôte).
- La ROM est copiée dans la flash, ce qui laisse environ 210 Ko pour la ROM
  et le code compilé. Jusqu'à 128 Ko de ROM, ça passe. 256 Ko ne rentrent
  pas.
- La RAM de la cartouche occupe la RAM de la META (32 Ko au total). Une
  cartouche avec 8 Ko de RAM est très juste. Pas de sauvegarde sur la SD
  pour l'instant.

## Options de compilation

Ajoute-les dans `build_flags` du `platformio.ini` généré, par exemple
`build_flags = -O2 -Wno-unused-label -DMETA_LCD_BUFFERS=2`.

| Define | Défaut | Rôle |
|---|---|---|
| `GBL_PPU_BATCH` | 8 | Lignes rendues par paquet quand aucune interruption par ligne n'est active |
| `GBL_LINE_SIGNATURE` / `META_LINE_SKIP` | 1 / 1 | Ne redessine ni ne renvoie les lignes inchangées (exact) |
| `GBL_FRAME_SIGNATURE` / `META_STATIC_SKIP` | 1 / 1 | Saute les frames identiques ; un changement s'affiche avec une frame de retard |
| `GBL_PPU_PAL32` | 0 | Table de 4 pixels (2 Ko de RAM), sans gain mesuré |
| `GBL_GEN_OPTIMIZE_SIZE` | 1 | Code généré en `-Os` |
| `GBL_SPIN` | 1 | Avance d'un coup le temps des boucles d'attente (LY/STAT, drapeau en RAM, temporisations) ; résultat identique, 0 pour vérifier |
| `META_AUDIO_DMA` | 0 | Chemin de sortie choisi à la **compilation** depuis 0.10.29 (la bascule en jeu de 0.10.28 et la ligne Audio des menus sont retirées : 0,5 % de processeur mesuré sur Pokémon Jaune, ça ne vaut pas le risque DMAC) : 1 le son va au DAC par DMA au lieu de l'interruption à 22 kHz. Les deux chemins partagent depuis 0.10.28 un seul anneau (`META_AUDIO_RING`, 512 échantillons de 2 octets). `compile.sh` : `AUDIO_DMA=1` |
| `META_LCD_RUNS` | 1 | Fenêtre de l'écran posée une fois par suite de lignes (au lieu d'une par bande) : Ristar −4,6 % de temps par image ; 0 pour l'ancien comportement |
| `SG_BOUNDED_SYNC` | 0 | SG-1000 / Game Gear / Master System : le code natif regarde les interruptions au plus tous les N cycles Z80 (`SG_SYNC_CYCLES` dans `compile.sh`, `--meta-sync-cycles`). Device 2026-10-20, banc attract de Ristar, 2370 frames : 40 cycles 22,15 ms/frame, 161 images de plus de 25 ms ; 0 (têtes de boucles seulement) 21,61 (−2,4 %), 77 ; 120 cycles 21,70, 91 — et bornait la latence de l'eau de la démo de Ristar (ligne prise 185 T-states en retard, 12 lignes noires) ; `SG_SYNC_CYCLES=40` le rend |
| `GBL_INTERP_DELAY_LOOP` | 1 | Raccourci de l'interpréteur pour l'attente DMA `DEC A / JR NZ` en HRAM |
| `GBL_PPU_ASM` | 1 sur M0+ | Chargement des tuiles et conversion des couleurs en assembleur Thumb-1 (`gbl_ppu_asm.S`) |
| `GBL_PPU_RAMFUNC` | 0 (META), 1 (Pokitto) | Ces boucles tournent depuis la SRAM, sans état d'attente de la flash ; coupé sur la META, où le code en SRAM plantait processeur à 70/72 MHz (≈1 % de processeur en plus à 48 MHz) |
| `META_LCD_DMA` | 1 | Écran en DMA avec file de transferts ; 0 = envoi par le CPU |
| `META_LCD_SPI_MHZ` | 48 | Débit SPI de l'écran au démarrage : 12, 24, 36 ou 48 (48 par défaut, le maximum, vu correct sur une META ; ramené à 35 quand le processeur est à 70 MHz, à 24 si la PLL ne s'accroche pas) ; le menu « SPI ecran » le change |
| `META_OVERCLOCK` | 1 (META) | Démarrage à 70 MHz (META) ; 0 : 48 MHz.  compile.sh : `OVERCLOCK=0` |
| `META_SD_SPI_HZ` | 12000000 | Débit SPI de la carte SD hors identification (24 MHz : lectures en échec sur une META) |
| `META_USB_MODE` | 3 | 0 aucun USB, 1 serie, 2 carte SD, 3 serie et carte SD. Le menu « Carte SD USB » arrete le jeu, vide les sauvegardes et expose la carte comme lecteur USB. Une demande d’ejection du PC ou le debranchement USB ramene au jeu ; MENU+HOME maintenus 0,5 s quittent aussi le mode apres demontage cote PC. |
| `META_USB` | derive | Ancien interrupteur 0/1, utilise seulement si `META_USB_MODE` n’est pas defini ; correspond aux modes 0/3. Le port serie permet le televersement a 1200 bauds et `tools/meta-usb.py`. |
| `META_LCD_BUFFERS` / `META_STRIP_LINES` | 3 / 4 | Buffers d'écran et lignes par buffer |
| `META_MAX_SKIP` | 3 | Frames consécutives sautées au maximum en cas de retard |
| `META_BUTTONS_LAPINOU_ORDER` | off | Ordre des bits de meta-lapinou au lieu de celui de la bibliothèque (inutile sur la META testée) |

## Vérifier sur PC

```bash
# Référence (interpréteur) contre le code compilé, avec les mêmes entrées
./out/tetris/host/build/tetris_host --interp --frames 3000 --input out/tetris.input
./out/tetris/host/build/tetris_host --frames 3000 --input out/tetris.input
```

Autres options utiles : `--dump-frame N image.ppm`, `--wav son.wav`,
`--hash-every 1`, `--static-skip-check`.

Tests de l'outil :

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON && ninja -C build
ctest --test-dir build --output-on-failure
```

Le fonctionnement détaillé, les mesures et les limites techniques sont dans
`targets/meta/README.md`.
