# ARM920T et bus S3C2400X01 pour la GP32

Ce module autonome en C99 fournit la fenêtre de ROM de démarrage, la fenêtre
SDRAM et le routage des accès d'E/S vers un modèle de périphériques fourni par
l'appelant. Il utilise des adresses physiques en petit-boutiste. La définition
des fenêtres et des espaces de registres vient du manuel
[Samsung S3C2400X01](https://datasheets.chipdb.org/Samsung/S3C2400.pdf).

Le bus reçoit les accès avec leur initiateur (fetch CPU, données CPU, DMA ou
lecture LCD). L'observateur voit l'adresse physique, la largeur, la valeur
effective, le statut de l'accès et l'offset dans la ROM ou la RAM. Une sonde peut
remplacer la valeur d'une lecture CPU d'E/S après les effets de bord normaux.
L'inspection traverse les mêmes fenêtres sans appeler les observateurs, les
sondes ou les effets de bord des périphériques. Un transfert DMA réussi publie
les deux accès puis l'événement de copie, avec les offsets physiques des deux
extrémités. L'appelant fournit la RAM et la configuration de chaque instance.

```sh
cmake -G Ninja -S targets/gp32/runtime -B build/gp32-bus
cmake --build build/gp32-bus
ctest --test-dir build/gp32-bus --output-on-failure
```

La bibliothèque `m0_arm920t` s'appuie sur le bus physique. L'appelant
initialise une instance avec `arm920t_reset`, pose les niveaux IRQ et FIQ,
puis appelle `arm920t_step`. Chaque appel traite une instruction, une entrée
d'exception ou l'attente d'une interruption. Le PC d'état désigne la prochaine
instruction, avec la valeur visible par les instructions calculée à la lecture.
Le cœur interprète ARMv4T (ARM et Thumb), gère les banques de registres et
les retours d'exception, et expose les registres CP15 utilisés pour la traduction
MMU. Celle-ci gère les sections, les tables fines et grossières, les pages et
sous-pages, la protection par domaine, FCSE, et deux TLB avec invalidation
globale ou individuelle. Les lectures de tables ont leur propre type d'accès.
Voir le [manuel ARM920T DDI 0151C](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb).

Le callback `predicate` reçoit la décision naturelle pour les instructions ARM
prédicatives et les branches Thumb conditionnelles, avant leurs effets. Il peut
retourner l'autre issue pour une fourche. Le callback `trace` reçoit le PC
virtuel, l'adresse physique, l'opcode, l'état ARM ou Thumb et la décision
appliquée. L'état du processeur se copie par valeur en mémoire. Une fourche
doit aussi restaurer la RAM et les périphériques et rattacher leurs callbacks
à l'instance correspondante.

Les caches d'instructions et de données ont chacun 16 Kio et des lignes de
32 octets. Le cœur émule les modes écriture directe et différée du cache de
données, ainsi que les opérations CP15 de nettoyage, d'invalidation et de
verrouillage. Le tampon d'écriture conserve l'ordre des accès externes et se
vide avant une lecture externe ou sur demande via CP15. Le verrouillage des
TLB permet de protéger des entrées contre le remplacement et l'invalidation
globale. Une invalidation individuelle peut toujours les retirer.

Le callback CPU `access` observe les lectures et écritures terminées avec leurs
adresses virtuelle et physique, y compris lorsqu'un cache répond sans accès au
bus. Les callbacks du bus voient les remplissages de cache, les écritures de
lignes sales et la matérialisation des écritures tamponnées. L'état copiable
inclut les caches, les TLB et le tampon d'écriture.

L'unité de progression est l'instruction. Le tampon peut avancer d'un mot à
chaque pas et se vide avant un accès externe qui exige l'ordre. Les délais
du pipeline et du bus, JTAG et EmbeddedICE restent à modéliser. Un rejet du
bus pendant un remplissage de cache est signalé comme un abort dans ce modèle.
## Démarrer la GP32

La bibliothèque `m0_s3c2400` associe le processeur, les registres, 8 Mio de
SDRAM à `0x0c000000` et un BIOS brut de 512 Kio à l'adresse zéro.
`s3c2400_boot` initialise l'instance et `s3c2400_step` avance le processeur,
les quatre canaux DMA, les timers PWM et l'affichage. L'application peut
injecter des requêtes DMA matérielles et des interruptions. Les accès au
framebuffer par le DMA dédié sont signalés au bus comme `S3C2400_LCD_LOAD`.
Le TFT lit la mémoire ligne par ligne, selon LCDSADDR1–3 et BSWP/HWSWP. Le
callback de trame reçoit le résultat complet en ARGB, dans l'orientation
native 240 × 320. Les exécutables présentent ou exportent l'image en
320 × 240, comme sur la GP32.

```sh
build/gp32-bus/m0_gp32_boot /chemin/vers/bios.bin 350000000 ecran.ppm
build/gp32-bus/m0_gp32_sdl /chemin/vers/bios.bin
```

La fenêtre est construite sur les hôtes POSIX qui disposent de SDL2.
Elle applique une courbe de puissance 1,22 sur les canaux RVB, avec un blanc à
235/255, pour atténuer les hautes lumières et assombrir les tons moyens.
La fenêtre capture les lignes LCD brutes (`lcd_capture`) : le cœur copie à
chaque ligne les mots lus en mémoire et la palette quand elle change. Le
fragment shader OpenGL décode les formats, la palette et la correction de
couleur. La rotation du tampon en colonnes 240 × 320 vers l'écran 320 × 240
est faite elle aussi par le GPU, avec les coordonnées de texture du quad.
Sans capture, le cœur convertit l'image en ARGB côté CPU, comme pour
`m0_gp32_boot`. Un observateur du bus voit les lectures LCD dans les deux cas.
Les touches sont lues par position (disposition QWERTY de référence) : les
flèches pilotent la direction, Q et W correspondent à L et R, Z et X à B et A,
Entrée à Start et Retour arrière à Select. Sur un clavier AZERTY, ces
positions portent A, Z, W et X. Une manette SDL permet aussi d'utiliser la
croix ou le stick gauche. P met en pause, F11 bascule en plein écran, Tab
maintenu accélère l'émulation et Échap ferme la fenêtre.

L'émulation tourne dans son propre fil. L'image passe au GPU par des PBO et
la cadence suit l'audio, avec un asservissement de ±2 %. Le nombre de cycles
FCLK par instruction s'adapte entre 2 et 16 selon la charge de l'hôte ;
`--cpi N` le fixe. `--fullscreen` démarre en plein écran et `--stats` affiche
la vitesse et le CPI courants.

### Menu et états sauvegardés

Maintenir Select une demi-seconde, seul, ouvre le menu des options (F1 au
clavier aussi). Un appui court reste transmis au jeu, et Select combiné à une
autre touche également. Le menu met l'émulation en pause et propose :
reprendre, redémarrer, quatre emplacements d'état (sauver, charger), le volume,
la vitesse, le plein écran et quitter. B, Select ou Échap le ferment.

Un emplacement d'état est un **shot** : un PNG 320×240 de l'écran qui embarque
la machine, l'EEPROM et la SmartMedia (bloc `gpSt` compressé, environ 8 à 12 Mo
pour une carte de 32 Mo). Les shots sont rangés dans le dossier `shots/` à côté
de l'exécutable (`<fichier>.shots/` si ce dossier n'est pas inscriptible) :
`<nom>-slot1.png` à `slot4.png` pour le menu, `<nom>-<date>-f<trame>-<n>.png`
pour les autres. Le shot qu'un emplacement contenait est conservé sous ce second
nom. Les anciens fichiers `.state1` à `.state4` se chargent encore si
l'emplacement est vide.

