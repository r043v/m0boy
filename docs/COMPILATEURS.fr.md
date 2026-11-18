# Choisir GCC ou Clang pour les firmwares

GCC reste le choix par défaut des firmwares. Clang peut remplacer le compilateur C/C++
pour la Gamebuino META et la Pokitto, sans changer le fonctionnement de
`m0recomp`, de la couverture ou du runtime.

## Build CMake natif

Sur un nouveau répertoire de build, CMake choisit Clang si `clang` et
`clang++` sont disponibles, sinon GCC et G++. Les projets hôte générés,
qui ne compilent que du C, demandent seulement `clang`, sinon `gcc`.

```bash
cmake -G Ninja -S . -B output/build-clang
cmake -G Ninja -S . -B output/build-gcc -DM0BOY_PREFER_CLANG=OFF
```

`CC`, `CXX`, `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER` et les toolchains
explicites sont prioritaires. Un build déjà configuré garde son compilateur ;
utiliser un nouveau répertoire pour changer. L'option
`M0BOY_PREFER_CLANG=OFF` privilégie GCC ; si aucune toolchain native complète
n'est trouvée, CMake conserve sa détection de plateforme habituelle.
Ce choix natif est indépendant du compilateur des firmwares ARM.

## Projet individuel

```bash
./build/bin/m0recomp jeu.gb --target meta --coverage jeu.cov \
    --meta-compiler clang -o output/jeu
pio run -d output/jeu

./build/bin/m0recomp jeu.gb --target pokitto --coverage jeu.cov \
    --meta-compiler clang -o output/jeu_pokitto
make -C output/jeu_pokitto
```

La sélection est enregistrée dans le projet généré. `--meta-thumb` est une
option indépendante : elle n'est pas nécessaire pour compiler avec Clang.

Pour changer le compilateur d'un projet existant :

```bash
FIRMWARE_COMPILER=clang pio run -d output/jeu
make -C output/jeu_pokitto COMPILER=clang
```

La Pokitto sépare ses objets : `build/` pour GCC, `build-clang/` pour Clang.
PlatformIO reconstruit les objets dont la commande de compilation change.
Les deux variantes Pokitto produisent `firmware.bin` dans leur répertoire.

## Compilation par lots

```bash
FIRMWARE_COMPILER=clang ./compile.sh dossier_roms dossier_sortie
FIRMWARE_COMPILER=clang TARGET=pokitto ./compile.sh dossier_roms dossier_sortie
node tools/meta-compiler/compile.mjs dossier_roms dossier_sortie --compiler clang
```

Le compilateur est enregistré dans `.gbrmeta.json`. Un changement entre GCC
et Clang déclenche une reconstruction, même si la ROM et la version sont
identiques. Un ancien état sans ce champ est considéré comme produit par GCC.
La version Node conserve son périmètre META existant.

## SG-1000

Les projets SG-1000 suivent le même choix : `compile.sh` transmet
`FIRMWARE_COMPILER` à `m0recomp --meta-compiler`. META passe par PlatformIO
comme le Game Boy ; Pokitto par CMake, où `-DSG_COMPILER=clang` place
`toolchain/arm_clang.py --launcher` devant le compilateur C/C++ (objets dans
`build-clang/`).

## Prérequis et composants conservés

Clang nécessite `clang` et `clang++` dans PATH. Les variables `CLANG` et
`CLANGXX` permettent de choisir des exécutables précis. La toolchain GNU ARM
reste nécessaire : elle fournit les en-têtes embarqués, newlib-nano,
libstdc++, libgcc, l'assembleur, le linker, `objcopy` et `size`.

`arm_clang.py` cible explicitement `arm-none-eabi`, avec le sysroot de la
toolchain GNU sélectionnée. Pour C++, les chemins d'en-têtes sont ceux du
compilateur ARM, jamais ceux de la machine hôte. Les sources `.S`, le script
de liens prétraité et l'édition de liens conservent leur chemin GNU. Cela
préserve les syntaxes et les fonctions en SRAM du runtime existant.

Pour META, une copie locale des en-têtes SAMD21 est créée dans le répertoire
PlatformIO de build : seuls les qualificateurs des champs de bits anonymes
réservés sont retirés, car Clang C++ les refuse. Les champs nommés gardent
leurs qualificateurs et le SDK installé reste intact. Les enums Clang sont
configurées pour respecter l'ABI de la toolchain GNU ARM.

