# m0boy

**Des jeux Game Boy, SG-1000, Game Gear, Master System, MSX, NES, PICO-8,
Vectrex, PC Engine, Mega Drive et SNES compilés en firmwares natifs pour de
petites consoles (Gamebuino META, Pokitto, ESPboy, et plus récemment Gamebuino
AKA et ODROID-GO) et en programmes SDL2 pour le PC.**

*English version: [README.md](README.md).*

m0boy n'est pas un émulateur. Il traduit le code du jeu lui-même en C,
compile ce C pour le processeur de la console et le lie à un petit runtime qui
remplace le matériel d'origine (vidéo, son, timers, mappers). Le code que le
recompilateur n'a pas pu prouver ou n'a jamais vu tourner passe par un petit
interpréteur : tout jeu démarre, même quand une partie seulement est compilée.

Une conversion donne un dossier à copier sur la carte SD de la console (sur
l'ESPboy, une image à flasher).

| | Gamebuino META | Pokitto | ESPboy |
|---|---|---|---|
| Puce | SAMD21, Cortex-M0+ prévu pour 48 MHz, à 70 par défaut | LPC11U68, Cortex-M0+ prévu pour 50 MHz, à 72 par défaut | ESP8266, Xtensa 80/160 MHz |
| Mémoire | 32 Kio de RAM, 240 Kio de flash pour le jeu | 36 Kio de RAM, 228 Kio de flash | 80 Kio de RAM, 4 Mio de flash |
| Écran | 160×128 | 220×176 | 128×128 |
| Stockage | carte SD | carte SD | flash seulement |
| Version du runtime | 0.10.56 | 0.10.40 | 0.3.11 |

Cibles plus récentes, bâties sur le cœur ESP32 commun
`targets/common/consoles/esp32/` (PlatformIO), et la fenêtre PC :

| | Gamebuino AKA | ODROID-GO | PC |
|---|---|---|---|
| Puce | ESP32-S3, PSRAM octale, LCD i80, SDMMC | ESP32-WROVER | fenêtre SDL2 |
| Mémoire | 8 Mio de PSRAM, slot de jeu de 3000 Kio | 320 Kio de RAM, 4 Mio de PSRAM | celle de l'hôte |
| Écran | 320×240 | 320×240 | la résolution native de la source |
| Stockage | carte SD | carte SD | la carte en fichier `sd.img` |
| Version du runtime | 0.3.8 | 0.1.0 | 0.2.4 |

Le câblage de l'AKA suit la Gamebuino_AKA_lib officielle (`docs/AKA.fr.md`) ;
on peut l'essayer sans matériel dans `tools/AKA_Emulator` (un vrai émulateur
d'ESP32-S3) et dans gm0 (machine d'application expérimentale). L'ODROID-GO
n'est pas émulé : ses vérifications sur l'appareil restent manuelles. Quel
système peut générer pour quelle console vient de la matrice de capacités
(`gbrecomp --list-systems`, `--list-targets`).

## Ce qui tourne où