### Shots

- **F5** prend un shot.
- Un shot **glissé sur la fenêtre** ou passé en argument (`jeu shot.png`) reprend
  la partie à cet endroit. Avant le premier chargement d'une session, la carte est
  copiée dans `<carte>.before-shot`, car le shot apporte la sienne.
- Avec une application recompilée, le code rencontré qui n'est pas encore couvert
  est écrit dans `shots/<nom>-found.code` à chaque shot et à la sortie.
- deepeel (0.446.0) écrit et relit le même format : une exploration peut repartir
  d'un shot du jeu, et le jeu d'un instantané de deepeel.

Un shot ne se charge que pour le même programme et une carte de même taille. Un
shot produit sous Linux se charge sous Windows et inversement (vérifié avec Wine
sur le banc d'essai) ; une version du runtime à la structure différente le
refuse. Redémarrer revient à l'instant qui suit l'amorçage et conserve la carte
et l'EEPROM.

En mode HLE, une lecture ou une écriture par pointeur nul (banque 0, sans BIOS)
ne plante pas : l'écriture est ignorée et la lecture rend zéro, alors qu'une vraie
console rendrait un octet de son BIOS. Un saut à ces adresses reste une faute.

`--freeze ADRESSE=OCTETS` (hexadécimal, répétable) réécrit en continu une zone de
la RAM, par exemple pour bloquer la vie du joueur : le jeu est alors modifié.

### Construction optimisée

La configuration par défaut est `Release` avec LTO (`M0_GP32_LTO`). Le build
guidé par profil s'entraîne sur des fichiers locaux : applications GP32 et,
au choix, un BIOS de 512 Kio.

```sh
sh targets/gp32/runtime/pgo.sh build/gp32-pgo jeu.fxe bios.bin
build/gp32-pgo/m0_gp32_bench --hle 300000000 --exec jeu.fxe
```

`M0_GP32_JOBS` borne les compilations et les tâches LTO parallèles, et
`M0_GP32_TARGETS` limite les exécutables du build final (chacun recompile
l'application). Une application recompilée par `recompile.py` s'ajoute avant
`pgo.sh` avec `cmake -S targets/gp32/runtime -B build/gp32-pgo
-DM0_GP32_AOT=/chemin/application_aot.c`. La sortie est découpée en plusieurs
fichiers ; la compilation optimisée d'un gros jeu demande plusieurs Go de
mémoire.

Build Windows (MinGW croisé) : la bibliothèque `libgcov` de la cible peut ne pas
permettre l'entraînement ; le profil du build Linux est alors repris. Configurer
avec `x86_64-w64-mingw32-cmake` et la chaîne MinGW, les mêmes options
`M0_GP32_AOT`, `-DM0_GP32_PGO=use -DM0_GP32_PGO_DIR=<profil>` et
`-DCMAKE_C_FLAGS=-Wno-error=coverage-mismatch` (quelques fonctions se compilent
différemment et restent alors sans profil). Les fichiers `.gcda` copiés dans
`<profil>` doivent être renommés : GCC encode le chemin du répertoire de build
dans leur nom (remplacer la partie qui désigne le build Linux par celle du build
Windows).

`m0_gp32_bench` mesure la vitesse sans fenêtre : instructions par seconde,
part des boucles d'attente sautées et rapport au temps réel. Le saut de ces
boucles et des périodes d'inactivité reste équivalent à l'exécution pas à pas
dans ce modèle. Une écriture qui range en SDRAM la valeur déjà présente ne
compte pas comme un effet, et les attentes du retour vertical dans le BIOS HLE
(bascule de page, palette) avancent jusqu'au prochain événement périphérique au
lieu de répéter l'appel pas à pas.
Le banc d'essai rend la carte inscriptible **en mémoire seulement** (le fichier
n'est pas modifié) : certains jeux restent bloqués si leur écriture échoue.
`--from SHOT.png` démarre d'un shot, `--shot SORTIE.png` en écrit un à la fin,
et `--found FICHIER` écrit le code non couvert rencontré ; ces options se
placent en fin de ligne.

Réduire la taille du code recompilé : une instruction en ligne coûte environ
500 octets, alors que peu d'entre elles font l'essentiel du temps.

```sh
python3 recompile.py jeu.code image.bin compte/app_aot.c --count
# construire ce build, puis le rejouer avec M0_GP32_COUNTS=comptes/essai1
python3 recompile.py jeu.code image.bin final/app_aot.c --hot comptes/* --share 99.5
```

`--count` produit un build qui compte les exécutions dans le fichier nommé par
`M0_GP32_COUNTS`. `--hot` relit ces fichiers et met en ligne les instructions
les plus exécutées, jusqu'à la part `--share` des exécutions (99,5 % par
défaut). Sans `--hybrid`, les autres restent recompilées mais partagent une même
étape, environ trois fois plus petite ; avec `--hybrid`, elles ne sont pas
recompilées du tout (voir plus bas). Parmi les instructions en ligne, les suites sans accès
mémoire ni branchement d'une même ligne de cache ne font qu'une vérification
commune. `M0_GP32_MISSES=FICHIER` ajoute au fichier le code rencontré qui n'a
pas été recompilé.

`--fast`, placé juste après le fichier de sortie, privilégie la vitesse sur
l'exactitude. Les instructions ARM en ligne sont comparées à la mémoire une
seule fois par bloc (24 instructions au plus), sans passer par le cache
d'instructions, et leurs cycles sont comptés d'un coup : un événement
périphérique ou une interruption peut attendre la fin du bloc. L'hôte exécute
environ 33 à 40 % d'instructions de moins qu'en mode exact sur trois scènes de
Pinball Dreams (écrans comparés identiques), et 27 % de moins sur dix scènes de
Her Knights. L'état final de la machine diffère de celui du mode exact : les
comparaisons d'états entre builds se font sans `--fast`.

`--hybrid`, à la place de `--fast`, garde la machine du mode exact et exécute le
processeur en natif : les blocs recompilés tiennent les registres et les drapeaux
en variables C, lisent et écrivent la SDRAM directement et décomptent leurs
cycles d'un coup, sans dépasser le prochain événement périphérique. Le code
précis reprend la main pour les registres matériels, le code non couvert ou
modifié, le Thumb, les modes avec MMU ou cache actifs, les observateurs et les
instructions que la traduction native ne gère pas. Une boucle arrière courte
retourne à la machine, qui saute les boucles d'attente, sauf si un registre y
avance à chaque tour ou si elle modifie la mémoire (en natif, un transfert de
plusieurs registres compte toujours comme une modification). L'état final est identique à celui du mode exact sur les
scènes comparées. Par rapport au mode exact, l'hôte exécute 47 à 62 %
d'instructions de moins sur les trois scènes de Pinball Dreams et 59 % de moins
sur les dix scènes de Her Knights.

Avec `--hybrid --hot`, seules les instructions chaudes sont recompilées. Le reste
du code couvert est exécuté par l'interpréteur, déjà présent dans l'exécutable,
depuis une boucle qui évite de repasser par la machine ; une table d'un bit par
adresse distingue ce code du code non couvert (qui seul est signalé et ajouté à
la carte). Ajouter du code couvert rarement exécuté ne grossit donc presque plus
l'exécutable. `--share` règle le compromis entre taille et vitesse (99,5 % par
défaut, 99,9 % pour un binaire plus gros et un peu plus rapide).

