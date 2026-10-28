# Gamebuino AKA — CORE v1.4

Référence : https://github.com/jmp42/Gamebuino_AKA_lib,
révision `b990418e1c35cb104d090a53a891e7d760c58cbe`.
Les faits viennent de `components/gamebuino/include_ll/gb_ll_common.h`,
`gb_ll/gb_ll_lcd.c`, `gb_ll_audio.c`, `gb_ll_sdcard.c` et `sdkconfig`.
Le code du pilote m0boy est indépendant ; la bibliothèque officielle est LGPL.

| Élément | Configuration officielle utilisée |
|---|---|
| CPU | ESP32-S3, 240 MHz |
| Flash | 8 Mio, DIO, 80 MHz (`sdkconfig`) |
| Budget SRAM m0boy | 320 Kio, limite du manifeste de build ; budget conservateur pour statiques/pile, distinct de la SRAM totale du SoC |
| PSRAM | octale, 80 MHz, taille autodétectée ; 8 Mio reste une hypothèse de capacité, pas une mesure |
| Écran | 240×320 physique, 320×240 paysage, RGB565, rotation MX\|MV, inversion |
| Bus écran | i80 8 bits, 20 MHz, WR=21, DC=48, D0..7=3/46/9/10/11/12/13/14 ; CS câblé bas |
| Rétroéclairage / TE | GPIO45, PWM 50 kHz ; TE GPIO47 |
| SDMMC | CLK=2, CMD=38, D0..3=1/15/7/16, 4 bits, 20 MHz |
| I²C | SDA=0, SCL=43, 400 kHz ; expandeurs 0x38 et 0x3f |
| Audio | TAS2505 à 0x18 ; I²S BCLK=18, WS=17, DATA=8, mono 16 bits 44100 Hz |
| Joystick | ADC1 canaux 5/4 (GPIO6/5), X positif droite, Y positif haut |
| Autres | batterie ADC1 canal 3 ; 4 NeoPixels GPIO44 ; vibreur via TAS2505 |

Le pilote remplace les anciennes broches supposées. Les lignes m0boy de
160 pixels sont agrandies sur le panneau. Le tampon DMA n'est réutilisé
qu'après la fin du transfert. Les boutons passent par les expandeurs,
RUN étant actif haut, les autres actifs bas. Le joystick complète la croix.
A/B : jeu ; RUN ou D : Start ; MENU ou C : options. L1/R1, NeoPixels,
batterie et vibreur ne sont pas exploités par m0boy. La question RGB/BGR se règle dans le MADCTL
(ST7789 : bit 3 = 1, le panneau lit des mots BGR). La bibliothèque officielle
pose le bit (0x68) parce que son `lcd_color_rgb` range le rouge dans les bits
bas ; m0boy envoie du RGB565 (rouge en haut), donc le bit reste à 0 (0x60,
`AKA_LCD_BGR` = 0) — constaté sur la carte : Alex Kidd sortait en BGR avec le
bit posé. L'échange des octets de chaque pixel est fait par le bus i80 en
matériel (`swap_color_bytes = 1`, comme `gb_ll_lcd.c`). Aucun échange
logiciel : tous les frontends envoient du RGB565 natif (`M0_COLOR` natif sur
l'AKA, `*_PANEL_SWAP` à 0). Le firmware exporte `m0_aka_lcd` (MADCTL,
COLMOD, `swap_color_bytes`, inversion), les valeurs que son init envoie ;
gm0 les lit pour modéliser l'écran (bit 3, échange d'octets du bus). Les
bits MX/MV/MY ne sont pas modélisés : le tampon est déjà en paysage.
Les frontends Sega
montrent 120 lignes visibles (GG 144→120, SMS et SG 192→120) que le panneau
zoome ×2 depuis la rangée 0 (`M0_PANEL_SRC_ROWS`) ; avant, le plan META de
128 rangées s'étalait en pas de 1,875, d'où un mauvais cadrage et des sauts
de ligne. Le frontend Mega Drive retient une trame sur quatre quand le cœur
rame, pour que l'image ne disparaisse pas, et construit en -O2 (la RAM n'est
pas serrée).

Le TAS2505 utilise la séquence de registres de référence. Le runtime produit
11025 échantillons/s ; chaque échantillon est répété quatre fois pour le
flux 44100 Hz. La pause coupe le flux sans créer une seconde tâche audio.
Le stockage utilise les secteurs SDMMC directement sous le FAT commun ;
le protocole SD SPI ne compile pas pour l'AKA. La RAM de cartouche GB reste
en RAM, avec persistance .SAV sur la carte. La partition application conserve
0x370000 octets et l'ensemble de la table tient dans 8 Mio.

## Validation

- Génération Game Boy, SG-1000, Game Gear et Master System : égalité
  `run_hash` précis/interpréteur sur l’hôte et firmware compilé puis exécuté
  dans gm0 avec une image non uniforme. Jeux de test synthétiques seulement.
- Compilation réelle Xtensa ESP32-S3 avec PlatformIO 6.9.0 / Arduino 2.0.17.
- Firmware META identique avant/après le changement commun SD (SHA-256
  `2f17acd771bbdc0d380234996bbca0a1ed60f9a368728fd41ace4c400dcbaac5`).