| Source | META | Pokitto | ESPboy |
|---|---|---|---|
| Game Boy (DMG) | oui | oui | oui |
| SG-1000 | oui | oui | oui |
| Game Gear | oui | oui | oui |
| Master System | oui | oui | oui |
| MSX1 (mappers ASCII8/16, Konami et Konami SCC ; AY-3-8910 et SCC ; expérimental) | oui | oui | — |
| PICO-8 | oui | oui | oui |
| NES (NROM : PRG 16/32 Kio, CHR ROM 8 Kio ; expérimental) | oui | oui | — |
| PC Engine (HuCards, interpréteur ; expérimental, PC seulement) | — | — | — |
| Vectrex (cartouches jusqu'à 32 Kio, ROM système intégrée ; expérimental) | oui | — | — |
| Mega Drive (profil M68k borné ; expérimental) | oui | — | — |

Matrice de capacités des cibles récentes (d'après `gbrecomp --list-systems` ;
expérimental là où les lignes ci-dessus le disent) :

| Source | PC | ODROID-GO | AKA |
|---|---|---|---|
| Game Boy, SG-1000, Game Gear, Master System | oui | oui | oui |
| MSX1, NES | oui | — | — |
| PICO-8, Vectrex, Mega Drive, PC Engine | oui | — | oui (ODROID : non) |
| SNES (expérimental, harnais et fenêtre PC seulement) | oui | — | — |
| Source M0+ (un firmware META ou Pokitto, `--system meta`) | oui | — | oui (META seulement) |

SNES (`--system snes`) : le 65816 est recompilé en C avec une marche de
contextes, un PPU/HDMA/IRQ par ligne, un interpréteur SPC700 et un DSP BRR ;
DSP-1, SuperFX et SA-1 en cours. L'état, les preuves et ce qui reste, avec
leurs étiquettes, sont dans `docs/SNES_STATUS_2026-10-31.md` et
`docs/SNES.fr.md`.

La cible PC (`--target pc`) ajoute une fenêtre SDL2 pour Game Boy, SG-1000,
Game Gear, Master System, MSX, NES, PICO-8, PC Engine, Vectrex, Mega Drive et SNES (expérimental).
Les projets partagent les manettes, filtres d'image (F8), échelle (F10) et
plein écran (F11). Le NES PC affiche l'image 160×128 du panneau META ; les
autres cœurs gardent leur géométrie propre.

Enregistrement des entrées dans les fenêtres PC (NES, PC Engine, PICO-8) :
chaque cœur garde le dialecte que son harnais lit avec `--input`. NES et PC
Engine écrivent des lignes « trame boutons » (noms joints par `+`, une ligne
par trame où le jeu reçoit des boutons — les scripts du harnais sont
momentanés, tenus le temps de leur seule trame) ; la fenêtre PICO-8 écrit des
segments `début:masque:durée`. NES et PICO-8 lisent les variables
d'environnement (`M0_RECORD_INPUT=fichier` enregistre, `M0_INPUT=fichier`
rejoue un script dans la fenêtre) ; la fenêtre PC Engine prend
`--record-input fichier` et `--input fichier` en ligne de commande. La
fermeture de la fenêtre, la limite de trames (`M0_FRAMES`/`--frames`) et
l'anti-blocage du démarrage ferment le fichier ; un redémarrage ou un état
sauvegardé du menu y met fin (le harnais repart de la mise sous tension, ce
qui suit ne pourrait pas se rejouer). Le harnais rejoue tel quel : NES et
PC Engine avec `--input fichier`, PICO-8 avec `--input-file fichier`.

MSX1 est vérifié contre openMSX (VRAM des écrans titres) et contre le rendu
de référence de l'hôte ; Gradius 2 (128 Kio, Konami SCC) a tenu 60 images de
jeu par seconde sur une META lors d'un essai de 90 s (compteur d'images du
firmware ; images affichées non mesurées). La passe 0.10.41 sur le rendu et
l'audio saute les mélanges des tuiles et paires vides et les copies
d'échantillon une à une : sur 3 600 images gm0 de Gradius 2, 3,4 s de moins
dans les routines de tuiles et 0,8 s de moins dans la génération audio
(estimations), l'écran et la RAM du jeu restant identiques à la build
précédente à la même image. Notes : `docs/MSX.fr.md`.