Les petites routines que le HLE écrit en RAM (affichage, interruption du timer,
broches de la carte) sont recompilées avec l'application, et les services de la
carte atteignent les registres du port GPIO sans passer par le décodage du bus,
sauf si un observateur du bus est branché. Une écriture sur ces broches
n'interrompt plus le lot d'instructions en cours.

Sur un programme ARM synthétique de calcul
pur, il en reste environ 7 instructions x86 par instruction ARM (compilation
avec LTO et profil) ; dans les jeux, les services HLE, les périphériques et les
événements dominent, et le ratio est bien plus élevé. Ces chiffres sont des
mesures de ce runtime sur une machine hôte.

La barre de titre de la fenêtre affiche la vitesse du jeu en pour cent, la
capacité de l'hôte (`4,2x réel` : ce qu'il pourrait émuler sans la cadence) et
le CPI. `--stats` ajoute les compteurs audio : sous-alimentations, paires
abandonnées et ouvertures de flux.

Le cœur expose les dix boutons par `s3c2400_set_buttons`. Start et Select
peuvent déclencher EINT6 et EINT7 selon la configuration GPIO du firmware.

### Carte et EEPROM

Les deux exécutables acceptent les mêmes options de stockage.

```sh
build/gp32-bus/m0_gp32_sdl bios.bin --smc jeux.smc --eeprom console.eep
build/gp32-bus/m0_gp32_sdl bios.bin --smc ./smartmedia --eeprom console.eep
build/gp32-bus/m0_gp32_boot bios.bin 350000000 ecran.ppm --smc ./smartmedia --readonly
```