Les options GCC `--param` et `--specs` ne sont pas transmises à la compilation
Clang ; les specs restent utilisées par le linker GNU. Le LTO LLVM n'est
pas pris en charge dans cette configuration : `-flto` avec Clang provoque
une erreur explicite. Une installation Clang absente produit également une
erreur, sans retour silencieux à GCC.

## Estimation et comparaison

```bash
python3 targets/meta/tools/m0bench/m0_estimate.py output/jeu --compiler gcc
python3 targets/meta/tools/m0bench/m0_estimate.py output/jeu --compiler clang
```

Le choix de l'estimateur est indépendant de celui du projet. Les résultats
sont des estimations Cortex-M0+, pas des mesures sur console. Pour vérifier
le comportement avant de comparer les performances, ajouter
`--cflag=-DBENCH_PIXEL_HASH --cflag=-DBENCH_STATE_HASH`. Ces instruments ont
un coût et doivent être retirés des mesures de vitesse.

Pour inclure le pipeline de rendu META TBLEND dans les deux runs, ajouter
`--cflag=-DBENCH_META_DISPLAY=6`. Dans ce mode, le hash pixels couvre les
bandes réellement envoyées au LCD simulé, avec leurs coordonnées ; le hash
d'état reste indépendant du choix du pipeline. La latence physique SD/SPI
et les interruptions audio du firmware complet ne sont pas modélisées.

Le test natif de l'IR est exécuté avec les deux compilateurs lorsqu'ils sont
disponibles. Le test Pokitto construit les deux firmwares, vérifie les
vecteurs, le checksum, le marqueur de version et la limite Flash. Les tests
hôte restent disponibles sans Clang ni toolchain ARM.

## Validation effectuée

Le 11 octobre 2026 : suite CTest complète validée, y compris les
comparaisons natives des blocs Thumb avec GCC et Clang aux trois niveaux
d'optimisation. Les tests concernés par les derniers ajustements d'ABI ont
été relancés avec succès. Les firmwares synthétiques Pokitto ont été liés
avec les deux compilateurs et leurs formats de démarrage vérifiés. Le
firmware META Clang a aussi été construit via PlatformIO : 161 936 octets
de Flash et 27 344 octets de RAM, dans les limites de la carte.

Une comparaison Cortex-M0+ sur huit trames du programme synthétique donne
les mêmes hashes pixels (`ade26958`) et état (`e5523b5b`) avec GCC 14.2.1
et Clang 23.1.1. Ces résultats ne constituent pas une validation sur
console physique ou sur un corpus de jeux.

La bascule Clang → GCC dans le même projet META a aussi été vérifiée :
PlatformIO recompile bien le C/C++ avec GCC, puis produit un firmware de
152 536 octets de Flash et 27 356 octets de RAM. La provenance des objets
a été contrôlée dans leur section `.comment`, pour les deux compilateurs.

La comparaison sans instrumentation, sur 60 trames du même programme
synthétique, donne 388 277 cycles estimés/trame avec GCC 14.2.1 et
393 079 avec Clang 23.1.1 : Clang utilise environ 1,2 % de cycles en plus
sur ce cas. Le code du benchmark passe de 127 376 à 130 076 octets.
Ce seul programme ne permet pas de conclure pour les jeux commerciaux.

Le 13 octobre 2026, avec la seule toolchain GNU ARM de PlatformIO (GCC 7.2,
binutils 2.29) : Clang émettait du DWARF 5 (`.file 0`, MD5) que cet
assembleur refuse, et aucun firmware Pokitto Clang ne se construisait.
`arm_clang.py` impose maintenant `-gdwarf-4` dès qu'une option `-g` est
présente. Sur Super Mario Land, Kirby's Dream Land et Tetris (600 trames,
entrées d'une partie automatique), l'estimateur donne pour Clang 23.1.1
face à GCC 7.2 : META +1,4 %, +0,2 %, +2,1 % de cycles par trame ;
Pokitto −0,9 %, −0,8 %, +1,2 %. Dans gm0 (firmware META complet, 1800
trames, attentes flash comprises), Clang consomme 2,5 % (Mario) et 3,6 %
(Tetris) de cycles utiles en plus. Les firmwares Clang sont 2 à 5 % plus
gros. Ce sont des estimations et des mesures d'émulateur.

Le 12 octobre 2026, les builds CMake/Ninja natifs ont été validés avec
Clang et GCC. Les 16 tests CTest passent avec le build Clang, y compris
les projets hôte générés ; les neuf cas de sélection couvrent la
préférence automatique, le repli GCC et les choix explicites. La
reconfiguration d'un build GCC conserve bien son compilateur en cache.