Vectrex (expérimental) : le 6809 est recompilé en C devant l'interpréteur,
la VIA (en Thumb-1 sur la META) et l'AY-3-8912 sont émulés, le faisceau est
tracé en 128×128 ; la ROM système est intégrée au firmware généré
(`--bios`), la cible PC la lit au lancement. Sur une META, la démo de
Berzerk tourne à 36 images par seconde sur 50 (mesuré ; 19 avec
l'interpréteur seul). Le projet `--meta-precise` donne le même run_hash que
`--interp` sur les 231 cartouches de la collection locale (2 000 trames).
Notes : `docs/VECTREX.fr.md`.

Les cœurs sur console partagent un menu d'options (HOME maintenu ; C sur la Pokitto) :
reprendre, redémarrer, volume, vitesse,
fréquence du processeur, retour au loader, plus les lignes propres à chaque
système (modes d'affichage et palettes sur Game Boy, bootloader USB sur la
META). Les emplacements d'état apparaissent pour les cœurs qui les prennent
en charge.

| Fonction | META | Pokitto | ESPboy |
|---|---|---|---|
| États sauvegardés | GB, SG, GG, SMS, MSX, PICO-8, NES, Vectrex : fichier sur la carte | GB, SG, GG, SMS, MSX, PICO-8, NES : fichier sur la carte | GB, SG, GG, SMS, PICO-8 : en flash |
| Sauvegardes sur pile | `.SAV` sur la carte | `.SAV` sur la carte | en flash, deux copies alternées |
| ROM plus grande que la flash | lue sur la carte, secteurs chauds gardés en flash | lue sur la carte | `rom.bin` dans les 4 Mio de flash |
| Secteurs de ROM compressés (LZSS) | oui | oui | oui |
| Cache ROM partageant la RAM avec la pile C | oui | Game Boy | — (pas de carte) |

Ce qui a été vérifié, et comment, est écrit à côté de chaque affirmation
ci-dessous et dans les notes liées : **vérifié** veut dire comparaison ou test
automatique, **gm0** mesuré dans l'émulateur [gm0](https://github.com/r043v/gm0),
**console** mesuré sur une console, **estimé** le modèle de cycles du
Cortex-M0+ (`targets/meta/tools/m0bench`).

## Outils

- `tools/meta-usb.py` : flasher un firmware en USB et lire la ligne série de
  la console (sous-commande `monitor` : un terminal printf pour toute console
  série, avec reconnexion ; `--baud`, `--prefix` des binutils de la cible ;
  Windows, Linux et macOS). SAM-BA n'est qu'un protocole sur une liaison
  USB-série ordinaire.
- `M0_AKA_PERF` (désactivé par défaut, sans effet sur le firmware) : le cœur
  M0+ natif de l'AKA écrit des temps `[perf]` (émulation / son / écran) sur la
  ligne série. C'est une chaîne de debug, pas un profileur.
- `tools/profile/` (profils par jeu, recherche de parties `m0search.py`),
  `tools/snes_sweep.py`, `tools/AKA_Emulator`, `tools/megadrive/`.

## Démarrer

Il faut CMake 3.20+, Ninja, un compilateur C/C++ pour le PC, Python 3, les
fichiers de développement de zlib et [PlatformIO](https://platformio.org/)
pour la META et l'ESPboy. La Pokitto n'a besoin que de `arm-none-eabi-gcc`
(celui du `PATH`, sinon celui de PlatformIO). Le PICO-8 demande un dépôt
[femto8](https://github.com/benbaker76/femto8) à côté de celui-ci
(`FEMTO8_DIR`, par défaut `../femto8`).

```bash
cmake -G Ninja -B build .
ninja -C build

# ROMs dans roms/gb/, roms/sg/, roms/gg/, roms/sms/, roms/pico8/ puis :
./compile.sh                    # demande la console (META par défaut)
TARGET=pokitto ./compile.sh
TARGET=espboy ./compile.sh
TARGET=both ./compile.sh        # META et Pokitto
```

Chaque jeu arrive dans `roms/<console>/<système>/<Jeu>.zip`. Décompresse-le à
la racine de la carte SD et lance-le depuis le loader de la console ; garde
les fichiers `.SAV`, `.STA` ou `.ST8` déjà sur la carte, ils contiennent ta
progression. Sur l'ESPboy, le zip contient `firmware.bin` (et `rom.bin`
au-delà de 256 Kio) et la ligne `esptool.py` dans `FLASH.txt`.

Options utiles de `compile.sh` :

| Option | Effet |
|---|---|
| `RELEASE=1` | firmwares sans le port USB de débogage (plus de RAM ; reflasher par le bootloader) |
| `UPDATE=1`, `FORCE=1` | refaire les jeux d'une autre version / tout refaire |
| `SOURCE=1` | livrer le projet généré, prêt pour `pio run`, au lieu du firmware |
| `SRC_SYSTEM=gb\|sg\|gg\|sms\|pico8` | un seul système |
| `PACK=0` | pas de secteurs de ROM compressés (Game Boy) |
| `CACHE_DYN=0` | cache carte de taille fixe au lieu du cache adaptatif |
| `FIRMWARE_COMPILER=clang` | firmwares compilés par Clang |
| `REPLAY=1`, `PLAY=1` | rejouer tes parties enregistrées / en jouer une avant de compiler |

Le guide complet : [docs/GUIDE.fr.md](docs/GUIDE.fr.md). Les notes
techniques (en anglais) : [targets/meta/README.md](targets/meta/README.md).

Tests :

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

## Comment ça marche

### Une compilation guidée par le jeu

Avant de compiler un jeu, `compile.sh` y joue tout seul sur le PC : parties
au hasard, une partie sans touche (logos, démo) et des explorations guidées
par la couverture, qui gardent l'état de chaque moment où du code ou des
données nouveaux sont apparus et repartent de là. Les parties notent quel
code a tourné, combien de fois, et quels secteurs de la ROM ont été lus. Elles
sont gardées par ROM, dans un dossier commun à toutes les consoles cibles
(`profiles/<gb|sg|gg|sms>/<Jeu>/`) : une conversion suivante les réutilise,
et une partie jouée à la main les complète. Chaque dossier reçoit aussi
`profile.json` (parties, carte du code et des données de la ROM, dernier
build de chaque console, profils de cycles gm0) et `play.sh`, qui lance le
firmware construit dans [gm0](https://github.com/r043v/gm0). Chaque session
gm0 est enregistrée : gm0 note les boutons que reçoit le jeu émulé, image
par image, la session est rejouée dans le harnais PC, et son code rejoint le
profil pour la conversion suivante ; gm0 donne aussi un profil de cycles par
fonction (estimations de l'émulateur).

Un balayage statique les complète. Il classe chaque octet de la ROM
(remplissage, motifs répétés, tables de pointeurs), puis essaie des suites
d'instructions spéculatives ; une suite ne compte comme code que si elle
atteint une fin inconditionnelle sans opcode illégal ni accès à un matériel
inexistant. La flash se remplit ensuite de code compilé, le plus exécuté
d'abord, puis le code balayé que les parties n'ont pas atteint.

Le C généré garde la sémantique du processeur d'origine : les drapeaux ne
sont calculés que là où une instruction suivante les lit, les cycles sont
ajoutés par suite d'instructions, les boucles d'attente (lecture d'un
registre ou d'un drapeau d'interruption) sautent des itérations entières
jusqu'au prochain événement. Une compilation précise (`--meta-precise`)
synchronise le matériel avant chaque instruction et donne **vérifié** le même
résultat que l'interpréteur : Super Mario Land sur Game Boy, les 68 cartouches
SG-1000 et les 225 cartouches Game Gear d'une collection de test.

### Toutes les tailles de ROM

- Une ROM qui tient reste en flash à côté du runtime et du code compilé.
- Sinon, les secteurs de 512 octets les moins lus sont compressés en LZSS dans
  la flash et décodés à la demande par un décodeur Thumb-1 ; les plus lus
  restent bruts.
- Si c'est encore trop gros, la ROM est lue sur la carte SD à travers un petit
  cache, les secteurs les plus lus pendant les parties gardés en flash. Pas de
  limite de taille : Link's Awakening, Pokémon Jaune et Donkey Kong Land
  passent ainsi.
- Une RAM de cartouche plus grande que ce que la console peut céder (8 Kio,
  32 Kio) vit dans un fichier `.SAV` sur la carte derrière un cache à
  écriture différée.

Sur la META, le cache carte partage la RAM libre avec la pile C pendant le
jeu : une part fixe (celle du menu) plus des blocs pris dans la RAM sous la
pile. Un appel natif refusé faute de pile rend un bloc tout de suite ; un bloc
n'est repris qu'après plusieurs secondes sans pression et avec de la marge.
En blocs de 64 octets, la démo de Super Mario Land 2 a fait 13 326 images en
150 s contre 10 985 avec l'ancien cache fixe en blocs de 128 (**console**),
et les lectures de carte de Ristar sont passées de 9 001 à 4 131
(**console**).

Ce qui va en flash, brut ou compressé, est choisi d'après le coût de chaque
accès, pas seulement d'après la fréquence de lecture d'un secteur : le PC
rejoue chaque partie enregistrée dans un modèle du cache de la console
(`tools/profile/m0romsim`, ROM et RAM de cartouche, code et données, chaque
accès compté) avec des coûts mesurés dans gm0 (un secteur de carte environ
17 250 cycles, un décodage LZSS environ 11 400 ; estimations). Les copies de
blocs du cache sont en Thumb-1 déroulé à taille fixe (16 à 512 octets, tous
les alignements de source). Ristar sur la META, les mêmes 5 500 images
rejouées sans rythme : 133,37 s → 130,82 s, images de plus de 25 ms 1 657 →
1 149, décodages LZSS 2 119 → 21 (**console**, 0.10.34 → 0.10.36). Sur cette
partie, le plan simulé ne fait pas mieux que le classement des secteurs par
nombre de lectures (130,84 s) : le gain vient du chemin d'accès et des coûts
mesurés. Détails : `docs/PROFILING_STOCKAGE_2026-10-22.fr.md`.

### Runtime

- Un seul compteur de cycles pilote tout ; la vidéo dessine une ligne quand le
  programme peut l'observer, timers et interruptions ne lèvent un événement
  que lorsqu'il devient visible.
- Les chemins chauds sont en assembleur Thumb-1 : la machine à états des
  lignes du PPU Game Boy et son rendu, les accès mémoire, l'interpréteur, le
  cache de dispatch, le fond du Game Gear, les boucles de tuiles du SG-1000,
  le décodeur LZSS.
- Le Cortex-M0+ n'a pas de diviseur : les divisions ont été sorties des
  chemins chauds.
- Les firmwares META et Pokitto sont bare metal (ni cœur Arduino, ni
  PokittoLib) ; l'ESPboy démarre sur le cœur Arduino et pilote le matériel
  lui-même.

### Game Boy

Jeux DMG, mappers aucun, MBC1, MBC2, MBC3 (sans horloge) et MBC5. Sur la META
les 144 lignes sont fondues en 128 (plusieurs modes, celui par défaut garde
le texte lisible) ; sur la Pokitto l'image est en 1:1 ou agrandie en 200×176.
Les palettes de démarrage de la Game Boy Color sont intégrées (une direction
maintenue au démarrage, comme sur une GBC).

### SG-1000, Game Gear, Master System

Le recompilateur Z80 gère le code paginé (une instruction est une page et une
adresse ; le code rend la main au dispatcher quand le mapper déplace sa page),
mappers Sega et Codemasters, et celui de Super Game 150 à six fenêtres de
8 Kio. Ce dernier utilise le dispatch natif précis ; ses anciens profils
faits avec le mapper Sega sont refaits. [Détails et validation](docs/KOREAN150.fr.md).
Le TMS9918A et le VDP mode 4 sont dessinés ligne
par ligne quand le programme y touche ; les images SG-1000 et Master System
sont filtrées de 8 en 5 sur les petits écrans. Le cœur Z80 passe ZEXDOC et 66
des 67 tests ZEXALL (**vérifié**). Dans gm0 à 48 MHz, 67 des 68 jeux SG-1000
tournent à pleine vitesse sur la META et la Pokitto, et Alex Kidd in Miracle
World, Shinobi et Wonder Boy III à 97–99 % (**gm0**). Pas de son FM.

### PICO-8

Le Lua d'une cartouche est compilé en C, une fonction par fonction Lua, sur un
runtime à tas avec ramasse-miettes, dessin et son en entiers. Côté PC, le
lecteur de cartouches et le compilateur Lua de femto8. L'image du code généré
est **vérifiée** égale à celle de femto8, octet pour octet, sur une cartouche
synthétique (aussi avec une collecte à chaque allocation, et à travers un état
sauvegardé) et sur Celeste et son Practice Mod. Les états contiennent les
pointeurs absolus du tas : un état ne se recharge que dans le firmware qui l'a
écrit, reconnu par une empreinte de son code. Notes :
[docs/PICO8.fr.md](docs/PICO8.fr.md).

```bash
build/bin/m0recomp celeste.p8.png --system pico8 --target meta -o out/celeste
pio run -d out/celeste
build/bin/m0recomp celeste.p8.png --system pico8 --target pokitto -o out/celeste-pokitto
cmake -G Ninja -S out/celeste-pokitto -B out/celeste-pokitto/build -DCMAKE_TOOLCHAIN_FILE=out/celeste-pokitto/arm-toolchain.cmake && ninja -C out/celeste-pokitto/build
```

### ESPboy

ESP8266 (ESP-12F, 4 Mio de flash), écran 128×128, pas de carte SD. Une ROM
jusqu'à 256 Kio fait partie du firmware ; une plus grande y garde sa première
banque et le reste va dans `rom.bin`, flashé en `0x100000`. Sauvegardes sur
pile (deux copies alternées) et états vivent dans la flash à partir de
`0x3C0000`. L'émulation tourne sur une pile de 12 Kio en RAM, qui rend ce
qu'il faut au tas pour garder 4 Kio libres. Vérifié par des tests sur le PC
(flash simulée comprise), des compilations Xtensa et gm0 ; la version actuelle
n'a pas été validée sur une ESPboy. État :
[docs/ESPBOY_STATUS_2026-10-19.md](docs/ESPBOY_STATUS_2026-10-19.md).

## Sans console

[gm0](https://github.com/r043v/gm0) fait tourner ces firmwares : la META et la
Pokitto instruction par instruction, le code Xtensa de l'ESPboy lui-même avec
sa ROM et son SDK émulés par fonction. `gm0 Jeu.zip` prend un jeu converti
comme carte SD ; `EMU_INPUT` scripte les boutons (`image:touches:durée,...`).

## Limites

- Les jeux Game Boy Color seulement sont refusés ; les fonctions CGB ne sont
  pas émulées.
- Temps Game Boy simplifié : le mode 3 dure toujours 172 points, la DMA OAM
  est instantanée, le son est quantifié à l'image, pas de câble link.
- Le code que les parties n'ont jamais atteint tourne dans l'interpréteur,
  bien plus lent : une partie enregistrée à travers ce passage le corrige.
- SG-1000 : cartouches non paginées jusqu'à 48 Kio, NTSC, pas de clavier
  SC-3000. Master System : pas de son FM. ESPboy : RAM de cartouche Game Gear
  de 8 Kio.
- PICO-8 : la cartouche doit tenir dans le tas (18 Kio sur la META et la
  Pokitto, 22 Kio sur l'ESPboy).
- Des images sont sautées quand la console prend du retard.
- Les chiffres marqués **estimé** ou **gm0** ne sont pas des mesures sur
  console.

## Organisation du dépôt

| Chemin | Contenu |
|---|---|
| `recompiler/` | `m0recomp` (aussi `gbrecomp`) : backend Game Boy, recompilateur Z80 (`src/system/sg1000/`), front-end PICO-8 (`src/system/pico8/`), plan de stockage des ROM |
| `targets/meta/runtime/` | le runtime Game Boy commun aux trois consoles ; réglages par console dans `gbl_profile.h` |
| `targets/common/platform/` | le frontend commun à toutes les consoles (`m0_*` : menu, sauvegardes, états, carte SD, FAT, touches) |
| `targets/meta/platform/` | le frontend Game Boy (`gb_main.cpp`, sauvegardes, streaming, modes d'écran) |
| `targets/consoles/` | pilotes bare-metal et gabarits de projet de chaque console |
| `targets/sg1000/`, `targets/gamegear/` | runtimes SG-1000 et Game Gear/Master System, frontends, harnais |
| `targets/pico8/` | runtime PICO-8, frontend, harnais |
| `targets/common/` | codecs de ROM, cache carte, partage pile/cache, explorateur, morceaux du frontend Sega |
| `targets/meta/host/`, `targets/meta/tools/` | harnais, estimateur de cycles |
| `compile.sh`, `tools/meta-compiler/` | conversion par lots (shell, Node) |
| `tests/`, `targets/meta/tests/` | tests |
| `targets/common/consoles/esp32/`, `targets/consoles/{aka,odroid,pc}/` | cœur ESP32 commun et pilotes AKA, ODROID-GO et PC |
| `targets/{nes,pce,vectrex,megadrive,snes,msx,m0plus}/` | runtimes et harnais des autres cœurs |
| `tools/` | `meta-usb.py`, profils et recherche de parties, émulateur AKA, balayages |
| `docs/` | guide, notes de développement |

## Écrit par des agents IA

Aucune ligne de ce que m0boy ajoute à son point de départ n'a été écrite à la
main. Le code, les tests, les pilotes et la documentation ont été écrits par
des agents IA guidés par r043v, qui a testé sur les consoles et fait les
choix. Claude (Claude Code) en a écrit l'essentiel ; GLM (ZCode) a écrit le
convertisseur web, les palettes GBC, des correctifs Pokitto et l'émulateur
gm0 ; DeepSeek (OpenCode) a travaillé sur les bus et regroupé le code commun
des backends Sega. Chiffres des dix premiers jours (29 septembre – 9 octobre
2026), d'après les transcriptions et l'historique git : Claude, 67 commits et
42 375 lignes ajoutées (95 %) ; GLM, 26 commits et 2 207 lignes (5 %).

m0boy est parti d'un portage de
[gb-recompiled](https://github.com/arcanite24/gb-recompiled) d'arcanite24, un
recompilateur Game Boy pour ordinateurs, dont il garde l'historique (104
commits). Le runtime de bureau, les portages et les dossiers de test qui
contenaient des ROM commerciales converties ont été retirés ; l'émulateur
écrit en même temps que m0boy est devenu [gm0](https://github.com/r043v/gm0).

## Contributeurs

- **r043v** : auteur et mainteneur ; teste sur les consoles et fait les choix.
- **arcanite24** : [gb-recompiled](https://github.com/arcanite24/gb-recompiled),
  le point de départ.
- **Jean-Charles LEBEAU (Jicehel)** : le travail sur la Gamebuino AKA. Son
  [fork](https://github.com/Jicehel-Aka/m0boy) et sa pull request n° 2 ont
  apporté les temps `[perf]` du cœur M0+ de l'AKA (maintenant derrière
  `M0_AKA_PERF`), les correctifs Mega Drive (version lue dans l'en-tête, SP/PC
  sur 24 bits) et l'idée d'une console série AKA, d'où le `monitor`
  multiplateforme de `tools/meta-usb.py`.
- Autres auteurs de l'historique : Brandon G. Neri, HDR, Majora,
  RedReaper1515, squach90.

## ROM et droits

Aucune ROM ni aucun firmware compilé ne sont fournis, et aucun ne doit être
ajouté à ce dépôt. Ne convertis que des jeux que tu possèdes, ou des homebrews
dont la licence le permet.

## Licence

MIT, voir [LICENSE](LICENSE). Basé sur
[gb-recompiled](https://github.com/arcanite24/gb-recompiled), © 2026
arcanite24, licence MIT. `tools/gb-emu` garde sa propre licence MIT (© 2017
Andy O'Neill).