Une image SMC contient les pages NAND brutes et leurs octets auxiliaires.
Les images avec un en-tête GeePee32 pris en charge fournissent aussi
l'identifiant de carte. En mode dossier, Python 3.11+, dosfstools
et mtools fabriquent une carte de 32 Mio contenant un volume FAT16.
Le BIOS utilise les mêmes commandes GPIO pour les deux supports. Placez
les titres dans les sous-dossiers attendus par votre firmware, par exemple
`GPMM` pour les FXE chargés par yAnl.

Le dossier est lu à l'ouverture. À la fermeture normale, les créations,
modifications et suppressions du volume sont répercutées sur le dossier.
Si le même fichier a changé côté hôte et côté invité, l'exécutable signale
le conflit et conserve l'image mise à jour dans le répertoire temporaire
indiqué. `--readonly` protège la SmartMedia en écriture. Sans `--smc`,
la GP32 démarre sans carte.

L'EEPROM I²C Samsung S524LB0X91 de 4 Kio est initialisée à `ff`.
`--eeprom` charge un fichier brut et y enregistre les modifications à la
fermeture. Le fichier est créé à la première sauvegarde. Le modèle gère
l'adresse sur deux octets, la lecture séquentielle, la page de 32 octets
et le NACK pendant la programmation. [Fiche technique Samsung](https://www.farnell.com/datasheets/101920.pdf).

Pour intégrer le cœur, l'appelant peut attacher un tampon de carte avec
`gp32_smc_attach` après `s3c2400_boot`. Les registres GPIO et I²C restent
observables par les hooks du bus. Une fourche qui autorise les écritures
SmartMedia doit posséder sa propre copie de l'image.

### Son et validation

Le FIFO IIS accepte la lecture PCM stéréo avec le DMA 2 ou des écritures
CPU. Le callback `S3c2400Audio` reçoit les paires PCM signées et leur
fréquence calculée à partir des registres d'horloge. La fenêtre les convertit
elle-même à 48 kHz (`gp32_resample.h`, filtre à 16 points, phase continue d'un
bloc à l'autre) et préremplit la file audio avant lecture : le convertisseur de
SDL 2 repart de zéro à chaque bloc, ce qui faisait craquer le son
(`test_resample.c` vérifie qu'un sinus traverse les blocs sans rupture). Elle
ajuste le nombre de cycles FCLK par instruction, comme décrit plus haut. Le
cœur utilise un cycle par instruction par défaut.

Le jeu commande le DAC audio par le bus L3. Le bit de coupure (mute) est
appliqué. Le registre de volume est enregistré mais **n'est pas appliqué** :
l'initialisation du son de Pinball Dreams y écrit en dur 0x3f, que la fiche de
l'UDA1330ATS définit comme une atténuation infinie, et n'y écrit plus ensuite.
La réaction de la carte de la console à cette écriture n'est pas établie ;
suivre la fiche rendrait le jeu muet. Le filtrage de désaccentuation et les
effets analogiques ne sont pas modélisés.

[Fiche technique NXP](https://www.nxp.com/docs/en/data-sheet/UDA1330ATS.pdf).

Les tests synthétiques vérifient le DMA et l'affichage TFT, la
lecture et l'écriture NAND via GPIO, l'effacement et la protection en
écriture, l'EEPROM I²C, les boutons et le PCM par DMA. Le BIOS GP32 Europe
v1.5.7 affiche son menu et produit le son d'amorçage. Le navigateur de yAnl
a lu le dossier `GPMM` d'une SmartMedia montée depuis l'hôte et affiché
le FXE après navigation aux boutons. Les horloges restent approximatives.
Les projets GP32 générés utilisent encore gp32emu et le cœur autonome
reste à intégrer dans deepeel.

### Registres d'état et horloge

LCDCON5 publie les phases de balayage horizontale (bits 18:17) et verticale
(bits 20:19).

### Autres périphériques du S3C2400

| Périphérique | Comportement émulé | Limites |
|---|---|---|
| Watchdog | décompte (prescaler et diviseur sur PCLK), rechargement, `INT_WDT`, reset de la machine (RAM, carte, EEPROM et calendrier conservés) | en HLE, sans BIOS à redémarrer, un reset termine l'application ; le watchdog y est arrêté au démarrage, comme le fait le BIOS |
| RTC | `INT_TICK` selon TICNT, calendrier BCD qui avance chaque seconde émulée, alarme `RTCALM` (`INT_RTC`) | la fenêtre SDL part de l'heure locale du PC, les outils sans fenêtre d'une date fixe (déterminisme) |
| UART 0/1 | émission immédiate, requêtes d'interruption en émission et réception, bouclage interne | rien n'est branché : pas de débit en bauds, pas de DMA, pas de FIFO |
| ADC | conversion immédiate, drapeau de fin, `INT_ADC` | valeur fixe (batterie pleine) sur tous les canaux |
| SPI | toujours prêt, reçoit `0xff`, interruption de fin de transfert | aucun périphérique, pas de DMA |
| EINT | START et SELECT (EINT6/7), seules touches câblées sur des lignes d'interruption | |
| MMC, USB | registres mémorisés | la console n'a pas de lecteur MMC ; aucun hôte ni périphérique USB n'est branché |

Les états sauvegardés avant l'ajout de ces périphériques se chargent toujours.

## Démarrer un exécutable GP32

`m0_gp32_sdl` et `m0_gp32_boot` partagent l'option `--exec`. Le
chargeur lit le GXB (aussi distribué sous les extensions AXF ou BIN) et
déchiffre le FXE avec sa section RW. Le firmware exécute le programme
ARM emboîté, y compris le stub de décompression lorsqu'il existe. Un
BIN de firmware de 512 Kio démarre depuis son propre vecteur de reset.
Au chargement, la section ZI de l'application est remise à zéro. Les
variantes de paquets ZPK, dont le profil à table XOR embarquée de Her Knights
et Tomak, sont reconnues. Les ressources livrées avec un jeu ZPK sont lues
sur la SmartMedia dans leur arborescence GP32.

Sans `--smc`, un ZPK est extrait automatiquement dans un dossier de carte :
`~/.local/share/m0boy-gp32/<nom>` (ou sous `XDG_DATA_HOME`), ou
`$M0_GP32_CARDS/<nom>`. Ce dossier reçoit aussi les sauvegardes. Les dossiers
système `GPMM` et `GPSYS` y sont créés. La fabrication du volume demande
`python3`, `mkfs.fat` et `mcopy`. La carte est découpée en zones de 1024 blocs
physiques pour 1000 blocs logiques, adressés relativement à leur zone, comme
l'attendent les pilotes des jeux. Un arrêt forcé du processus laisse le
répertoire temporaire `/tmp/m0-gp32-card.*`.

```sh
build/gp32-bus/m0_gp32_sdl BIOS.bin --exec jeu.fxe --smc ./smartmedia
build/gp32-bus/m0_gp32_boot BIOS.bin 100000000 écran.ppm --exec jeu.gxb
```

Pour un couple GXE/GXC installé et lié à un identifiant, monter la
SmartMedia puis sélectionner le titre dans le menu GAME du firmware
Game Park. Le firmware lit le SMCID à travers le contrôleur SmartMedia
ou le PDUID à travers l'EEPROM I²C. Les identifiants peuvent provenir
d'un dump SmartMedia GeePee32 avec UID, de l'EEPROM chargée avec
`--eeprom`, d'un fichier `--id-file` (GPM pour la carte, GPS pour la
console), ou de `--smcid` et `--pduid` (32 chiffres hexadécimaux).
La surcharge SMCID fonctionne également avec un répertoire SmartMedia.
Les surcharges d'identité s'appliquent à la session.

```sh
build/gp32-bus/m0_gp32_sdl --identify ./smartmedia/GAME/JEU.GXE
build/gp32-bus/m0_gp32_sdl --identify ./JEU.GPM
build/gp32-bus/m0_gp32_sdl BIOS.bin --smc ./smartmedia --id-file ./JEU.GPM --ids
```

`--identify` indique quelle identité demande le descripteur GXE et lit
sa valeur lorsqu'un GPM ou GPS du même nom l'accompagne. Si le
descripteur seul fournit le mode de chiffrement, la valeur est
signalée comme indisponible. `--ids` affiche les identités effectives
de la machine démarrée.

Essais locaux avec le firmware officiel 1.57E et le runtime natif :
jezZBall depuis son FXE affiche le terrain de jeu, Her Knights depuis
son ZPK atteint le logo du studio, et le Free Launcher chiffré s'ouvre
depuis une SmartMedia GeePee32 avec son SMCID. Le passage par le menu
GAME du 1.57E utilise le bouton START.
La routine PDUID native du 1.57E relit également les 16 octets injectés
par `--pduid` après décodage. Le modèle I²C diffère l'acquittement du
START répété jusqu'à la libération de l'interruption du transfert
précédent, comme l'attend le pilote du firmware.

## Amorçage HLE sans BIOS

`--hle` remplace le chemin du BIOS. Le décodeur partagé charge un GXB, un FXE,
un BIN au format GXB ou un ZPK reconnu directement dans les 8 Mio de SDRAM.
Le mode initialise la pile et le TFT en 8 bits indexés, comme le firmware,
puis lance le stub ARM de l'application.
Une partie de la SDRAM haute est réservée à la pile, aux paramètres du SDK et
aux surfaces vidéo.

```sh
build/gp32-bus/m0_gp32_boot --hle 10000000 ecran.ppm --exec jeu.fxe
build/gp32-bus/m0_gp32_sdl --hle --exec jeu.gxb --smc ./carte --eeprom gp32.eeprom
```

Les SWI ARM et Thumb passent par le hook optionnel `software_interrupt` du CPU.
Le HLE traite le chargement en chaîne depuis la RAM, la sortie, les horloges,
les minuteries 0 à 3 du SDK, le chemin de l'application, les boutons, la
présence de la SmartMedia, ses identifiants et les lectures et écritures EEPROM.
Les minuteries utilisent le PWM et le contrôleur d'interruptions du S3C2400.
Le SDK peut lire le compteur de ticks. Le HLE installe et retire aussi les
handlers IRQ ARM des jeux, notamment celui du timer 4 de Pinball Dreams.

Les appels vidéo HLE configurent le LCD du S3C2400 et basculent ses surfaces
en SDRAM. En mode indexé, le contrôleur utilise la palette par défaut du SDK
ou celle fournie par le jeu. Le LCD lit les pixels par son DMA d'affichage.
SDL2 reçoit la trame terminée et la présente tournée à 320×240. Le mode
sans fenêtre prend la même trame pour écrire son PPM.

Les accès de données des services passent par le cœur ARM et ses observateurs.
Les accès directs au LCD, à l'audio et à la SmartMedia conservent leurs
périphériques émulés. `--smc` accepte toujours une image ou un répertoire. Les
appels HLE de présence et d'identité interrogent la carte montée directement.
Les bibliothèques FAT embarquées dans les jeux accèdent à cette carte via
la NAND, par le GPIO ou par les callbacks fournis par le SWI `0x0b`,
sélecteur 3. Le chemin direct entre cette couche et le répertoire hôte reste
à implémenter.

Quelques tolérances couvrent des programmes qui comptent sur le firmware : les
écritures vers l'adresse nulle sont absorbées et comptées, les mots
d'identification du firmware lus aux adresses `0x1010` à `0x1017` sont
fournis, et les appels de semihosting sans effet sur la console sont acceptés.

Un SWI absent arrête l'exécution et indique son numéro et son PC. JezzBall
démarre directement depuis son FXE, décompresse le jeu et affiche son terrain
sans image BIOS ni SmartMedia.

```sh
build/gp32-bus/m0_gp32_sdl --hle --exec /chemin/vers/jezzball.fxe
```

Essais en HLE avec des entrées aléatoires : les 11 paquets ZPK disponibles
atteignent leurs menus ou le jeu, Pinball Dreams et un programme de test GDL
tournent. Ce sont des observations de cet émulateur, sans comparaison avec la
console.

La synchronisation des bascules vidéo est approchée à l’intervalle de retour vertical.
Le menu BIOS et le lancement des paires commerciales GXE/GXC restent à couvrir.