- Tests existants PC, ODROID et Pokitto : succès ; fenêtre Xtensa profonde
  et virgule flottante : résultat identique à l’hôte à cinq optimisations.
- gm0 : branche `feat/aka`, checkout voisin `../gm0-aka` ; exécution native
  Xtensa, appels de plateforme émulés par fonction, temps estimé.
- Aucune validation sur une console physique. La sortie vers le launcher
  est un redémarrage, pas un protocole de retour officiellement vérifié.

Ne pas extrapoler ces vérifications à une compatibilité générale avec les
firmwares ESP-IDF ni à une mesure de performance de l'AKA.

## Conversion simple (une ROM, un zip)

```bash
tools/aka/convert.sh jeu.gg                 # -> ./aka-out/<Jeu>.zip
tools/aka/convert.sh -r jeu.sms             # -r : couverture courte, plus rapide
tools/aka/convert.sh -o sortie dossier_de_roms/
```
Sous Windows : double-clic sur `tools\aka\convert_gui.bat` (Python 3 requis) : une
fenêtre permet de choisir des ROMs ou un dossier et le dossier de sortie, puis
« Convertir » ; le dossier des `.zip` s'ouvre à la fin. Elle lance `convert.sh` avec
MSYS2 (`C:\msys64` ; `pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
mingw-w64-x86_64-ninja mingw-w64-x86_64-zlib mingw-w64-x86_64-python`) ou, à défaut,
WSL ; PlatformIO s'installe côté Windows (`pip install platformio`).
Le script construit le recompilateur à la première utilisation, copie les ROMs
dans un dossier de travail, lance `TARGET=aka ./compile.sh` sans question et
range les `.zip` à plat. Décompresser un zip à la racine de la carte SD.
Game Boy/Color, SG-1000, Game Gear, Master System et, en expérimental, Mega Drive (`.md`, `.gen`, `.smd`) :
pour le Mega Drive, le script rejoue une partie automatique (entrées aléatoires) sur PC pour établir le profil natif, puis
génère le projet AKA et le compile. Un zip ou un dossier de ROMs est accepté.

## Conversion et essai

`TARGET=aka ./compile.sh` produit le dossier du launcher : `firmware.bin`,
`firmware.sym`, `meta.json`, sauvegardes et états ; une vignette `screen.bmp`
si une icône est disponible. Les fichiers de boot et `FLASH.txt` permettent
aussi une installation par port série. Le fichier de symboles provient du
même ELF et permet le chargement du ZIP dans gm0.

Le BunnyMark fourni a été essayé sans changer ses instructions de jeu, avec
une table de symboles reconstruite pour son binaire exact (conservée dans
`logs/`, pas dans git). B puis relâchement ajoute 100 lapins ; capture et
entrée fonctionnent. Le ZIP original sans symboles n’est pas reconnu seul
par cette première version de gm0. Les nombres FPS qu’affiche le jeu dans
cet émulateur ne sont pas des mesures de la console.

Les journaux de cette validation sont dans `logs/aka-*` et
`logs/gm0-aka-*`. Tests gm0 : 9/9 (dont ESP8266, CPU AKA et machine AKA),
puis CPU/machine AKA repassés après les derniers ajustements de stockage
et de hauteur de surface Sega. La reconstruction des symboles BunnyMark
et son essai sont spécifiques au firmware fourni.

## Jeux réels dans gm0 (2026-10-26)

`TARGET=aka ./compile.sh` construit sans erreur Super Mario Land (World).gb
et Ristar (USA).gg : firmwares de 996 Ko et 1,0 Mo (partition 0x370000,
RAM statique 44/320 Kio pour Mario, ROM de Ristar en flash), emballés pour
le launcher avec les fichiers de boot et `FLASH.txt`. Le harnais hôte du
projet Mario affiche l'écran titre à l'image 600 ; le test synthétique
(`tests/test_aka_target.py` avec `AKA_PIO` et `AKA_GM0`) repasse
intégralement le même jour.

Dans gm0 (branche `aka`, `79490f1`, reconstruit), les mêmes firmwares ne
montrent pas l'image du jeu :

- Mario, build profilé de `compile.sh` : faute Xtensa pendant l'init de
  l'application, avant l'image 30 — appel ROM `memset` à arguments
  incohérents (destination 0x6, taille ≈ 1 Gio), identique aux images
  30, 120 et 600.
- Mario et Ristar régénérés sans profil (`--meta-static --meta-precise`,
  sans couverture) : pas de faute, l'écran reste uniforme (vert du fond
  DMG `e0f8d0` pour Mario, noir pour Ristar) — le frontend peint une fois
  puis la partie ne vient pas.
- Cartouche synthétique : marche (image non uniforme, aucune ligne
  « AKA: »), même gm0, même chaîne.

La suspicion va à la machine AKA expérimentale de gm0 (réveil de la boucle
de l'application : fin de DMA LCD, flux audio, délais de tâche — invisibles
sur une cartouche de test qui dessine dès la première image), sans qu'un
bug du frontend AKA soit exclu. Une carte physique reste l'arbitre ; ne pas
en déduire une compatibilité des jeux réels sur l'AKA.
