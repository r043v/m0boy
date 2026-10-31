#!/usr/bin/env bash
# Convertit les ROMs .gb/.gbc et .sg d'un dossier en jeux Gamebuino META
# (ou Pokitto, TARGET=pokitto), rangés comme le loader de la console les
# attend.
#
# Usage : ./compile.sh [dossier_roms] [dossier_sortie]
#   Par défaut : ./roms/gb/ et ./roms/sg/, jeux dans ./roms/meta/<sys>/<Jeu>/
#   (sys = plateforme source : gb, sg, gg ou sms ; deux consoles peuvent ainsi
#   porter le même nom de jeu sans se distraire).
#   Un dossier explicite accepte .gb/.gbc/.sg/.gg (ou ses sous-dossiers gb/, sg/ et gg/).
#   PICO-8 (META, Pokitto, ESPboy ; SRC_SYSTEM=pico8 pour eux seuls) : .p8 et
#   .p8.png, à la racine, sous pico8/ ou dans un dossier par cartouche sous
#   pico8/ (le jeu prend le nom du dossier) ; tout le Lua est compilé, fichier
#   d'états <NOM>.ST8 à côté du .bin (ESPboy : dans la flash).
#   SG-1000 : code Z80 recompilé (projets out/sg1000/<cible>/<jeu>/), GCC ou Clang, couverture NTSC automatique
#   de AUTO_SECONDS secondes émulées (SG_AUTO_FRAMES pour choisir les images) :
#   bouton 1 maintenu, une partie sans touche (logos, démo), plus SG_SEEDS parties aléatoires (défaut 8)
#   et EXPLORERS explorations (voir plus bas).
#   Game Gear : même chaîne (projets out/gg/<cible>/<jeu>/), parties 9 fois plus longues (les jeux sont plus
#   longs à atteindre) ; une grosse ROM laisse des pages sur la carte (<NOM>.GG copié à côté du .bin), avec
#   les secteurs les plus lus gardés en flash ; la RAM de cartouche vit dans <NOM>.SAV.
#   Le cache carte (META) part à 3 secteurs en blocs de 128 et grossit d un
#   secteur tant que le rapport RAM du build laisse ~2000 octets à la pile.
#   Les parties
#   enregistrées par une version qui ne comptait pas les exécutions (sans « # frames ») ou sans exploration
#   sont refaites.  Parties GG/SMS : bouton 1 maintenu, sans touche, SG_SEEDS parties aléatoires et
#   EXPLORERS explorations.
#   PLAY=1 utilise le lecteur natif SG ; PACK et les sauvegardes GB ne s’appliquent pas.
#   (le .elf de débogage reste dans out/<jeu>/.pio/build/meta/)
#
#   TARGET=pokitto  même chaîne pour la Pokitto : jeux dans ./roms/pokitto/<sys>/<Jeu>/,
#                   projets dans out/pokitto/<jeu>/ (build/firmware.elf), firmware
#                   construit par make ou cmake+ninja (arm-none-eabi-gcc seul,
#                   sans PlatformIO),
#                   228 Kio de flash sous le loader au lieu de 240, pas
#                   d'icônes (format du loader META).  Les parties
#                   enregistrées sont communes aux deux consoles.
#   TARGET=both     les deux consoles, l'une après l'autre.  La sélection des
#                   jeux et la confirmation des mises à jour de version sont
#                   demandées une seule fois, puis appliquées aux deux.
#                   Avec un dossier de sortie explicite, chaque console y a
#                   son sous-dossier (<sortie>/meta, <sortie>/pokitto).
#   Sans TARGET, dans un terminal, la console est demandée (méta par défaut).
#
# Chaque jeu devient un .zip contenant son répertoire, à déposer tel quel
# (décompressé) à la racine de la carte SD — <Sortie>/<source>/<Jeu>.zip,
# avec <source>/<Jeu>/ dedans, <source> étant la plateforme de la ROM
# (gb, sg, gg ou sms) :
#   Les fichiers du jeu portent tous le même nom court 8.3, <NOM> (les
#   lettres et chiffres du nom du jeu, en majuscules ; au-delà de 8, les 6
#   premiers et 2 chiffres hexadécimaux du SHA-1 de la ROM : « Zerd (J) [!] »
#   -> ZERDJ, « Super Mario Land » -> SUPERM<xx>) :
#   <Jeu>/<NOM>.BIN        le firmware.  Le loader essaie « /Jeu/Jeu.bin »
#                          puis prend le premier .bin du dossier ; le
#                          dossier garde le nom long, affiché par le loader.
#                          Le firmware retrouve son dossier sur la carte par
#                          ce .BIN (premier secteur = le firmware en flash)
#                          et y cherche ses autres fichiers : deux dossiers
#                          (variantes 48 et 72 MHz) peuvent porter le même
#                          <NOM>.  (Pokitto : <Jeu>/<Jeu>.bin.)
#   <Jeu>/<NOM>.GB         la ROM entière, pour les jeux qui ne tiennent pas
#   (ou .GG, .SMS)         dans la flash.  La RAM de cartouche Game Gear vit
#                          de même dans <NOM>.SAV.
#   <Jeu>/ICON.BMP         icône 32x32 du menu (grille)
#   <Jeu>/TITLESCREEN.BMP  écran titre 80x64 du menu (détail)
#     Icônes tirées de thumbnails.libretro.com (Named_Titles), mises en cache
#     dans <dossier_sortie>/.cache/icons/ ; un fichier local du même nom
#     (<dossier_roms>/icons/<Jeu>.png) a la priorité.  Un échec n'arrête
#     rien : le loader affiche son icône par défaut.  NO_ICONS=1 pour
#     désactiver les téléchargements.
#   <Jeu>/<NOM>.SAV        fichier de sauvegarde (cartouche à pile ou grande
#                         RAM) : créé une seule fois, la console y écrit.
#   <Jeu>/.gbrmeta.json    état interne (ROM, sha1, version, PACK) ; ignorable.
#   <Sortie>/<source>/<Jeu>.flags  options de build du jeu, une par ligne
#                          (# = commentaire ; ex. « -D META_TBLEND=0 » pour un
#                          jeu dont la pile est trop juste) ; optionnel.
#
# Versions : chaque firmware embarque la version de l'édition de sa console
# (« gbrecomp-meta x.y.z » ou « gbrecomp-pokitto x.y.z » dans le .bin ; une
# version par console : targets/meta/VERSION, targets/consoles/pokitto/VERSION).
# compile.sh la relit : un firmware déjà à jour (même
# version, même ROM) est conservé tel quel ; un firmware fait par une autre
# version est proposé à la mise à jour.
#
# Interface :
#   - Interactif quand le script est lancé dans un terminal : liste des ROMs
#     avec leur état (nouvelle, à jour, version vA -> vB, ROM modifiée,
#     version inconnue), choix des jeux à traiter, puis confirmation pour
#     reconstruire les firmwares d'une autre version.
#   - Sinon (script, tube) : traite tout ce qui a à faire.
#       ALL=1       comme si on répondait « tout » dans le menu
#       UPDATE=1    reconstruit aussi les firmwares d'une autre version
#       FORCE=1     refait même ceux déjà à jour
#
#   PLAY=1        ouvre aussi une fenêtre pour jouer soi-même (défaut 0 : non)
#   AUTO=1        (défaut) chaque ROM est jouée sans fenêtre ni son, avec des
#                 touches aléatoires (Start de temps en temps), à pleine
#                 vitesse : AUTO_SEEDS parties en parallèle (défaut : une par
#                 cœur) de AUTO_FRAMES images (défaut 1 080 000 : 5 h de jeu
#                 à 60 fps ; la graine 1 trois fois plus, AUTO_LONG_FRAMES),
#                 AUTO_SECONDS secondes de temps réel au plus (défaut 300 :
#                 le vrai garde-fou), pour trouver le code chaud à compiler.
#                 Les parties automatiques de plusieurs ROMs passent une à
#                 une (elles prennent tous les cœurs) ; le reste du lot
#                 continue en parallèle.
#   JOBS=n        ROMs traitées en parallèle (défaut : cœurs / 4 ; 1 avec PLAY=1)
#   AUTO_REDO=1   refaire la partie automatique même si elle est déjà enregistrée
#                 (sinon seules les graines manquantes sont jouées)
#   DEMO=1        (GB, défaut) une partie de plus sans aucune touche, pendant
#                 DEMO_FRAMES images (défaut 108000 : 30 mn de jeu),
#                 AUTO_SECONDS de temps réel au plus : titre, puis les démos
#                 que beaucoup de jeux jouent seuls dans plusieurs niveaux
#                 (coverage-demo) ; les parties au hasard appuient sur Start
#                 et ne les voient jamais.
#   REPLAY=1      rejouer les parties à la main enregistrées (profils à jour)
#   EXPLORERS=n   (toutes plateformes, défaut 4) explorations guidées par la
#                 couverture (--explore des harnais, targets/common/host/
#                 m0_explore.h) : un joueur au hasard qui garde l'état des
#                 parties où il a vu du code ou des données nouveaux et y
#                 repart, pour atteindre ce qu'un joueur aveugle ne voit pas.
#                 EXPLORE_FRAMES images chacune (défaut AUTO_SECONDS x 20000
#                 en GB, x 6000 en SG/GG/SMS) ; 0 = aucune.  Les profils
#                 enregistrés sans exploration la reçoivent au lancement
#                 suivant.  SG_EXPLORERS et SG_EXPLORE_FRAMES : anciens noms.
#   AI=1          (GB) ajoute AI_GAMES parties (défaut 1) jouées par un modèle de
#                 décision local servi par Ollama (AI_MODEL, défaut tev1:0.8b ;
#                 AI_URL, défaut http://localhost:11434) : tools/ai-play/ai_play.py
#                 lui décrit l'écran (tuiles, défilement, sprites, sprite qui
#                 suit la croix) et joue ce qu'il choisit, AI_SECONDS secondes
#                 (défaut 120).  Enregistrées comme les autres (coverage-ai<n>),
#                 rejouables à l'identique ; AI_REDO=1 pour les refaire.
#   FIRMWARE_COMPILER=gcc|clang (défaut gcc) : compilateur C/C++ du firmware
#   PACK=1 (défaut)
#                 données de ROM peu lues compressées (LZSS) dans la flash au
#                 lieu d'être lues sur la carte SD : plus de place pour le code
#                 compilé, pas de .GB à copier (retombe sur la SD ou le mode
#                 normal si la ROM compressée ne tient pas)
#   STATIC=0      ne pas ajouter le code trouvé par analyse statique
#                 (par défaut il complète les parties, dans la limite de la flash)
#   OVERCLOCK=0   firmwares démarrant à 48 MHz ; par défaut (1) ils démarrent
#                 overclockés : 70 MHz sur la META (mesuré stable sur une
#                 console), 72 sur la Pokitto (hors spécification : 48 MHz
#                 pour le SAMD21, 50 pour le LPC11U68 ; SPI carte redivisé à
#                 18 MHz sur la Pokitto ; le menu « Processeur » bascule à
#                 l'exécution ; POKITTO_HC est l'ancien nom)
#   STORAGE=0     (GG/SMS plus grands que la flash) ne pas simuler le cache
#                 carte : les secteurs gardés en flash sont alors choisis par
#                 fréquence de lecture.  Par défaut, les parties enregistrées
#                 sont rejouées en trace d'accès ROM (out/romtrace/), le cache
#                 du firmware est simulé dessus et la taille des blocs gardés
#                 en flash validée sur des parties tenues à l'écart
#                 (profiles/<src>/<Jeu>/storage.json, rom.rank).
#   SG_SYNC_CYCLES=N  (SG-1000/GG/SMS) le code natif regarde aussi les
#                 interruptions dès que N cycles Z80 ont passé depuis la
#                 dernière fois (défaut 0 : en tête de boucle seulement ;
#                 40 bornait la latence de l'eau de la démo de Ristar, seul
#                 bug visible)
#   RELEASE=1     (META) firmware sans port USB (meta_usb.c, -420 octets de
#                 RAM) : ni meta-usb.py, ni l'entrée du bootloader par le
#                 toucher 1200 bauds — reflasher demande le bootloader
#                 autrement (double appui sur reset, à vérifier sur la
#                 console).  La garde du cache carte passe de 2000 à 2400
#                 octets libres : quatre secteurs, 16 blocs de 128 au lieu de
#                 12 (le plus profond mesuré au filigrane est de 1640 octets,
#                 +128 non peints sous main, USB comprise donc majorée pour un
#                 firmware sans USB ; marge ~620 octets).
#   SOURCE=1      le code source prêt à compiler au lieu du firmware : chaque
#                 zip contient source/ (le projet généré ; platformio.ini
#                 porte les options de ce build, BUILD.txt dit la commande)
#                 et les fichiers de la carte SD (ROM lue sur la carte,
#                 .SAV, états).  Destination par défaut ./roms/<console>-source.
#                 Le firmware est tout de même construit : le remplissage de
#                 la flash se règle sur ses tailles, la source livrée tient.
#   AUDIO_DMA=0|1 son de la META par interruption (0, défaut pour toutes les
#                 consoles : moins de RAM, laissée au cache carte) ou par DMA
#                 (1).  Un jeu déjà à jour n'est pas refait : FORCE=1 après
#                 avoir changé AUDIO_DMA.
#
# Sans aucune partie à la main, la partie automatique et l'analyse statique
# suffisent à compiler l'essentiel ; le code est gardé du plus exécuté au
# moins exécuté jusqu'à remplir la flash.
#
# Jeux à pile (sauvegarde) ou à grande RAM de cartouche : le fichier .SAV
# (ex. ZERDNODE.SAV) est créé une fois ; la console y garde la progression.
#
# Profils : un dossier par ROM, commun à toutes les consoles cibles,
# profiles/<source>/<Jeu>/ dans le dépôt (PROFILES_DIR pour un autre
# endroit ; les anciens <dossier_roms>/profiles/<sha1>/ y sont déplacés au
# premier passage).  Chaque partie (aléatoire, exploration ou à la main) y
# est sauvée : entrées (.input), code exécuté (.cov) et lectures de données
# (.prof).  Les lancements suivants les réutilisent au lieu de rejouer, et
# toutes sont cumulées : rejouer un jeu à la main étend le code compilé.
# Après chaque firmware, le dossier reçoit aussi :
#   profile.json  parties (images, instructions, apport propre de chacune),
#                 carte de la ROM (code / données lues, par blocs de 256
#                 octets), instructions les plus chaudes, derniers builds de
#                 chaque console, cycles par fonction des runs gm0
#   play.sh       une session dans gm0 (./play.sh [meta|pokitto|espboy]) :
#                 gm0 enregistre les boutons que reçoit le jeu, la session
#                 devient coverage-gm0-<date>.input, rejouée dans le harnais
#                 PC (code exécuté) ; le lancement suivant la compile (le jeu
#                 passe « profil enrichi ») et la rejoue chaque fois que le
#                 profil est refait (REPLAY=1 : toutes) ; profil de cycles
#                 EMU_PROF ; ./play.sh record (GB) : une partie dans la
#                 fenêtre du harnais ; ./play.sh -h
# Ils sont faits par tools/profile/m0profile.py et tools/profile/play.sh.
# Une ROM en échec n'arrête pas le lot ; le bilan est affiché à la fin.
set -uo pipefail

TARGET="${TARGET:-}"
if [ -z "$TARGET" ] && [ -t 0 ]; then
  printf 'Console : méta (entrée), p = pokitto, d = les deux : '
  read -r t
  case "$t" in p|pokitto) TARGET=pokitto ;; d|deux|both|all) TARGET=both ;;
    e|espboy) TARGET=espboy ;; pc) TARGET=pc ;; odroid) TARGET=odroid ;; aka) TARGET=aka ;;
    *) TARGET=meta ;; esac
fi
TARGET="${TARGET:-meta}"

# Plateforme source, demandée avant tout : le balayage de tous les répertoires
# de ROMs (et le sha1 de chacune) est long.  SRC_SYSTEM court-circuite la
# question (scripts) ; vide = toutes les plateformes.
SRC_SYSTEM="${SRC_SYSTEM:-}"
if [ -z "$SRC_SYSTEM" ] && [ -t 0 ]; then
  printf 'Plateforme des ROMs : gb (entrée), sg, gg, sms, t = toutes : '
  read -r s
  case "$s" in sg|SG) SRC_SYSTEM=sg ;; gg|GG) SRC_SYSTEM=gg ;; sms|SMS) SRC_SYSTEM=sms ;;
               t|T|tout|all|toutes) SRC_SYSTEM="" ;; *) SRC_SYSTEM=gb ;; esac
fi
export SRC_SYSTEM

# Horloge des consoles, demandée une fois : overclock (par défaut : 70 MHz
# sur la META, 72 sur la Pokitto) ou 48 MHz (nominal).  OVERCLOCK (ou
# POKITTO_HC, son ancien nom) court-circuite la question (scripts).
OVERCLOCK="${OVERCLOCK:-${POKITTO_HC:-}}"
if [ -z "$OVERCLOCK" ] && [ -t 0 ]; then
  printf 'Horloge des consoles : overclock 70/72 (entrée) ou 48 : '
  read -r h
  case "$h" in 48) OVERCLOCK=0 ;; *) OVERCLOCK=1 ;; esac
fi
OVERCLOCK="${OVERCLOCK:-1}"
export OVERCLOCK POKITTO_HC="$OVERCLOCK"
export OVERCLOCK_FLAG="-DMETA_OVERCLOCK=$OVERCLOCK${AUDIO_DMA:+ -DMETA_AUDIO_DMA=$AUDIO_DMA}"
RELEASE="${RELEASE:-0}"
export RELEASE
RELEASE_FLAG=""
if [ "$RELEASE" = 1 ]; then
  if [ "$TARGET" = meta ]; then RELEASE_FLAG="-DMETA_USB_MODE=2" # SD without serial debug
  else RELEASE_FLAG="-DMETA_USB_MODE=0"; fi
fi
# CACHE_DYN=0 : caches de taille fixe (GBL_CACHE_DYN, GG_CACHE_DYN ; META).
[ "${CACHE_DYN:-1}" = 0 ] && export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }-DGBL_CACHE_DYN=0 -DGG_CACHE_DYN=0"
export RELEASE_FLAG
if [ "$TARGET" = both ] || [ "$TARGET" = all ]; then
  # Une exécution par console.  La sélection des jeux et la confirmation des
  # mises à jour sont demandées une seule fois (pass de capture ci-dessous,
  # état de la META) puis transmises aux deux passes via CHOICE/CHOICE_UPD.
  rc=0
  src="${1:-./roms}"
  if [ -n "${2:-}" ]; then mkdir -p "$2/meta" "$2/pokitto"; dm="$2/meta"; dp="$2/pokitto"; else dm=""; dp=""; fi
  if [ -t 0 ] && [ "${ALL:-0}" != 1 ] && [ -z "${CHOICE:-}" ]; then
    tf=$(mktemp)
    if TARGET=meta CHOICE_CAPTURE=1 CHOICE_CAPTURE_OUT="$tf" "$0" "$src" "$dm"; then
      if [ -s "$tf" ]; then
        CHOICE=$(sed -n 's/^CHOICE=//p' "$tf")
        CHOICE_UPD=$(sed -n 's/^UPDANS=//p' "$tf")
        export CHOICE CHOICE_UPD
      else
        rm -f "$tf"; exit 0 # « q » : rien à faire
      fi
    fi
    rm -f "$tf"
  fi
  for t in meta pokitto; do
    echo; echo "################################## $t ##################################"
    if [ "$t" = meta ]; then TARGET=meta "$0" "$src" "$dm" || rc=1
    else TARGET=pokitto "$0" "$src" "$dp" || rc=1; fi
  done
  exit $rc
fi
case "$TARGET" in
  meta|pokitto|espboy|pc|odroid|aka) ;;
  *) echo "TARGET=$TARGET inconnu (meta, pokitto, espboy, pc, odroid, aka ou both)."; exit 1 ;;
esac
SRC_DIR="${1:-./roms}"
SOURCE="${SOURCE:-0}"
if [ "$SOURCE" = 1 ]; then DEST_DIR="${2:-./roms/$TARGET-source}"; else DEST_DIR="${2:-./roms/$TARGET}"; fi
# Projets générés : out/<jeu> pour la META, out/pokitto/<jeu> pour la Pokitto
# (les deux peuvent coexister).
if [ "$TARGET" != meta ]; then OUT_ROOT="out/$TARGET"; else OUT_ROOT="out"; fi
PLAY="${PLAY:-0}"
STATIC="${STATIC:-1}"
AUTO="${AUTO:-1}"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# Précalcul : AUTO_SEEDS parties aléatoires en parallèle par ROM (une par
# cœur), chacune jouée à pleine vitesse jusqu'à AUTO_FRAMES images ou
# AUTO_SECONDS secondes de temps réel, la première des deux bornes (3 000 à
# 12 000 frames par seconde selon le jeu).
AUTO_SEEDS="${AUTO_SEEDS:-$NPROC}"
AUTO_SECONDS="${AUTO_SECONDS:-300}"
# ROMs traitées en parallèle (1 avec PLAY=1) ; journal de chacune dans
# out/<jeu>/compile.log.
JOBS="${JOBS:-$(( NPROC / 4 > 0 ? NPROC / 4 : 1 ))}"
AUTO_REDO="${AUTO_REDO:-0}"
DEMO="${DEMO:-1}"
DEMO_FRAMES="${DEMO_FRAMES:-108000}" # sans touche : 30 mn de jeu à 60 fps
case "$DEMO_FRAMES" in ''|*[!0-9]*) echo "[x] DEMO_FRAMES doit être un entier."; exit 1 ;; esac
REPLAY="${REPLAY:-0}"
EXPLORERS="${EXPLORERS:-${SG_EXPLORERS:-4}}"
EXPLORE_FRAMES="${EXPLORE_FRAMES:-${SG_EXPLORE_FRAMES:-}}" # vide : selon la plateforme
case "$EXPLORERS" in ''|*[!0-9]*) echo "[x] EXPLORERS doit être un entier."; exit 1 ;; esac
case "$EXPLORE_FRAMES" in *[!0-9]*) echo "[x] EXPLORE_FRAMES doit être un entier."; exit 1 ;; esac
AI="${AI:-0}"
AI_MODEL="${AI_MODEL:-tev1:0.8b}"
AI_URL="${AI_URL:-http://localhost:11434}"
AI_GAMES="${AI_GAMES:-1}"
AI_SECONDS="${AI_SECONDS:-120}"
AI_REDO="${AI_REDO:-0}"
FIRMWARE_COMPILER="${FIRMWARE_COMPILER:-gcc}"
case "$FIRMWARE_COMPILER" in gcc|clang) ;; *) echo "FIRMWARE_COMPILER doit être gcc ou clang."; exit 1 ;; esac
PACK="${PACK:-1}"  # auto : packe quand ça tient, retombe sur la carte SD sinon (PACK=0 pour désactiver)
ALL="${ALL:-0}"
UPDATE="${UPDATE:-0}"
FORCE="${FORCE:-0}"
NO_ICONS="${NO_ICONS:-0}"
# OVERCLOCK (1 par défaut) : firmwares démarrant overclockés, 70 MHz sur la
# META, 72 sur la Pokitto (hors spécification : voir gbl_profile.h,
# META_OVERCLOCK) ; 0 : 48 MHz.  Le menu « Processeur » garde la bascule.
OVERCLOCK="${OVERCLOCK:-${POKITTO_HC:-1}}"
export OVERCLOCK POKITTO_HC="$OVERCLOCK"
export OVERCLOCK_FLAG="-DMETA_OVERCLOCK=$OVERCLOCK${AUDIO_DMA:+ -DMETA_AUDIO_DMA=$AUDIO_DMA}"
RELEASE="${RELEASE:-0}"
export RELEASE
RELEASE_FLAG=""
if [ "$RELEASE" = 1 ]; then
  if [ "$TARGET" = meta ]; then RELEASE_FLAG="-DMETA_USB_MODE=2" # SD without serial debug
  else RELEASE_FLAG="-DMETA_USB_MODE=0"; fi
fi
export RELEASE_FLAG
if [ "$TARGET" = espboy ]; then
  PACK=0
  FIRMWARE_COMPILER=gcc
  OVERCLOCK=0
  OVERCLOCK_FLAG=""
fi
PIO_ENV="$TARGET"
# m0recomp is the console-neutral CLI; keep the historical binary as fallback.
GBRECOMP="./build/bin/gbrecomp"
[ -x ./build/bin/m0recomp ] && GBRECOMP="./build/bin/m0recomp"
# Profils (parties enregistrées, carte de la ROM, builds, runs gm0) : un
# dossier par ROM, commun à toutes les consoles cibles,
# <PROFILES_DIR>/<source>/<Jeu>/ (voir profile_store).  SESSIONS_DIR : ancien nom.
REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
PROFILES_DIR="${PROFILES_DIR:-${SESSIONS_DIR:-$REPO_DIR/profiles}}"
mkdir -p "$PROFILES_DIR" && PROFILES_DIR="$(cd "$PROFILES_DIR" && pwd)"
PROFILE_TOOL="$REPO_DIR/tools/profile/m0profile.py"
ICON_CACHE="$DEST_DIR/.cache/icons"
THUMB_BASE="https://thumbnails.libretro.com"

if [ ! -x "$GBRECOMP" ]; then
  echo "gbrecomp introuvable : compile d'abord l'outil (cmake -G Ninja -B build . && ninja -C build)."
  exit 1
fi
# Version de l'édition de la console (targets/meta/VERSION pour la META,
# targets/consoles/pokitto/VERSION pour la Pokitto), celle inscrite dans
# les firmwares « gbrecomp-<console> x.y.z » ; un gbrecomp plus ancien ne
# connaît que --version-json.
CUR_VERSION=$("$GBRECOMP" --"$TARGET"-version 2>/dev/null | head -n1)
case "$CUR_VERSION" in
  [0-9]*) ;;
  *) CUR_VERSION=$("$GBRECOMP" --version-json | sed -n 's/.*"version":"\([^"]*\)".*/\1/p') ;;
esac
[ -n "$CUR_VERSION" ] || CUR_VERSION="inconnue"
mkdir -p "$DEST_DIR"

sha1_of() { (sha1sum "$1" 2>/dev/null || shasum -a 1 "$1") | cut -d' ' -f1; }

# « Zerd no Densetsu (J) [!] » -> « Zerd no Densetsu »
clean_name() {
  local n="${1%.*}"
  n=$(printf '%s' "$n" | sed -E 's/[[(][^])]*[])]//g; s/[[:space:]]+/ /g;
                                s/^[ ._-]+//; s/[ ._-]+$//')
  printf '%s' "${n:-${1%.*}}"
}

# Caractères interdits sur une carte FAT -> espace.
sd_safe() {
  printf '%s' "$1" | sed -E 's#[\\/:*?"<>|]# #g; s/[[:space:]]+/ /g; s/^ //; s/ $//'
}

# Sous-dossier de sortie d'un système : gb, sg, gg ou sms.
sys_dir() {
  case "$1" in
    gameboy) printf 'gb' ;;
    sg1000) printf 'sg' ;;
    *) printf '%s' "$1" ;;
  esac
}

# Longueur des noms de jeux, pour le loader et le bootloader de la META
# (le .bin porte le nom du jeu, comme le dossier) :
#   - le loader (github.com/Gamebuino/Loader-META) garde « /Nom » dans
#     40 octets et construit « /Nom/Nom.bin » dans 80 sans contrôle : au-delà
#     de 36 caractères le jeu n'est pas listé (vu avec 38) ;
#   - le bootloader, qui charge ce chemin, n'en prend pas 72 (un nom de 33
#     ne se lance pas) mais prend 62 (28) : 64 octets probablement, d'où
#     28 caractères au plus.
# Le loader essaie « /Nom/Nom.bin », puis prend le premier .bin du dossier :
# un .bin court garde le chemin passé au bootloader loin de sa limite.
# Plus long : voir les étapes de loader_name (article, abréviations,
# séparateurs, coupe sur un mot ; le début du sous-titre reste : il
# distingue les jeux d'une même série).
LOADER_NAME_MAX=28
loader_name() {
  local LC_ALL=C s="$1" max="${2:-$LOADER_NAME_MAX}"
  [ "${#s}" -le "$max" ] && { printf '%s' "$s"; return; }
  # 1. « Titre, The - X » -> « The Titre - X ».
  s=$(printf '%s' "$s" | sed -E 's/^([^,]+), (The|A|An|Le|La|Les|L'"'"'|Die|Der|Das) - /\2 \1 - /; s/^([^,]+), (The|A|An|Le|La|Les|Die|Der|Das)$/\2 \1/')
  # 2. Abréviations courantes.
  [ "${#s}" -gt "$max" ] && s=$(printf '%s' "$s" | sed -E '
    s/Teenage Mutant Ninja Turtles/TMNT/; s/Teenage Mutant Hero Turtles/TMHT/
    s/(The )?Legend of Zelda/Zelda/; s/ No\. ([0-9])/ \1/g
    s/Championship/Champ./g; s/International/Intl/g; s/Professional/Pro/g
    s/Adventures/Adv./g; s/Adventure/Adv./g; s/Productivity/Prod./g
    s/Edition/Ed./g; s/Collection/Coll./g; s/Tournament/Tourn./g
    s/Baseball/B.ball/g; s/Basketball/B.ball/g; s/Football/F.ball/g')
  # 3. « - » devient une espace, 4. plus d'article en tête.
  [ "${#s}" -gt "$max" ] && s="${s// - / }"
  [ "${#s}" -gt "$max" ] && s="${s#The }"
  # 5. Coupe sur un mot, sans mot de liaison pendant (« Asteroids & »).
  if [ "${#s}" -gt "$max" ]; then
    s="${s:0:$max}"
    [[ "$s" == *" "* ]] && s="${s% *}"
    s=$(printf '%s' "$s" | sed -E ':a; s/[ ,.:-]+$//; s/ (&|and|of|the|The|a|A|an|An|vs|vs\.|no|o|to|de|wa|ga|with|in|Ed|Ver)$//; ta')
  fi
  printf '%s' "$s" | sed -E 's/[ ,:-]+$//'
}

# Nom court 8.3 des fichiers d'un jeu sur la carte (<NOM>.BIN, .GB/.GG/.SMS,
# .SAV, .STA) : $1 nom du jeu, $2 SHA-1 de la ROM.  Le loader prend le
# premier .bin du dossier quand « /Jeu/Jeu.bin » manque.
sd_name_of() {
  local LC_ALL=C s
  s=$(printf '%s' "$1" | tr -dc 'A-Za-z0-9' | tr 'a-z' 'A-Z')
  [ -n "$s" ] || s=GAME
  if [ "${#s}" -gt 8 ]; then s="${s:0:6}$(printf '%s' "$2" | cut -c1-2 | tr 'a-z' 'A-Z')"; fi
  printf '%s' "$s"
}

url_quote() {
  python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$1" \
    2>/dev/null || printf '%s' "$1" | sed 's/ /%20/g'
}

json_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
sh_quote() { printf "'%s'" "${1//\'/\'\\\'\'}"; }

# ---- Profils ---------------------------------------------------------------
# Un dossier par ROM, partagé par la META, la Pokitto et l'ESPboy :
# <PROFILES_DIR>/<source>/<Jeu>/ (source : gb, sg, gg ou sms ; une autre ROM
# du même nom y prend « <Jeu> [<sha1 court>] »).  Il contient :
#   rom.env                  ROM, SHA-1, système, nom (lu par play.sh)
#   coverage-<id>.cov/.input parties (code exécuté, touches), romdata-<id>.prof
#                            lectures de données (GB) ; <id> : auto<n>, rand<n>,
#                            explore<n>, idle, demo, ai<n>, ou la date d'une
#                            partie à la main
#   build-<console>.txt      dernier firmware de chaque console (zip, journal)
#   <console>.syms.tsv       symboles de ce firmware (profil de cycles gm0)
#   gm0/                     runs gm0 (<console>-<date>.cyc) et leur carte SD
#   play.sh                  le jeu dans gm0, ou une partie à la main (record)
#   profile.json             statistiques des parties, carte de la ROM, builds
#                            et cycles gm0 (tools/profile/m0profile.py)

profile_set() { # $1 dossier, $2 clé, $3 valeur : rom.env
  { grep -v "^$2=" "$1/rom.env" 2>/dev/null; printf '%s=%s\n' "$2" "$(sh_quote "$3")"; } > "$1/rom.env.tmp" &&
    mv "$1/rom.env.tmp" "$1/rom.env"
}

# Anciens rangements, repris une fois (déplacés, jamais écrasés) :
# <dossier_roms>/profiles/<sha1>/[<système>/] et <PROFILES_DIR>/<sha1>/.
profile_import() { # $1 dossier, $2 système, $3 sha1, $4 dossier de la ROM
  local src f n=0
  for src in "$4/profiles/$3/$2" "$4/profiles/$3" "$PROFILES_DIR/$3"; do
    [ -d "$src" ] || continue
    for f in "$src"/coverage-* "$src"/romdata-* "$src"/.auto-profile-config; do
      [ -f "$f" ] && [ ! -e "$1/${f##*/}" ] && mv "$f" "$1/" && n=$((n + 1))
    done
    [ -z "$(ls -A "$src" | grep -vx JEU.txt)" ] && rm -f "$src/JEU.txt" && rmdir "$src"
  done
  rmdir "$4/profiles" 2> /dev/null
  [ $n -eq 0 ] || echo "[i] Profil : $n fichier(s) repris de l'ancien rangement -> $1"
  return 0
}

# Dossier du profil d'une ROM, créé : variable store de l'appelant.
profile_store() { # $1 système, $2 nom du jeu, $3 sha1, $4 ROM
  local base="$PROFILES_DIR/$(sys_dir "$1")" old
  store="$base/$2"
  old=$(sed -n "s/^SHA1='\(.*\)'$/\1/p" "$store/rom.env" 2>/dev/null)
  [ -n "$old" ] && [ "$old" != "$3" ] && store="$base/$2 [${3:0:8}]"
  mkdir -p "$store" || return 1
  profile_import "$store" "$1" "$3" "$(dirname "$4")"
  profile_set "$store" NAME "$2" && profile_set "$store" SYSTEM "$1" && profile_set "$store" SHA1 "$3" &&
    profile_set "$store" ROM "$(readlink -f "$4")" || return 1
  printf '#!/usr/bin/env bash\n# %s (%s), écrit par compile.sh : ./play.sh -h\nexec %s "$(cd "$(dirname "$0")" && pwd)" "$@"\n' \
    "$2" "$(basename "$4")" "$(sh_quote "$REPO_DIR/tools/profile/play.sh")" > "$store/play.sh" && chmod +x "$store/play.sh"
}

# Firmware d'une console terminé : build-<console>.txt, symboles, profile.json.
profile_build() { # $1 dossier, $2 zip, $3 ELF ou "", $4 journal, $5... clé=valeur
  local store="$1" zip="$2" elf="$3" log="$4"; shift 4
  [ "$SOURCE" = 1 ] && return 0 # pas de firmware dans le zip
  zip="$(cd "$(dirname "$zip")" && pwd)/$(basename "$zip")"
  { printf 'zip=%s\nversion=%s\ndate=%s\ncompiler=%s\noverclock=%s\n' "$zip" "$CUR_VERSION" "$(date '+%Y-%m-%d %H:%M')" \
      "$FIRMWARE_COMPILER" "$OVERCLOCK"
    [ $# -eq 0 ] || printf '%s\n' "$@"
    echo ---
    printf '%s\n' "$log"; } > "$store/build-$TARGET.txt"
  [ -f "$PROFILE_TOOL" ] || return 0
  rm -f "$store/$TARGET.syms.tsv"
  [ -n "$elf" ] && [ -f "$elf" ] && python3 "$PROFILE_TOOL" syms "$elf" "$store/$TARGET.syms.tsv"
  python3 "$PROFILE_TOOL" report "$store" || true
  echo "[i] Jouer dans gm0 / enrichir le profil : $store/play.sh"
}

# Version de gbrecomp embarquée dans un firmware (voir gb_main.cpp) :
# « gbrecomp-meta x.y.z » ou « gbrecomp-pokitto x.y.z ».
firmware_version() {
  grep -aoE "(gbrecomp-$TARGET|m0boy-sg1000-$TARGET|m0boy-gg-$TARGET|m0boy-pico8-$TARGET) [0-9A-Za-z._+-]{1,32}" "$1" 2>/dev/null \
    | head -n1 | cut -d' ' -f2
}

# Sortie d'un jeu : un .zip par jeu, le répertoire du jeu dedans (prêt à
# déposer sur la carte SD).
zip_game() { # $1 dossier du jeu, $2 zip de destination
  python3 - "$1" "$2" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
name = os.path.basename(os.path.normpath(src))
tmp = dst + ".tmp"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.join(name, os.path.relpath(p, src)))
os.replace(tmp, dst)
PY
}

# SOURCE=1 : le projet généré remplace le firmware dans le dossier du jeu.
# Les options que ce script passe au build par l'environnement vont dans
# platformio.ini (build_flags y est la dernière clé) ; la Pokitto, construite
# par cmake ou make, reçoit les siennes dans BUILD.txt.
export_source() { # $1 projet généré, $2 dossier du jeu, $3 options pio de ce build, $4 commande de build
  local src="$2/source" f
  rm -rf "$src" && mkdir -p "$src" || return 1
  tar -C "$1" --exclude=./.pio --exclude=./build --exclude=./build-clang --exclude='./host/build*' \
    --exclude=./compile.log -cf - . | tar -C "$src" -xf - || return 1
  if [ -n "$3" ] && [ -f "$src/platformio.ini" ]; then
    [ -z "$(tail -c1 "$src/platformio.ini")" ] || echo >> "$src/platformio.ini"
    for f in $3; do printf '    %s\n' "$f"; done >> "$src/platformio.ini"
  fi
  printf '%s\n' "$4" > "$src/BUILD.txt"
  rm -f "$2"/*.BIN "$2/firmware.bin" "$2/firmware.sym" "$2/FLASH.txt" "$2/README.md" "$2/$(basename "$2").bin"
  if [ "$TARGET" = aka ]; then rm -f "$2/bootloader.bin" "$2/partitions.bin" "$2/boot_app0.bin"; fi
  echo "[i] Source prête à compiler : $(basename "$2")/source ($4)"
}

# AKA loader layout and gm0 symbol sidecar. The serial-flash files also make
# the archive usable on a board without the AKA launcher installed.
aka_package() { # project, game directory, title
  local project="$1" game="$2" title="$3" built="$1/.pio/build/aka"
  local boot_app="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
  for file in firmware.bin firmware.sym bootloader.bin partitions.bin; do
    cp "$built/$file" "$game/$file" || return 1
  done
  cp "$boot_app" "$game/boot_app0.bin" || return 1
  # the HuCard rides in the flash's rom partition (0x380000), outside the app slot
  local romline=""
  if [ -f "$project/rom.bin" ]; then
    cp "$project/rom.bin" "$game/rom.bin" || return 1
    romline=" 0x380000 rom.bin"
  fi
  cp "$project/README.md" "$game/README.md" || return 1
  python3 - "$game/meta.json" "$title" "$CUR_VERSION" <<'PY'
import datetime, json, sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps(dict(title=sys.argv[2], author='m0boy',
    description='Converted with m0boy', version=sys.argv[3], date=str(datetime.date.today())),
    ensure_ascii=False, indent=2)+'\n')
PY
  printf 'esptool.py --chip esp32s3 --port PORT write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin%s\n' "${romline}" > "$game/FLASH.txt"
}

# ESPboy : la pile de continuation (CONT_STACKSIZE, 12 Kio par défaut) cède
# au tas ce qui lui manque pour garder 4 Kio libres, sans descendre sous
# 6 Kio.  platformio.ini étant régénéré à chaque passe, la taille retenue
# pour le jeu (esp_stack, variable de l'appelant) y est remise avant chaque
# build.  Remplit build_log ; code de pio run.
espboy_pio_run() { # $1 projet, $2 options de build
  local ini="$1/platformio.ini" stack used free cut rc tries=0
  while :; do
    [ -n "${esp_stack:-}" ] && sed -i "s/-DCONT_STACKSIZE=[0-9]*/-DCONT_STACKSIZE=$esp_stack/" "$ini"
    stack=$(sed -n 's/.*-DCONT_STACKSIZE=\([0-9]*\).*/\1/p' "$ini")
    build_log=$(FIRMWARE_COMPILER="$FIRMWARE_COMPILER" PLATFORMIO_BUILD_FLAGS="$2" pio run -d "$1" 2>&1); rc=$?
    used=$(sed -n 's/^RAM:.*(used \([0-9]*\) bytes from 81920 bytes).*/\1/p' <<<"$build_log")
    if [ $rc -eq 0 ] && [ -n "$used" ]; then
      free=$((81920 - used)); [ $free -ge 4096 ] && return 0
      cut=$(( (4096 - free + 511) / 512 * 512 ))
    elif grep -q "dram0_0_seg" <<<"$build_log"; then
      free=0; cut=2048 # RAM dépassée d'une quantité que l'éditeur de liens ne dit pas
    else
      return $rc
    fi
    { [ -n "$stack" ] && [ $((stack - cut)) -ge 6144 ] && [ $tries -lt 6 ]; } || return $rc
    esp_stack=$((stack - cut)); tries=$((tries + 1))
    echo "[i] Pile ESPboy : $esp_stack octets (tas libre : $free octets)"
  done
}

unzip_game() { # $1 zip, $2 dossier de destination
  python3 - "$1" "$2" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
}

# Un membre du zip d'un jeu, trouvé par base de nom (sans la casse) :
# contenu binaire sur stdout, code 1 si absent.
zip_member() { # $1 zip, $2 base du nom
  python3 - "$1" "$2" <<'PY'
import os, sys, zipfile
try:
    z = zipfile.ZipFile(sys.argv[1])
except (OSError, zipfile.BadZipFile):
    sys.exit(1)
want = sys.argv[2].lower()
for n in z.namelist():
    if os.path.basename(n).lower() == want:
        sys.stdout.buffer.write(z.read(n))
        sys.exit(0)
sys.exit(1)
PY
}

# Outil de conversion PNG -> BMP (le loader attend des BMP 24 bits).
IM_BIN=none
command -v magick > /dev/null 2>&1 && IM_BIN=magick
[ "$IM_BIN" = none ] && command -v convert > /dev/null 2>&1 && IM_BIN=convert
HAVE_PIL=0
python3 -c 'import PIL' > /dev/null 2>&1 && HAVE_PIL=1

make_bmp() { # $1 png, $2 bmp, $3 largeur, $4 hauteur
  if [ "$IM_BIN" != none ]; then
    "$IM_BIN" "$1" -thumbnail "$3x$4^" -gravity center -extent "$3x$4" \
      -strip -type truecolor "BMP3:$2" > /dev/null 2>&1
  elif [ "$HAVE_PIL" = 1 ]; then
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys
from PIL import Image, ImageOps
im = Image.open(sys.argv[1]).convert("RGB")
ImageOps.fit(im, (int(sys.argv[3]), int(sys.argv[4]))).save(sys.argv[2], "BMP")
PY
  else
    return 1
  fi
}

# Nom (décodé <TAB> encodé) de chaque vignette d'un catalogue libretro,
# mis en cache : les fichiers Named_Titles gardent les tags No-Intro
# (« Tetris (World) (Rev 1).png »), un nom nettoyé seul ne suffit pas.
thumb_listing() { # $1 = gbc, sg, gg, sms ou "" -> fichier cache, ou échec
  local f url
  if [ "$1" = sg ]; then
    f="$ICON_CACHE/listing-sg.txt"
    url="$THUMB_BASE/Sega%20-%20SG-1000/Named_Titles/"
  elif [ "$1" = gg ]; then
    f="$ICON_CACHE/listing-gg.txt"
    url="$THUMB_BASE/Sega%20-%20Game%20Gear/Named_Titles/"
  elif [ "$1" = sms ]; then
    f="$ICON_CACHE/listing-sms.txt"
    url="$THUMB_BASE/Sega%20-%20Master%20System%20-%20Mark%20III/Named_Titles/"
  elif [ "$1" = gbc ]; then
    f="$ICON_CACHE/listing-gbc.txt"
    url="$THUMB_BASE/Nintendo%20-%20Game%20Boy%20Color/Named_Titles/"
  else
    f="$ICON_CACHE/listing-gb.txt"
    url="$THUMB_BASE/Nintendo%20-%20Game%20Boy/Named_Titles/"
  fi
  if [ ! -s "$f" ]; then
    mkdir -p "$ICON_CACHE"
    if command -v curl > /dev/null 2>&1; then
      curl -fsSL --max-time 60 -o "$f.raw" "$url" || { rm -f "$f.raw"; return 1; }
    elif command -v wget > /dev/null 2>&1; then
      wget -q -T 60 -O "$f.raw" "$url" || { rm -f "$f.raw"; return 1; }
    else
      return 1
    fi
    python3 - "$f.raw" "$f" <<'PY'
import re, sys, urllib.parse
raw, out = sys.argv[1], sys.argv[2]
text = open(raw, encoding="utf-8", errors="replace").read()
with open(out, "w", encoding="utf-8") as f:
    for enc in re.findall(r'href="([^"]+\.png)"', text):
        f.write(urllib.parse.unquote(enc)[:-4] + "\t" + enc[:-4] + "\n")
PY
    rm -f "$f.raw"
    [ -s "$f" ] || return 1
  fi
  printf '%s' "$f"
}

# Meilleure vignette pour un nom nettoyé : sans les tags (région, révision,
# [!]) ; (World) puis (USA, Europe) préférés, révision la plus haute.
find_thumb() { # $1 nom du jeu, $2 = gbc ou "" -> nom de fichier encodé
  local list
  list=$(thumb_listing "$2") || return 1
  python3 - "$list" "$1" <<'PY'
import re, sys
def norm(s):
    return " ".join(re.sub(r"\([^)]*\)|\[[^\]]*\]", " ", s).split()).casefold()
target = norm(sys.argv[2])
best, best_score = None, float("-inf")
for line in open(sys.argv[1], encoding="utf-8"):
    dec, _, enc = line.rstrip("\n").partition("\t")
    if norm(dec) != target:
        continue
    dl = dec.casefold()
    score = 0
    for tag, pts in (("(world)", 40), ("(usa, europe)", 30),
                     ("(usa)", 20), ("(europe)", 20)):
        if tag in dl:
            score += pts
    m = re.search(r"\(rev (\d+)\)", dl)
    if m:
        score += int(m.group(1))
    if "(japan)" in dl and "(en)" not in dl:
        score -= 10
    if score > best_score:
        best, best_score = enc, score
if best:
    print(best)
PY
}

fetch_icons() { # $1 dossier du jeu, $2 nom du jeu, $3 base du fichier ROM, $4 catalogue (gbc, sg, gg, sms ou "")
  [ "$NO_ICONS" = 1 ] && return 0
  local dir="$1" name="$2" base="$3" png="$ICON_CACHE/${4:+$4-}$2.png" cand set url enc
  if [ ! -f "$png" ]; then
    for cand in "$SRC_DIR/icons/$2.png" "$SRC_DIR/icons/$3.png" "$SRC_DIR/$3.png"; do
      if [ -f "$cand" ]; then mkdir -p "$ICON_CACHE"; cp "$cand" "$png"; break; fi
    done
  fi
  if [ ! -f "$png" ]; then
    # Les jeux Game Boy Color ont leur propre catalogue libretro.
    local sets=("$4" "")
    [ "$4" = gbc ] && sets=("" gbc)
    [ "$4" = sg ] && sets=(sg)
    [ "$4" = gg ] && sets=(gg)
    [ "$4" = sms ] && sets=(sms)
    mkdir -p "$ICON_CACHE"
    for set in "${sets[@]}"; do
      enc=$(find_thumb "$2" "$set") && [ -n "$enc" ] && break
    done
    if [ -z "${enc:-}" ]; then
      # Catalogue inaccessible ou nom absent : essais directs en dernier
      # recours (nom nettoyé supposé identique au nom No-Intro).
      local catalogs=("Nintendo%20-%20Game%20Boy" "Nintendo%20-%20Game%20Boy%20Color")
      [ "$4" = sg ] && catalogs=("Sega%20-%20SG-1000")
      [ "$4" = gg ] && catalogs=("Sega%20-%20Game%20Gear")
      [ "$4" = sms ] && catalogs=("Sega%20-%20Master%20System%20-%20Mark%20III")
      for set in "${catalogs[@]}"; do
        url="$THUMB_BASE/$set/Named_Titles/$(url_quote "$2").png"
        if curl -fsSL --max-time 30 -o "$png" "$url" 2>/dev/null; then break; fi
        if wget -q -T 30 -O "$png" "$url" 2>/dev/null; then break; fi
        rm -f "$png"
      done
    else
      if [ "$set" = sg ]; then set="Sega%20-%20SG-1000"
      elif [ "$set" = gg ]; then set="Sega%20-%20Game%20Gear"
      elif [ "$set" = sms ]; then set="Sega%20-%20Master%20System%20-%20Mark%20III"
      elif [ "$set" = gbc ]; then set="Nintendo%20-%20Game%20Boy%20Color"
      else set="Nintendo%20-%20Game%20Boy"; fi
      url="$THUMB_BASE/$set/Named_Titles/$enc.png"
      curl -fsSL --max-time 30 -o "$png" "$url" 2>/dev/null || \
        wget -q -T 30 -O "$png" "$url" 2>/dev/null || rm -f "$png"
    fi
  fi
  if [ ! -f "$png" ]; then
    echo "[!] Icône introuvable pour « $2 » sur thumbnails.libretro.com"
    echo "    (nom No-Intro différent ? dépose $SRC_DIR/icons/$2.png)"
    return 1
  fi
  if ! make_bmp "$png" "$dir/TITLESCREEN.BMP" 80 64 || \
     ! make_bmp "$png" "$dir/ICON.BMP" 32 32; then
    echo "[!] Conversion d'icône impossible pour « $2 » (ImageMagick ou PIL requis)"
    rm -f "$dir/TITLESCREEN.BMP" "$dir/ICON.BMP"
    return 1
  fi
  return 0
}

# État d'un jeu : chaîne lisible pour le menu + décision de (re)construction.
# Entrées : $1 chemin du jeu sous la sortie (sans .zip), $2 sha1 de la ROM.
# Sorties globales : S_tag (affichage), S_todo (0 rien, 1 à faire, 2 à
# confirmer).  Le jeu vit dans <dir>.zip : état et marque de version se
# lisent dedans.
assess() {
  local dir="$1" sha="$2" system="${3:-gameboy}" expected_pack="$PACK"
  [ "$system" = sg1000 ] || [ "$system" = gg ] || [ "$system" = sms ] && expected_pack=0
  local expected_hc=0
  [ "$OVERCLOCK" = 1 ] && expected_hc=1
  local zip="$DEST_DIR/$dir.zip"
  local ver="" state_sha="" state_pack="" state_compiler="" state_system="" state_hc="" state tmpbin
  S_todo=1; S_tag="nouvelle"
  if [ -f "$zip" ]; then
    tmpbin=$(mktemp)
    zip_member "$zip" .bin > "$tmpbin" || true
    [ -s "$tmpbin" ] && ver=$(firmware_version "$tmpbin")
    rm -f "$tmpbin"
    state=$(zip_member "$zip" .gbrmeta.json || true)
    if [ -n "$state" ]; then
      if [ -z "$ver" ]; then
        ver=$(sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p' <<<"$state")
      fi
      state_sha=$(sed -n 's/.*"sha1":[[:space:]]*"\([^"]*\)".*/\1/p' <<<"$state")
      state_pack=$(sed -n 's/.*"pack":[[:space:]]*\([01]\).*/\1/p' <<<"$state")
      state_compiler=$(sed -n 's/.*"compiler":[[:space:]]*"\([^"]*\)".*/\1/p' <<<"$state")
      state_system=$(sed -n 's/.*"system":[[:space:]]*"\([^" ]*\)".*/\1/p' <<<"$state")
      state_hc=$(sed -n 's/.*"hc":[[:space:]]*\([01]\).*/\1/p' <<<"$state")
    fi
    if [ "${state_system:-gameboy}" != "$system" ]; then
      S_tag="système ${state_system:-gameboy} -> $system"; S_todo=1
    elif [ -n "$state_sha" ] && [ "$state_sha" != "$sha" ]; then
      S_tag="ROM modifiée"; S_todo=1
    elif [ "${state_pack:-0}" != "$expected_pack" ]; then
      S_tag="PACK=${state_pack:-0} -> PACK=$expected_pack"; S_todo=1
    elif [ "${state_compiler:-gcc}" != "$FIRMWARE_COMPILER" ]; then
      S_tag="compilateur ${state_compiler:-gcc} -> $FIRMWARE_COMPILER"; S_todo=1
    elif [ "${state_hc:-0}" != "$expected_hc" ]; then
      S_tag="horloge $([ "${state_hc:-0}" = 1 ] && echo 72 || echo 48) -> $([ "$expected_hc" = 1 ] && echo 72 || echo 48) MHz"; S_todo=1
    elif [ -z "$ver" ]; then
      S_tag="version inconnue"; S_todo=2
    elif [ "$ver" = "$CUR_VERSION" ]; then
      S_tag="à jour (v$CUR_VERSION)"; S_todo=0
    else
      S_tag="v$ver -> v$CUR_VERSION"; S_todo=2
    fi
    # Partie ajoutée au profil depuis ce firmware (play.sh record) : le
    # code qu'elle a trouvé n'est pas encore compilé.
    local st
    for st in "$PROFILES_DIR/$dir" "$PROFILES_DIR/$dir [${sha:0:8}]"; do
      [ "$(sed -n "s/^SHA1='\(.*\)'$/\1/p" "$st/rom.env" 2>/dev/null)" = "$sha" ] || continue
      if [ "$S_todo" = 0 ] && [ -n "$(find "$st" -maxdepth 1 -name 'coverage-*.cov' -newer "$zip" 2>/dev/null | head -n1)" ]; then
        S_tag="profil enrichi"; S_todo=1
      fi
    done
  fi
  return 0
}

in_array() { # $1 valeur, $2... tableau
  local v="$1"; shift
  local x
  for x in "$@"; do [ "$x" = "$v" ] && return 0; done
  return 1
}


# rom.plan / rom.rank / storage.json are shared by the consoles of a game: they
# are only reused for the console, simulator, profile tool and cache they were made with.
plan_stamp() { echo "$TARGET $(cat ./build/bin/m0romsim "$PROFILE_TOOL" targets/common/runtime/m0_cache.inc 2> /dev/null | sha1sum | cut -c1-16)"; }
plan_stale() { [ "$(cat "$1/rom.rank.stamp" 2> /dev/null)" != "$(plan_stamp)" ]; }

build_sg_game() { # SG-1000 and Game Gear own their generator, profiles and build system.
  local rom_path="$1" prefix="$2" game_name="$3" rom_sha="$4"
  local SRC_DIR="$(dirname "$rom_path")"
  local sys=sg1000 host=./build/bin/sg1000_host label=SG-1000 icons=sg
  case "${rom_path##*.}" in
    gg|GG|gG|Gg) sys=gg host=./build/bin/gg_host label="Game Gear" icons=gg ;;
    sms|SMS) sys=sms host=./build/bin/gg_host label="Master System" icons=sms ;;
  esac
  local out_dir="out/$sys/$TARGET/$prefix" game_dir="$DEST_DIR/$(sys_dir "$sys")/$game_name"
  local sd_name; sd_name=$(sd_name_of "$game_name" "$rom_sha")
  local store cov auto_frames
  local cov_args=() firmware
  profile_store "$sys" "$game_name" "$rom_sha" "$rom_path" || return 1
  mkdir -p "$out_dir" "$game_dir" || return 1
  if [ "$PLAY" = 1 ]; then
    local sg_player="${SG_PLAYER:-../gm0/build/bin/gm0}"
    [ -x "$sg_player" ] || { echo "[x] Lecteur gm0 absent : construire gm0 ou définir SG_PLAYER."; return 1; }
    "$sg_player" "$rom_path" || return 1
  fi
  auto_frames="${SG_AUTO_FRAMES:-$((AUTO_SECONDS * 60))}"
  [ "$sys" != sg1000 ] && auto_frames="${SG_AUTO_FRAMES:-$((AUTO_SECONDS * 540))}"
  local seeds="${SG_SEEDS:-8}" explorers="$EXPLORERS" explore_frames="${EXPLORE_FRAMES:-$((AUTO_SECONDS * 6000))}" seed pids=()
  [ "$explore_frames" -gt 0 ] || explorers=0
  [ "$explorers" -gt 0 ] || explore_frames=0
  # Revision 2 preserves even seeds and checkpoints discoveries. Rebuild old
  # automatic sessions once, and honour changes to their count or duration.
  # Manual recordings are not touched. Publish the stamp only after success.
  local profile_config="v2 $auto_frames $seeds $explorers $explore_frames"
  # Old Super Game 150 sessions used the Sega map and never left its menu.
  if [ "$sys" = sms ] && "$GBRECOMP" "$rom_path" --info | grep -q '^mapper=korean150$'; then
    profile_config="v3-korean150 $auto_frames $seeds $explorers $explore_frames"
  fi
  # GG/SMS : un profil sans « # frames » (ancien format) ne dit pas combien de fois chaque instruction
  # s'exécute : le générateur ne peut pas arbitrer code compilé et secteurs gardés en flash, on le refait ;
  # de même, toutes plateformes, un profil sans exploration (coverage-explore*.cov, voir plus bas).
  local stale=0
  [ "$(cat "$store/.auto-profile-config" 2>/dev/null)" != "$profile_config" ] && stale=1
  [ -f "$store/coverage-idle.cov" ] || stale=1
  for ((seed = 1; seed <= seeds; seed++)); do
    [ -f "$store/coverage-rand$seed.cov" ] || stale=1
  done
  for ((seed = 1; seed <= explorers; seed++)); do
    [ -f "$store/coverage-explore$seed.cov" ] || stale=1
  done
  [ "$sys" != sg1000 ] && [ -f "$store/coverage-auto.cov" ] && ! grep -q '^# frames ' "$store/coverage-auto.cov" && stale=1
  if [ "$AUTO" = 1 ] && { [ "$AUTO_REDO" = 1 ] || [ "$stale" = 1 ] || [ ! -f "$store/coverage-auto.cov" ]; }; then
    [ -x "$host" ] || { echo "[x] Construire $(basename "$host") avec CMake/Ninja."; return 1; }
    case "$auto_frames" in ''|*[!0-9]*|0) echo "[x] SG_AUTO_FRAMES doit être un entier positif."; return 1 ;; esac
    # Explorations guidées par la couverture (--explore) : le profil est celui de toutes les images jouées.
    local more=""
    [ "$explorers" -gt 0 ] && more=", $explorers explorations de $explore_frames images"
    echo "[i] Profil $label : $auto_frames images NTSC, bouton 1 pour démarrer et tirer, sans touche, $seeds parties aléatoires$more"
    "$host" "$rom_path" --frames "$auto_frames" --buttons 16 --start-frame 30 \
      --coverage "$store/coverage-auto.tmp" > /dev/null &
    pids+=($!)
    # no input at all: logos, title and demo, which the others skip
    "$host" "$rom_path" --frames "$auto_frames" \
      --coverage "$store/coverage-idle.tmp" > /dev/null &
    pids+=($!)
    for ((seed = 1; seed <= seeds; seed++)); do
      "$host" "$rom_path" --frames "$auto_frames" --start-frame 30 --random "$seed" \
        --coverage "$store/coverage-rand$seed.tmp" > /dev/null &
      pids+=($!)
    done
    for ((seed = 1; seed <= explorers; seed++)); do
      "$host" "$rom_path" --explore "$explore_frames" --random "$seed" \
        --coverage "$store/coverage-explore$seed.tmp" > /dev/null 2>&1 &
      pids+=($!)
    done
    for seed in "${pids[@]}"; do wait "$seed" || return 1; done
    for ((seed = 1; seed <= explorers; seed++)); do
      mv "$store/coverage-explore$seed.tmp" "$store/coverage-explore$seed.cov" || return 1
    done
    mv "$store/coverage-auto.tmp" "$store/coverage-auto.cov" || return 1
    mv "$store/coverage-idle.tmp" "$store/coverage-idle.cov" || return 1
    for ((seed = 1; seed <= seeds; seed++)); do
      mv "$store/coverage-rand$seed.tmp" "$store/coverage-rand$seed.cov" || return 1
    done
    printf '%s\n' "$profile_config" > "$store/.auto-profile-config.tmp" || return 1
    mv "$store/.auto-profile-config.tmp" "$store/.auto-profile-config" || return 1
  fi
  # Parties jouées (sessions gm0, coverage-<id>.input) : rejouées quand leur
  # couverture manque, quand le profil automatique est refait, ou REPLAY=1.
  local inp id
  for inp in "$store"/coverage-*.input; do
    [ -f "$inp" ] || continue
    id=$(basename "$inp" .input); id=${id#coverage-}
    if [ -f "$store/coverage-$id.cov" ] && [ "$REPLAY" != 1 ] &&
       ! { [ "$AUTO" = 1 ] && { [ "$AUTO_REDO" = 1 ] || [ "$stale" = 1 ]; }; }; then continue; fi
    [ -x "$host" ] || { echo "[x] Construire $(basename "$host") avec CMake/Ninja."; return 1; }
    echo "[i] Partie $id rejouée"
    "$host" "$rom_path" --frames "$(awk 'END { print $1 + 60 }' "$inp")" --input "$inp" \
      --coverage "$store/coverage-$id.tmp" > /dev/null && mv "$store/coverage-$id.tmp" "$store/coverage-$id.cov"
    rm -f "$store/coverage-$id.tmp"
  done
  [ -x "$host" ] && profile_set "$store" HOST "$(cd "$(dirname "$host")" && pwd)/$(basename "$host")" # sessions gm0 rejouées
  # Game Gear/SMS plus grands que la flash : ce qui reste en flash vient du
  # cache carte du firmware simulé sur les traces des parties (m0romsim,
  # tools/profile/m0profile.py storage), refait quand une partie change.
  local rom_plan=() plan_lock=()
  if [ "$sys" != sg1000 ] && [ "${STORAGE:-1}" = 1 ] && [ "$(stat -L -c %s "$rom_path")" -gt 229376 ] &&
     { [ "$TARGET" = meta ] || [ "$TARGET" = pokitto ]; } &&
     [ -f "$PROFILE_TOOL" ] && [ -x ./build/bin/m0romsim ]; then
    if [ ! -f "$store/rom.rank" ] || plan_stale "$store" || [ -n "$(find "$store" -maxdepth 1 \( -name 'coverage-*.cov' -o -name "calibration-$TARGET.json" \) -newer "$store/rom.rank" | head -n1)" ]; then
      echo "[i] Plan de stockage de la ROM : cache carte simulé sur les parties enregistrées"
      command -v flock > /dev/null && plan_lock=(flock "$DEST_DIR/.gbrecomp-auto.lock")
      # le projet généré du build précédent rejoue les parties avec son propre
      # code compilé : le trafic ROM de ce firmware (gm0 : identique)
      local plan_project=()
      [ -f "$out_dir/host/CMakeLists.txt" ] && plan_project=(--project "$out_dir")
      "${plan_lock[@]}" python3 "$PROFILE_TOOL" storage "$store" --quick --target "$TARGET" "${plan_project[@]}" &&
        plan_stamp > "$store/rom.rank.stamp" ||
        echo "[!] Plan de stockage impossible : secteurs choisis par fréquence de lecture"
    fi
    [ -f "$store/rom.rank" ] && rom_plan=(--rom-plan "$store/rom.rank")
  fi
  for cov in "$store"/*.cov; do
    [ -f "$cov" ] || continue
    case "$(head -n1 "$cov")" in
      "# m0boy.$sys.coverage.v1"|"# m0boy.$sys.coverage.v2") cov_args+=(--coverage "$cov") ;;
    esac
  done
  # Firmware filling the flash: a first build measures the generated code
  # per instruction (sg_gen.o), the second generates with that measure;
  # when it overflows, a few bytes more per instruction until it fits.
  local sg_build="$out_dir/build" gen_log build_log bpi="" fill="" tries=0 rc text compiled sg_extra="" esp_stack=""
  [ "$FIRMWARE_COMPILER" = clang ] && sg_build="$out_dir/build-clang"
  local size_bin
  if [ "$TARGET" = espboy ]; then size_bin="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-xtensa/bin/xtensa-lx106-elf-size"
  elif [ "$TARGET" = aka ]; then size_bin="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-size"
  else size_bin=$(command -v arm-none-eabi-size || echo "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-size"); fi
  sg_build_once() { # 0 ok, 1 flash full, 2 error
    gen_log=$("$GBRECOMP" "$rom_path" --system "$sys" --target "$TARGET" --output-prefix "$prefix" --sd-name "$sd_name" \
      --meta-compiler "$FIRMWARE_COMPILER" "${cov_args[@]}" "${rom_plan[@]}" ${bpi:+--meta-bytes-per-instruction "$bpi"} ${fill:+--meta-fill-bytes "$fill"} \
      ${SG_SYNC_CYCLES:+--meta-sync-cycles "$SG_SYNC_CYCLES"} -o "$out_dir") || return 2
    if [ "$TARGET" = pokitto ]; then
      local hc_flags=""
      hc_flags="-DEXTRA_DEFS=-DMETA_OVERCLOCK=$OVERCLOCK"
      cmake -G Ninja -S "$out_dir" -B "$sg_build" -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake \
        -DSG_COMPILER="$FIRMWARE_COMPILER" ${hc_flags} > /dev/null || return 2
      build_log=$(ninja -C "$sg_build" -j"$NPROC" 2>&1) && return 0
    elif [ "$TARGET" = espboy ]; then
      espboy_pio_run "$out_dir" "${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}${RELEASE_FLAG:+ $RELEASE_FLAG}" && return 0
    elif [ "$TARGET" = pc ]; then
      build_log=$(cmake -B "$out_dir/build" -S "$out_dir" >/dev/null && cmake --build "$out_dir/build" -j"$NPROC" 2>&1) && return 0
    else
      build_log=$(PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}${RELEASE_FLAG:+ $RELEASE_FLAG}" pio run -d "$out_dir" 2>&1) && return 0
    fi
    echo "$build_log" | grep -q -E "overflowed|will not fit|greater than maximum allowed|exceeds 1 MiB" && return 1
    echo "$build_log" | tail -20
    return 2
  }
  sg_code_bpi() { # measured flash per compiled instruction, rounded up
    local obj
    if [ "$TARGET" = pokitto ]; then obj=$(ls "$sg_build"/CMakeFiles/firmware.dir/src/sg_gen.c.o* 2>/dev/null | head -n1)
    else obj=$(ls "$out_dir/.pio/build/$PIO_ENV/src/sg_gen.o" "$out_dir/.pio/build/$PIO_ENV/src/sg_gen.c.o" 2>/dev/null | head -n1); fi
    compiled=$(echo "$gen_log" | sed -n 's/.*instructions: *\([0-9]*\) compiled.*/\1/p')
    [ -f "$obj" ] && [ -n "$compiled" ] && [ "$compiled" -gt 0 ] || return 1
    text=$("$size_bin" "$obj" 2>/dev/null | tail -n1 | awk '{print $1}')
    [ -n "$text" ] || return 1
    echo $(( (text + compiled - 1) / compiled + 1 ))
  }
  sg_build_once; rc=$?
  [ $rc -eq 2 ] && return 1
  if bpi=$(sg_code_bpi); then
    while :; do
      sg_build_once; rc=$?
      [ $rc -eq 0 ] && break
      [ $rc -eq 2 ] && return 1
      tries=$((tries + 1))
      # Au moins +3 ; sinon la part du dépassement retirée du code compilé (le code précis,
      # Super Game 150, coûte bien plus que la mesure du premier build : +3 par essai ne suffisait pas).
      local over cnt nb=$((bpi + 3))
      over=$(echo "$build_log" | sed -n 's/.*overflowed by \([0-9]*\) bytes.*/\1/p' | head -n1)
      cnt=$(echo "$gen_log" | sed -n 's/.*instructions: *\([0-9]*\) compiled.*/\1/p')
      if [ -n "$over" ] && [ -n "$cnt" ] && [ $((cnt * bpi)) -gt "$over" ]; then
        local grown=$(( cnt * bpi * bpi / (cnt * bpi - over) + 1 ))
        [ "$grown" -gt "$nb" ] && nb=$grown
      fi
      bpi=$nb
      [ $tries -le 6 ] || { echo "[x] Le firmware ne tient pas en flash."; return 1; }
    done
  elif [ $rc -ne 0 ]; then
    echo "[x] Le firmware ne tient pas en flash."; return 1
  fi
  # Flash saturée : le build mesuré laisse quelques Kio (bpi arrondi au-dessus,
  # secteurs de pool entiers) ; ils vont à des secteurs de ROM de plus, sans
  # toucher au code compilé.  Un dépassement retire un secteur et recommence.
  if [ "$TARGET" = meta ] && { [ "$sys" = gg ] || [ "$sys" = sms ]; } && echo "$gen_log" | grep -q 'ROM flash pool'; then
    local f_used f_max f_try f_ok="" f_log f_build
    f_used=$(echo "$build_log" | sed -n 's/^Flash:.*(used \([0-9]*\) bytes from \([0-9]*\) bytes).*/\1/p')
    f_max=$(echo "$build_log" | sed -n 's/^Flash:.*(used \([0-9]*\) bytes from \([0-9]*\) bytes).*/\2/p')
    if [ -n "$f_used" ] && [ -n "$f_max" ] && [ $((f_max - f_used)) -ge 516 ]; then
      f_try=$((f_max - f_used))
      while [ $f_try -ge 516 ] && [ $tries -le 12 ]; do
        fill=$f_try; sg_build_once; rc=$?
        if [ $rc -eq 0 ]; then f_ok=1; break; fi
        [ $rc -eq 2 ] && return 1
        tries=$((tries + 1)); f_try=$((f_try - 1024))
      done
      if [ -z "$f_ok" ]; then fill=""; sg_build_once || return 1; fi
    fi
  fi
  # Le cache carte grossit tant que le rapport RAM du build laisse la pile
  # large.  Le plus profond mesuré au filigrane (SG_STACK_PAINT, démo et menu
  # exercés, USB comprise donc majorée) est de 1640 octets, +128 non peints
  # sous main ; chaque secteur coûte 528 octets de .bss.  En release sans USB
  # (420 octets rendus), la garde passe à 2400 : quatre secteurs, 16 blocs de
  # 128, ~620 octets de marge sur le pire mesuré.  L'anneau audio, lui, ne
  # descend pas : le producteur écrit une image émulée entière (plancher 512
  # échantillons, meta_audio.cpp).
  # Sauf avec le cache dynamique (GG_CACHE_DYN, par défaut sur META) : le
  # firmware remplit lui-même le cache au démarrage jusqu'à cette garde.
  if [ "$TARGET" = meta ] && [ "${CACHE_DYN:-1}" = 0 ] && { [ "$sys" = gg ] || [ "$sys" = sms ]; } \
     && grep -q "GG_CACHE_BLOCK" "$out_dir/src/sg_config.h"; then
    local sectors=3 used
    used=$(echo "$build_log" | sed -n 's/.*(\(used [0-9]*\) bytes from 32768.*/\1/p'); used=${used#used }
    # Pile trop courte avec 3 secteurs (Master System : machine de 25 Kio) :
    # 2 secteurs, le minimum du cache, plutôt que du code natif refusé.
    if [ -n "$used" ] && [ $((32768 - used)) -lt 1800 ]; then
      sectors=2
      build_log=$(PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}${RELEASE_FLAG:+ $RELEASE_FLAG} -DGG_STREAM_SECTORS=2" \
        pio run -d "$out_dir" 2>&1) || return 1
      echo "[i] Cache carte : 2 secteurs (pile courte)"; sg_extra="-DGG_STREAM_SECTORS=2"
    fi
    while [ $sectors -ge 3 ]; do
      used=$(echo "$build_log" | sed -n 's/.*(\(used [0-9]*\) bytes from 32768.*/\1/p'); used=${used#used }
      local guard=2000; [ "$RELEASE" = 1 ] && guard=2400
      { [ -n "$used" ] && [ $((32768 - used - 528)) -ge $guard ]; } || break
      sectors=$((sectors + 1)); [ $sectors -le 8 ] || break
      build_log=$(PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}${RELEASE_FLAG:+ $RELEASE_FLAG} -DGG_STREAM_SECTORS=$sectors" \
        pio run -d "$out_dir" 2>&1) || break
      echo "[i] Cache carte : $sectors secteurs"; sg_extra="-DGG_STREAM_SECTORS=$sectors"
    done
  fi
  echo "$gen_log"
  echo "$build_log" | grep -E "^(RAM|Flash):" || true
  [ -n "$bpi" ] && echo "[i] Code généré : $bpi octets de flash par instruction"
  local stream_name
  stream_name=$(sed -n 's/^#define GG_STREAM_NAME "\(.*\)"$/\1/p' "$out_dir/src/sg_config.h")
  if [ "$TARGET" = pokitto ]; then
    firmware="$sg_build/firmware.bin"
    cp "$firmware" "$game_dir/$game_name.bin" || return 1
  elif [ "$TARGET" = espboy ]; then
    firmware="$out_dir/.pio/build/$PIO_ENV/firmware.bin"
    cp "$firmware" "$game_dir/firmware.bin" || return 1
    cp "${firmware%.bin}.sym" "$game_dir/firmware.sym" || return 1 # symboles pour gm0
    cp "$out_dir/README.md" "$game_dir/README.md" || return 1
    if [ -f "$out_dir/rom.bin" ]; then # ROM above 256 KiB: pages 1 and up, read from the flash
      cp "$out_dir/rom.bin" "$game_dir/rom.bin" || return 1
      printf 'esptool.py --chip esp8266 --port PORT write_flash 0x00000 firmware.bin 0x100000 rom.bin\n' > "$game_dir/FLASH.txt"
    else
      printf 'esptool.py --chip esp8266 --port PORT write_flash 0x00000 firmware.bin\n' > "$game_dir/FLASH.txt"
    fi
  elif [ "$TARGET" = aka ]; then
    firmware="$out_dir/.pio/build/$PIO_ENV/firmware.bin"
    aka_package "$out_dir" "$game_dir" "$game_name" || return 1
    fetch_icons "$game_dir" "$game_name" "$(basename "$rom_path")" "$icons" || true
    [ ! -f "$game_dir/TITLESCREEN.BMP" ] || make_bmp "$game_dir/TITLESCREEN.BMP" "$game_dir/screen.bmp" 160 120 || true
  else
    firmware="$out_dir/.pio/build/$PIO_ENV/firmware.bin"
    cp "$firmware" "$game_dir/$sd_name.BIN" || return 1
    fetch_icons "$game_dir" "$game_name" "$(basename "$rom_path")" "$icons" || echo "[i] (jeu conservé sans icône)"
  fi
  local save_name
  save_name=$(sed -n 's/^#define GG_SAVE_NAME "\(.*\)"$/\1/p' "$out_dir/src/sg_config.h")
  if [ -n "$save_name" ]; then # cartridge RAM: created once, never overwritten
    [ -f "$game_dir/$save_name" ] || head -c 32768 /dev/zero | tr '\0' '\377' > "$game_dir/$save_name"
    echo "[i] Sauvegarde : $game_name/$save_name (déjà sur la carte : ne plus y toucher)"
  fi
  local state_name state_sectors
  state_name=$(sed -n 's/^#define GG_STATE_NAME "\(.*\)"$/\1/p' "$out_dir/src/sg_config.h")
  if [ -n "$state_name" ]; then # save states: created once, never overwritten
    state_sectors=$(sed -n 's/^#define GG_STATE_SLOT_SECTORS \(.*\)$/\1/p' "$out_dir/src/sg_config.h")
    [ -f "$game_dir/$state_name" ] || head -c $((10 * state_sectors * 512)) /dev/zero | tr '\0' '\377' > "$game_dir/$state_name"
    echo "[i] Etats : $game_name/$state_name (déjà sur la carte : ne plus y toucher)"
  fi
  if [ -n "$stream_name" ]; then
    if [ $(( $(stat -L -c %s "$rom_path") % 16384 )) -eq 512 ]; then # copier header (same rule as m0boy/sg1000.h and targets/common/host/m0_host_util.h): the card holds the ROM itself
      tail -c +513 "$rom_path" > "$game_dir/$stream_name" || return 1
    else cp "$rom_path" "$game_dir/$stream_name" || return 1; fi
    echo "[i] Pages de ROM lues depuis la carte SD : $game_name/$stream_name (dans le dossier du jeu)"
  fi
  printf '{\n  "rom": "%s",\n  "sha1": "%s",\n  "version": "%s",\n  "prefix": "%s",\n  "system": "%s",\n  "pack": 0,\n  "hc": %s,\n  "compiler": "%s"\n}\n' \
    "$(json_escape "$(basename "$rom_path")")" "$rom_sha" "$CUR_VERSION" "$prefix" "$sys" "$([ "$OVERCLOCK" = 1 ] && echo 1 || echo 0)" "$FIRMWARE_COMPILER" > "$game_dir/.gbrmeta.json"
  if [ "$SOURCE" = 1 ]; then
    if [ "$TARGET" = pokitto ]; then
      export_source "$out_dir" "$game_dir" "" "cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DSG_COMPILER=$FIRMWARE_COMPILER -DEXTRA_DEFS=-DMETA_OVERCLOCK=$OVERCLOCK && ninja -C build" || return 1
    else
      export_source "$out_dir" "$game_dir" "${PLATFORMIO_BUILD_FLAGS:-} ${OVERCLOCK_FLAG:-} ${RELEASE_FLAG:-} $sg_extra" "pio run -d ." || return 1
    fi
  fi
  zip_game "$game_dir" "$game_dir.zip" || { echo "[x] Zip impossible : le jeu reste en dossier."; return 1; }
  rm -rf "$game_dir"
  echo "[i] $([ "$SOURCE" = 1 ] && echo "Source" || echo "Firmware") $label $TARGET v$CUR_VERSION : $game_dir.zip"
  local elf="$out_dir/.pio/build/$PIO_ENV/firmware.elf"
  [ "$TARGET" = pokitto ] && elf="$sg_build/firmware.elf"
  # what this firmware compiled: the plan's traces leave out those fetches
  [ -f "$out_dir/compiled.txt" ] && cp "$out_dir/compiled.txt" "$store/compiled-$TARGET.txt"
  profile_build "$store" "$game_dir.zip" "$elf" "$(echo "$gen_log"; echo "$build_log" | grep -E "^(RAM|Flash|flash):")" \
    ${bpi:+"bpi=$bpi"} ${sg_extra:+"flags=$sg_extra"}
}

# PICO-8 : p8c compile le Lua de la cartouche en C (pas de partie à jouer :
# tout est compilé) ; META, Pokitto et ESPboy.  Fichier d'états <NOM>.ST8 sur
# la carte (META, Pokitto), dans la flash sur l'ESPboy.
build_p8_game() { # $1 cartouche (.p8, .p8.png), $2 préfixe, $3 dossier du jeu, $4 sha1
  local rom_path="$1" prefix="$2" game_name="$3" rom_sha="$4"
  local out_dir="out/pico8/$TARGET/$prefix" game_dir="$DEST_DIR/pico8/$game_name"
  local sd_name gen_log build_log esp_stack="" flags firmware
  sd_name=$(sd_name_of "$game_name" "$rom_sha")
  mkdir -p "$out_dir" "$game_dir" || return 1
  gen_log=$("$GBRECOMP" "$rom_path" --system pico8 --target "$TARGET" --output-prefix "$prefix" --sd-name "$sd_name" \
    --meta-compiler "$FIRMWARE_COMPILER" -o "$out_dir" 2>&1) || { echo "$gen_log"; return 1; }
  flags="${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}${RELEASE_FLAG:+ $RELEASE_FLAG}"
  if [ "$TARGET" = espboy ]; then
    espboy_pio_run "$out_dir" "$flags" || { echo "$build_log" | tail -20; return 1; }
  elif [ "$TARGET" = pokitto ]; then
    build_log=$(cmake -G Ninja -S "$out_dir" -B "$out_dir/build" -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake \
      -DSG_COMPILER="$FIRMWARE_COMPILER" -DEXTRA_DEFS=-DMETA_OVERCLOCK=$OVERCLOCK 2>&1) \
      && build_log=$(ninja -C "$out_dir/build" -j"$NPROC" 2>&1) \
      || { echo "$build_log" | tail -20; return 1; }
  else
    build_log=$(FIRMWARE_COMPILER="$FIRMWARE_COMPILER" PLATFORMIO_BUILD_FLAGS="$flags" pio run -d "$out_dir" 2>&1) \
      || { echo "$build_log" | tail -20; return 1; }
  fi
  echo "$gen_log"
  echo "$build_log" | grep -E "^(RAM|Flash):" || true
  firmware="$out_dir/.pio/build/$PIO_ENV/firmware.bin"
  [ "$TARGET" = pokitto ] && firmware="$out_dir/build/firmware.bin"
  if [ "$TARGET" = espboy ]; then
    cp "$firmware" "$game_dir/firmware.bin" || return 1
    [ -f "${firmware%.bin}.sym" ] && cp "${firmware%.bin}.sym" "$game_dir/firmware.sym" # symboles pour gm0
    cp "$out_dir/README.md" "$game_dir/README.md" || return 1
    printf 'esptool.py --chip esp8266 --port PORT write_flash 0x00000 firmware.bin\n' > "$game_dir/FLASH.txt"
  else
    cp "$firmware" "$game_dir/$sd_name.BIN" || return 1
  fi
  local state_name state_sectors
  state_name=$(sed -n 's/^#define P8_STATE_NAME "\(.*\)"$/\1/p' "$out_dir/src/p8_config.h")
  if [ -n "$state_name" ]; then # save states: created once, never overwritten
    state_sectors=$(sed -n 's/^#define P8_STATE_SLOT_SECTORS \([0-9]*\)$/\1/p' "$out_dir/src/p8_config.h")
    [ -f "$game_dir/$state_name" ] || head -c $((10 * state_sectors * 512)) /dev/zero | tr '\0' '\377' > "$game_dir/$state_name"
    echo "[i] Etats : $game_name/$state_name (déjà sur la carte : ne plus y toucher)"
  fi
  printf '{\n  "rom": "%s",\n  "sha1": "%s",\n  "version": "%s",\n  "prefix": "%s",\n  "system": "pico8",\n  "pack": 0,\n  "hc": %s,\n  "compiler": "%s"\n}\n' \
    "$(json_escape "$(basename "$rom_path")")" "$rom_sha" "$CUR_VERSION" "$prefix" "$([ "$OVERCLOCK" = 1 ] && echo 1 || echo 0)" "$FIRMWARE_COMPILER" > "$game_dir/.gbrmeta.json"
  if [ "$SOURCE" = 1 ]; then
    if [ "$TARGET" = pokitto ]; then
      export_source "$out_dir" "$game_dir" "" "cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake -DSG_COMPILER=$FIRMWARE_COMPILER -DEXTRA_DEFS=-DMETA_OVERCLOCK=$OVERCLOCK && ninja -C build" || return 1
    else
      export_source "$out_dir" "$game_dir" "$flags" "$([ "$TARGET" = espboy ] || echo "FIRMWARE_COMPILER=$FIRMWARE_COMPILER ")pio run -d ." || return 1
    fi
  fi
  zip_game "$game_dir" "$game_dir.zip" || { echo "[x] Zip impossible : le jeu reste en dossier."; return 1; }
  rm -rf "$game_dir"
  echo "[i] $([ "$SOURCE" = 1 ] && echo "Source" || echo "Firmware") PICO-8 $TARGET v$CUR_VERSION : $game_dir.zip"
}

build_game() { # $1 rom, $2 préfixe, $3 dossier du jeu (dernier composant), $4 sha1
  case "$1" in *.p8|*.P8|*.p8.png|*.P8.PNG|*.p8.PNG) build_p8_game "$@"; return $? ;; esac
  case "${1##*.}" in sg|SG|sG|Sg|gg|GG|gG|Gg|sms|SMS) build_sg_game "$@"; return $? ;; esac
  local rom_path="$1" prefix="$2" game_name="$3" rom_sha="$4"
  local SRC_DIR="$(dirname "$rom_path")"
  local game_dir="$DEST_DIR/gb/$game_name"
  local sd_name; sd_name=$(sd_name_of "$game_name" "$rom_sha")
  local out_dir="$OUT_ROOT/$prefix"
  local flags_file="$DEST_DIR/gb/$game_name.flags" extra_flags=""

  # a0) Parties enregistrées pour cette ROM, dans son dossier de profil
  #     (profile_store, source de vérité, commun aux consoles cibles).
  local f store gbc=""
  case "${rom_path##*.}" in gbc|GBC) gbc=gbc ;; esac
  profile_store gameboy "$game_name" "$rom_sha" "$rom_path" || return 1
  mkdir -p "$out_dir" "$game_dir"
  save_sessions() { # out/<jeu> -> dossier des parties (sans écraser)
    for f in "$out_dir"/coverage-*.cov "$out_dir"/coverage-*.input "$out_dir"/romdata-*.prof; do
      [ -f "$f" ] && [ ! -f "$store/$(basename "$f")" ] && cp "$f" "$store/"
    done
    [ -f "$out_dir/auto.input" ] && [ ! -f "$store/coverage-auto.input" ] && cp "$out_dir/auto.input" "$store/coverage-auto.input"
    return 0
  }
  save_sessions # parties laissées dans out/ par une version précédente du script
  for f in "$store"/coverage-*.cov "$store"/romdata-*.prof; do
    [ -f "$f" ] && cp "$f" "$out_dir/"
  done
  local n_sessions
  n_sessions=$(ls "$store"/coverage-*.cov 2>/dev/null | wc -l)
  [ "$n_sessions" -gt 0 ] && echo "[i] $n_sessions partie(s) déjà enregistrée(s) réutilisée(s) : $store"

  # a) Projet interpréteur seul et outils PC
  local pack_args=()
  [ "$PACK" = "1" ] && pack_args=(--meta-pack)
  "$GBRECOMP" "$rom_path" --target "$TARGET" --meta-compiler "$FIRMWARE_COMPILER" --output-prefix "$prefix" --sd-name "$sd_name" "${pack_args[@]}" -o "$out_dir" || return 1
  cmake -G Ninja -S "$out_dir/host" -B "$out_dir/host/build" > /dev/null || return 1
  ninja -C "$out_dir/host/build" || return 1
  profile_set "$store" HOST_PLAY "$(cd "$out_dir/host/build" && pwd)/${prefix}_play" # play.sh record
  profile_set "$store" HOST "$(cd "$out_dir/host/build" && pwd)/${prefix}_profile" # sessions gm0 rejouées

  # a2) Parties à la main enregistrées (fenêtre du harnais, sessions gm0) :
  #     rejouées quand leur couverture manque, toutes avec REPLAY=1 (utile
  #     après une mise à jour du runtime ou pour ajouter le profil de
  #     lectures SD).
  local inp id last
  for inp in "$store"/coverage-*.input; do
      [ -f "$inp" ] || continue
      id=$(basename "$inp" .input); id=${id#coverage-}
      case "$id" in auto*|ai*) continue ;; esac
      [ "$REPLAY" = "1" ] || [ ! -f "$store/coverage-$id.cov" ] || continue
      last=$(awk 'END { print $1 + 120 }' "$inp")
      echo "[i] Partie $id rejouée ($last frames)"
      "$out_dir/host/build/${prefix}_profile" --interp --frames "$last" --input "$inp" \
        --coverage "$store/coverage-$id.cov" --rom-profile-out "$store/romdata-$id.prof" > /dev/null || true
      cp "$store/coverage-$id.cov" "$out_dir/"
      [ -f "$store/romdata-$id.prof" ] && cp "$store/romdata-$id.prof" "$out_dir/"
  done

  # b) Partie interactive : enregistre une couverture de plus
  if [ "$PLAY" = "1" ]; then
    local stamp
    stamp=$(date +%Y%m%d-%H%M%S)
    echo "[!] Partie d'enregistrement. Flèches | X=A | Z=B | Entrée=Start | Retour=Select | Échap=Quitter"
    "$out_dir/host/build/${prefix}_play" --interp \
      --coverage "$out_dir/coverage-$stamp.cov" \
      --rom-profile-out "$out_dir/romdata-$stamp.prof" \
      --record-input "$out_dir/coverage-$stamp.input" || true
    save_sessions
  fi

  # b2) Parties automatiques, sans fenêtre ni son, en parallèle : Start de
  #     temps en temps, directions/A/B au hasard, une graine par partie.
  #     Déjà enregistrées pour cette ROM : réutilisées (AUTO_REDO=1 pour
  #     les refaire).  Les graines manquantes sont ajoutées : les ROMs
  #     passées par une version qui ne jouait qu'une partie (coverage-auto,
  #     gardée) reçoivent les huit (une seule partie compilait du code moins
  #     général : Faceball 2000 -10 % par frame sur une partie inédite).
  local seed n_auto=0
  for seed in $(seq 1 "$AUTO_SEEDS"); do
    if [ -f "$store/coverage-auto$seed.cov" ]; then
      n_auto=$((n_auto + 1))
    fi
  done
  if [ "$AUTO" = "1" ] && [ "$AUTO_REDO" != "1" ] && [ "$n_auto" -ge "$AUTO_SEEDS" ]; then
    echo "[i] Parties automatiques déjà enregistrées : réutilisées"
  elif [ "$AUTO" = "1" ]; then
    if [ "$AUTO_REDO" = "1" ]; then
      rm -f "$store"/coverage-auto* "$store"/romdata-auto* "$out_dir"/coverage-auto* "$out_dir"/romdata-auto* "$out_dir/auto.input"
    elif [ "$n_auto" -gt 0 ] || [ -f "$store/coverage-auto.cov" ]; then
      echo "[i] Parties automatiques : $n_auto sur $AUTO_SEEDS enregistrées, ajout des autres"
    fi
    local frames=$((AUTO_SECONDS * 40000)) lock=()
    # Une ROM à la fois sur tous les cœurs (flock, quand il existe).
    command -v flock > /dev/null && lock=(flock "$DEST_DIR/.gbrecomp-auto.lock")
    for seed in $(seq 1 "$AUTO_SEEDS"); do
      [ -f "$store/coverage-auto$seed.cov" ] && continue
      awk -v frames="$frames" -v seed=$((41 + seed)) 'BEGIN { srand(seed); split("right left up down a b", n, " ");
        for (f = 30; f < frames; f += 20) {
          if (f % 600 < 20) { print f, "start"; continue }
          b = ""; for (i = 1; i <= 6; i++) if (rand() < (i == 1 ? 0.55 : 0.25)) b = b (b == "" ? "" : "+") n[i];
          print f, (b == "" ? "none" : b) } }' > "$out_dir/coverage-auto$seed.input"
    done
    # Pleine vitesse, arrêt au bout de AUTO_SECONDS ; l'entrée enregistrée
    # est coupée à la dernière frame jouée, pour rejouer la même partie.
    "${lock[@]}" bash -c '
      prof=$1 dir=$2 secs=$3 frames=$4; shift 4
      guard=(); command -v timeout > /dev/null && guard=(timeout $((secs + 30)))
      for seed in "$@"; do
        [ -f "$dir/coverage-auto$seed.input" ] && [ ! -f "$dir/coverage-auto$seed.cov" ] || continue
        ( n=$("${guard[@]}" "$prof" --interp --frames "$frames" --max-seconds "$secs" \
              --input "$dir/coverage-auto$seed.input" --coverage "$dir/coverage-auto$seed.cov" \
              --rom-profile-out "$dir/romdata-auto$seed.prof" 2> /dev/null | sed -n "s/^frames \([0-9]*\).*/\1/p")
          [ -n "$n" ] && awk -v n="$n" "\$1 <= n" "$dir/coverage-auto$seed.input" > "$dir/coverage-auto$seed.tmp" &&
            mv "$dir/coverage-auto$seed.tmp" "$dir/coverage-auto$seed.input" ) &
      done
      wait' _ "$out_dir/host/build/${prefix}_profile" "$out_dir" "$AUTO_SECONDS" "$frames" $(seq 1 "$AUTO_SEEDS")
    save_sessions
  fi

  # b2a) DEMO=1 : une partie sans aucune touche (titre, démos dans
  #     plusieurs niveaux), DEMO_FRAMES images (défaut 108000 : 30 mn de
  #     jeu), AUTO_SECONDS secondes de temps réel au plus.  Pas de fichier
  #     d'entrées : couverture et profil de lectures enregistrés directement.
  [ "$AUTO_REDO" = "1" ] && rm -f "$store"/coverage-demo.cov "$store"/romdata-demo.prof "$out_dir"/coverage-demo.cov "$out_dir"/romdata-demo.prof
  if [ "$AUTO" = "1" ] && [ "$DEMO" = "1" ] && [ "$DEMO_FRAMES" -gt 0 ] &&
     [ ! -f "$store/coverage-demo.cov" ]; then
    echo "[i] Démo : ${DEMO_FRAMES} images sans touche (AUTO_SECONDS s de temps réel au plus)"
    local guard=()
    command -v timeout > /dev/null && guard=(timeout $((AUTO_SECONDS + 30)))
    "${guard[@]}" "$out_dir/host/build/${prefix}_profile" --interp --frames "$DEMO_FRAMES" --max-seconds "$AUTO_SECONDS" \
      --coverage "$out_dir/coverage-demo.tmp" --rom-profile-out "$out_dir/romdata-demo.prof" > /dev/null 2>&1 &&
      mv "$out_dir/coverage-demo.tmp" "$out_dir/coverage-demo.cov"
    rm -f "$out_dir/coverage-demo.tmp"
    save_sessions
  fi

  # b2bis) Explorations guidées par la couverture (--explore, EXPLORERS) :
  #     reprennent depuis des états sauvés là où du code ou des données de
  #     ROM nouveaux sont apparus.  Pas de fichier d'entrées : la couverture
  #     et le profil de lectures sont enregistrés directement.  Images
  #     comptées (même partie d'une machine à l'autre) ; AUTO_SECONDS x 15
  #     secondes au plus, garde-fou que les jeux lents n'atteignent pas.
  local explore_frames="${EXPLORE_FRAMES:-$((AUTO_SECONDS * 20000))}" todo=()
  if [ "$AUTO" = "1" ] && [ "$EXPLORERS" -gt 0 ] && [ "$explore_frames" -gt 0 ]; then
    [ "$AUTO_REDO" = "1" ] && rm -f "$store"/coverage-explore* "$store"/romdata-explore* "$out_dir"/coverage-explore* "$out_dir"/romdata-explore*
    for seed in $(seq 1 "$EXPLORERS"); do
      [ -f "$store/coverage-explore$seed.cov" ] || todo+=("$seed")
    done
  fi
  if [ ${#todo[@]} -gt 0 ]; then
    echo "[i] Explorations : ${#todo[@]} de $explore_frames images"
    local lock=()
    command -v flock > /dev/null && lock=(flock "$DEST_DIR/.gbrecomp-auto.lock")
    "${lock[@]}" bash -c '
      prof=$1 dir=$2 secs=$3 frames=$4; shift 4
      for seed in "$@"; do
        "$prof" --interp --explore "$frames" --random "$seed" --max-seconds $((secs * 15)) \
          --coverage "$dir/coverage-explore$seed.tmp" --rom-profile-out "$dir/romdata-explore$seed.prof" \
          > /dev/null 2>&1 && mv "$dir/coverage-explore$seed.tmp" "$dir/coverage-explore$seed.cov" &
      done
      wait' _ "$out_dir/host/build/${prefix}_profile" "$out_dir" "$AUTO_SECONDS" "$explore_frames" "${todo[@]}"
    rm -f "$out_dir"/coverage-explore*.tmp
    save_sessions
  fi

  # b2ter) AI=1 : parties jouées par un modèle de décision (Ollama), une à la
  #     fois pour toutes les ROMs (le modèle répond à une requête à la fois).
  if [ "$AI" = "1" ] && ! python3 "$(dirname "$0")/tools/ai-play/ai_play.py" --check --model "$AI_MODEL" --url "$AI_URL"; then
    echo "[!] AI=1 : le modèle $AI_MODEL ne répond pas sur $AI_URL (ollama serve, ollama pull $AI_MODEL) ; parties IA sautées"
  elif [ "$AI" = "1" ]; then
    local g ai_lock=()
    command -v flock > /dev/null && ai_lock=(flock "$DEST_DIR/.gbrecomp-ai.lock")
    [ "$AI_REDO" = "1" ] && rm -f "$store"/coverage-ai* "$store"/romdata-ai* "$out_dir"/coverage-ai* "$out_dir"/romdata-ai*
    for g in $(seq 1 "$AI_GAMES"); do
      [ -f "$store/coverage-ai$g.cov" ] && continue
      echo "[i] Partie IA $g ($AI_MODEL, ${AI_SECONDS} s)"
      "${ai_lock[@]}" python3 "$(dirname "$0")/tools/ai-play/ai_play.py" \
        --harness "$out_dir/host/build/${prefix}_profile" --out "$out_dir/coverage-ai$g.input" \
        --seconds "$AI_SECONDS" --seed "$g" --model "$AI_MODEL" --url "$AI_URL" -- \
        --coverage "$out_dir/coverage-ai$g.cov" --rom-profile-out "$out_dir/romdata-ai$g.prof" > /dev/null || true
    done
    save_sessions
  fi

  # b3) Jeux lus depuis la carte SD ou compressés (PACK=1) : il faut savoir
  #     quelles données de la ROM sont lues pour choisir ce qui reste brut en
  #     flash.  Parties réutilisées sans ce profil (enregistrées avec un
  #     firmware ordinaire) : rejouées une fois pour le mesurer.
  if compgen -G "$out_dir/host/*_romfull.c" > /dev/null && ! compgen -G "$out_dir/romdata-*.prof" > /dev/null; then
    local inp id last guard=()
    command -v timeout > /dev/null && guard=(timeout $((AUTO_SECONDS + 30)))
    for inp in "$store"/coverage-*.input; do
      [ -f "$inp" ] || continue
      id=$(basename "$inp" .input); id=${id#coverage-}
      [ -f "$out_dir/romdata-$id.prof" ] && continue
      last=$(awk 'END { print $1 + 120 }' "$inp")
      "${guard[@]}" "$out_dir/host/build/${prefix}_profile" --interp --frames "$last" \
        --max-seconds "$AUTO_SECONDS" --input "$inp" \
        --rom-profile-out "$out_dir/romdata-$id.prof" > /dev/null 2>&1 &
    done
    wait
    compgen -G "$out_dir/romdata-*.prof" > /dev/null &&
      echo "[i] Profil des lectures de ROM mesuré en rejouant les parties enregistrées"
    save_sessions
  fi

  # c) Régénération avec toutes les couvertures enregistrées (+ analyse statique)
  local cov_args=()
  local cov
  for cov in "$out_dir"/coverage-*.cov; do
    [ -f "$cov" ] && cov_args+=(--coverage "$cov")
  done
  # Lectures de données dans la ROM (jeux lus depuis la carte SD) : choisit
  # ce qui reste en flash.
  local prof
  for prof in "$out_dir"/romdata-*.prof; do
    [ -f "$prof" ] && cov_args+=(--rom-profile "$prof")
  done
  # Plan de stockage simulé, comme pour le Game Gear : le cache carte du
  # firmware (m0_cache, partagé) rejoué sur les traces des parties
  # enregistrées ; le générateur en tire le poids de chaque bloc de 4 Kio.
  if [ "${STORAGE:-1}" = 1 ] && [ "$(stat -L -c %s "$rom_path")" -gt 229376 ] &&
     { [ "$TARGET" = meta ] || [ "$TARGET" = pokitto ]; } &&
     [ -f "$PROFILE_TOOL" ] && [ -x ./build/bin/m0romsim ] && ls "$store"/coverage-*.input > /dev/null 2>&1; then
    if [ ! -f "$store/rom.rank" ] || plan_stale "$store" ||
       [ -n "$(find "$store" -maxdepth 1 \( -name 'coverage-*.cov' -o -name "calibration-$TARGET.json" \) -newer "$store/rom.rank" | head -n1)" ]; then
      echo "[i] Plan de stockage de la ROM : cache carte simulé sur les parties enregistrées"
      python3 "$PROFILE_TOOL" storage "$store" --quick --target "$TARGET" &&
        plan_stamp > "$store/rom.rank.stamp" ||
        echo "[!] Plan de stockage impossible : blocs choisis par nombre de lectures"
    fi
    [ -f "$store/rom.rank" ] && cov_args+=(--rom-plan "$store/rom.rank")
  fi
  local static_args=()
  [ "$STATIC" = "1" ] && static_args=(--meta-static)
  if [ ${#cov_args[@]} -eq 0 ] && [ ${#static_args[@]} -eq 0 ]; then
    echo "[!] Aucune couverture : firmware 100 % interpréteur (plus lent)."
  fi

  # d) Firmware, en remplissant la flash au maximum.  Le code est trié du
  #    plus exécuté au moins exécuté ; --meta-bytes-per-instruction règle
  #    combien en garder.  Recherche dichotomique de la plus petite valeur
  #    (= le plus de code) qui tient encore dans les 240 Kio de la META
  #    (228 Kio sur la Pokitto).
  build_with() { # $1 = octets par instruction ; 0 ok, 1 flash pleine, 2 erreur
    gen_log=$("$GBRECOMP" "$rom_path" --target "$TARGET" --meta-compiler "$FIRMWARE_COMPILER" --output-prefix "$prefix" --sd-name "$sd_name" \
      "${cov_args[@]}" "${static_args[@]}" "${pack_args[@]}" --meta-bytes-per-instruction "$1" -o "$out_dir") || return 2
    # build_flags du jeu (<dossier_sortie>/<Jeu>/build_flags, une option par
    # ligne, # = commentaire ; ex. -D META_TBLEND=0 pour un jeu à la pile
    # trop juste) : platformio.ini est régénéré à chaque passe, on ajoute à
    # la fin (build_flags y est la dernière clé) ; côté pokitto, make reçoit
    # EXTRA_CFLAGS.
    if [ -s "$flags_file" ]; then
      if [ "$TARGET" = pokitto ]; then
        extra_flags=$(grep -v '^#' "$flags_file" | sed 's/[[:space:]]*$//' | grep -v '^$' | tr '\n' ' ')
      else
        python3 - "$out_dir/platformio.ini" "$flags_file" <<'PY' || return 2
import sys
ini, fl = sys.argv[1], sys.argv[2]
flags = [l.strip() for l in open(fl) if l.strip() and not l.startswith("#")]
text = open(ini).read()
if "build_flags" not in text:
    raise SystemExit("build_flags absent de platformio.ini")
with open(ini, "a") as f:
    if text and not text.endswith("\n"): f.write("\n")
    for flag in flags: f.write("    " + flag + "\n")
PY
      fi
    fi
    if [ "$TARGET" = pokitto ]; then
      extra_flags="${extra_flags:+$extra_flags }-DMETA_OVERCLOCK=$OVERCLOCK"
      build_log=$(make -C "$out_dir" -j"$NPROC" COMPILER="$FIRMWARE_COMPILER" EXTRA_CFLAGS="${extra_flags:-}" 2>&1) && return 0
    elif [ "$TARGET" = espboy ]; then
      espboy_pio_run "$out_dir" "${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}" && return 0
    elif [ "$TARGET" = pc ]; then
      build_log=$(cmake -B "$out_dir/build" -S "$out_dir" >/dev/null && cmake --build "$out_dir/build" -j"$NPROC" 2>&1) && return 0
    else
      build_log=$(FIRMWARE_COMPILER="$FIRMWARE_COMPILER" PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:+$PLATFORMIO_BUILD_FLAGS }${OVERCLOCK_FLAG:-}" pio run -d "$out_dir" 2>&1) && return 0
    fi
    echo "$build_log" | grep -q -E "overflowed|will not fit|greater than maximum allowed|exceeds 640 KiB" && return 1
    echo "$build_log" | tail -20
    return 2
  }
  local lo=8 hi=64 mid best=0 rc build_log gen_log need_rebuild=1 esp_stack=""
  if [ ${#static_args[@]} -eq 0 ] && [ ${#cov_args[@]} -eq 0 ]; then lo=64; fi
  # Rebuild de version (même ROM, même PACK) : le remplissage trouvé à la
  # passe précédente reste le bon ordre de grandeur — on le vérifie, on
  # tente un cran de remplissage de plus, et on ne fait la recherche
  # dichotomique complète que si la place a vraiment changé (~7 builds pio
  # économisés par jeu, c'est ça qui rendait les montées de version longues).
  local last_bpi
  last_bpi=$(sed -n 's/.*"bpi":[[:space:]]*\([0-9]*\).*/\1/p' "$game_dir/.gbrmeta.json" 2>/dev/null)
  case "$last_bpi" in ''|*[!0-9]*) last_bpi="" ;; esac
  if [ -n "$last_bpi" ] && [ "$last_bpi" -ge 8 ] && [ "$last_bpi" -le 64 ]; then
    if build_with "$last_bpi"; then
      best=$last_bpi
      local try=$(( last_bpi - 4 ))
      # essai infructueux ou pas : le firmware disque est déjà celui de best
      if [ "$try" -ge 8 ] && build_with "$try"; then best=$try; fi
      need_rebuild=0
    else
      lo=$(( last_bpi + 1 ))
      echo "[i] Remplissage précédent (bpi=$last_bpi) ne tient plus : recherche complète"
    fi
  fi
  while [ "$best" -eq 0 ] && [ "$lo" -le "$hi" ]; do
    mid=$(( (lo + hi) / 2 ))
    build_with "$mid"; rc=$?
    if [ $rc -eq 0 ]; then best=$mid; hi=$(( mid - 1 ))
    elif [ $rc -eq 1 ]; then lo=$(( mid + 1 ))
    else return 1
    fi
  done
  [ "$best" -gt 0 ] || { echo "[x] Même sans code compilé, le firmware ne tient pas en flash."; return 1; }
  # Le dernier essai n'est pas forcément le meilleur : reconstruire celui-ci
  # (inutile par le chemin rapide : le firmware disque est déjà celui de best).
  if [ "$need_rebuild" = 1 ]; then build_with "$best" || return 1; fi
  echo "$gen_log" | grep -E "compiled:|interpreter only:|wait loops:|ROM streaming:|ROM packed:|--meta-pack:"
  if [ "$TARGET" = pokitto ]; then
    echo "$build_log" | grep -E "^flash:"
    local firmware_build=build
    [ "$FIRMWARE_COMPILER" = clang ] && firmware_build=build-clang
    cp "$out_dir/$firmware_build/firmware.bin" "$game_dir/$game_name.bin"
  else
    echo "$build_log" | grep -E "RAM:|Flash:"
    # Le .bin porte le nom court des fichiers du jeu : le loader prend le
    # premier .bin du dossier, le firmware retrouve son dossier par lui.
    if [ "$TARGET" = espboy ]; then
      cp "$out_dir/.pio/build/$PIO_ENV/firmware.bin" "$game_dir/firmware.bin" || return 1
      cp "$out_dir/.pio/build/$PIO_ENV/firmware.sym" "$game_dir/firmware.sym" || return 1 # symboles pour gm0
      cp "$out_dir/README.md" "$game_dir/README.md" || return 1
      if [ -f "$out_dir/rom.bin" ]; then
        cp "$out_dir/rom.bin" "$game_dir/rom.bin" || return 1
        printf 'esptool.py --chip esp8266 --port PORT write_flash 0x00000 firmware.bin 0x100000 rom.bin\n' > "$game_dir/FLASH.txt"
      else
        printf 'esptool.py --chip esp8266 --port PORT write_flash 0x00000 firmware.bin\n' > "$game_dir/FLASH.txt"
      fi
    elif [ "$TARGET" = aka ]; then
      aka_package "$out_dir" "$game_dir" "$game_name" || return 1
    else
      cp "$out_dir/.pio/build/$PIO_ENV/firmware.bin" "$game_dir/$sd_name.BIN"
    fi
    # Icônes du loader (PNG libretro -> BMP 24 bits), sans bloquer le jeu.
    [ "$TARGET" = espboy ] || fetch_icons "$game_dir" "$game_name" "$(basename "$rom_path")" "$gbc" \
      || echo "[i] (jeu conservé sans icône)"
    if [ "$TARGET" = aka ] && [ -f "$game_dir/TITLESCREEN.BMP" ]; then
      make_bmp "$game_dir/TITLESCREEN.BMP" "$game_dir/screen.bmp" 160 120 || true
    fi
  fi
  # Sauvegarde (cartouche à pile) ou grande RAM de cartouche : un fichier
  # .SAV de la taille de cette RAM, à mettre sur la carte à côté du .bin.
  # Jamais écrasé : il contient la progression.
  local save_name eram battery
  save_name=$(sed -n 's/^#define GBL_SAVE_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  eram=$(sed -n 's/^#define GBL_ERAM_SIZE \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  battery=$(grep -c '^#define GBL_BATTERY 1' "$out_dir/src/gbl_config.h")
  if [ "$TARGET" != espboy ] && [ -n "$save_name" ] && { [ "$battery" = "1" ] || [ "${eram:-0}" -gt 2048 ]; }; then
    if [ ! -f "$game_dir/$save_name" ]; then
      head -c "$eram" /dev/zero | tr '\0' '\377' > "$game_dir/$save_name"
    fi
    echo "[i] Sauvegarde : $game_name/$save_name (déjà sur la carte : ne plus y toucher)"
  fi
  # États sauvegardés : un fichier de GBL_STATE_SLOTS emplacements, créé une
  # fois (jamais écrasé : il contient les états) ; sans lui, le menu de la
  # console n'offre pas Sauver/Charger l'état.
  local state_name state_slots state_sectors
  state_name=$(sed -n 's/^#define GBL_STATE_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  state_slots=$(sed -n 's/^#define GBL_STATE_SLOTS \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  state_sectors=$(sed -n 's/^#define GBL_STATE_SLOT_SECTORS \([0-9]*\)$/\1/p' "$out_dir/src/gbl_config.h")
  if [ -n "$state_name" ] && [ -n "$state_slots" ] && [ -n "$state_sectors" ]; then
    local state_bytes=$((state_slots * state_sectors * 512))
    if [ ! -f "$game_dir/$state_name" ] || [ "$(stat -c %s "$game_dir/$state_name")" -lt "$state_bytes" ]; then
      head -c "$state_bytes" /dev/zero > "$game_dir/$state_name"
    fi
    echo "[i] États : $game_name/$state_name ($state_slots emplacements, $((state_bytes / 1024)) Kio)"
  fi
  # ROM trop grosse pour la flash : le firmware la lit sur la carte SD.
  local stream_name
  stream_name=$(sed -n 's/^#define GBL_STREAM_NAME "\(.*\)"$/\1/p' "$out_dir/src/gbl_config.h")
  if [ -n "$stream_name" ]; then
    cp "$rom_path" "$game_dir/$stream_name"
    echo "[i] ROM lue depuis la carte SD : $game_name/$stream_name (dans le dossier du jeu)"
  fi
  # État pour les lancements suivants : version, ROM exacte, fichiers.
  {
    printf '{\n  "rom": "%s",\n  "sha1": "%s",\n  "version": "%s",\n  "prefix": "%s",\n  "pack": %s,\n  "hc": %s,\n  "compiler": "%s",\n  "bpi": %s,\n  "date": "%s"\n}\n' \
      "$(json_escape "$(basename "$rom_path")")" "$rom_sha" "$CUR_VERSION" "$prefix" "$PACK" \
      "$([ "$OVERCLOCK" = 1 ] && echo 1 || echo 0)" "$FIRMWARE_COMPILER" "$best" "$(date +%Y-%m-%d)"
  } > "$game_dir/.gbrmeta.json"
  local fw_ver
  if [ "$TARGET" = pokitto ]; then fw_ver=$(firmware_version "$game_dir/$game_name.bin")
  elif [ "$TARGET" = espboy ] || [ "$TARGET" = aka ]; then fw_ver=$(firmware_version "$game_dir/firmware.bin")
  else fw_ver=$(firmware_version "$game_dir/$sd_name.BIN"); fi
  [ -n "$fw_ver" ] || fw_ver="$CUR_VERSION (pas de marqueur : gbrecomp trop ancien)"
  echo "[i] Firmware produit par gbrecomp v$fw_ver"
  if [ "$SOURCE" = 1 ]; then
    if [ "$TARGET" = pokitto ]; then
      export_source "$out_dir" "$game_dir" "" "make COMPILER=$FIRMWARE_COMPILER EXTRA_CFLAGS='$(printf '%s\n' $extra_flags | awk '!seen[$0]++' | tr '\n' ' ' | sed 's/ $//')'" || return 1
    else
      export_source "$out_dir" "$game_dir" "${PLATFORMIO_BUILD_FLAGS:-} ${OVERCLOCK_FLAG:-}" "FIRMWARE_COMPILER=$FIRMWARE_COMPILER pio run -d ." || return 1
    fi
  fi
  zip_game "$game_dir" "$game_dir.zip" || { echo "[x] Zip impossible : le jeu reste en dossier."; return 1; }
  rm -rf "$game_dir"
  echo "[i] Jeu : $game_dir.zip"
  local elf="$out_dir/.pio/build/$PIO_ENV/firmware.elf"
  [ "$TARGET" = pokitto ] && elf="$out_dir/$firmware_build/firmware.elf"
  profile_build "$store" "$game_dir.zip" "$elf" \
    "$(echo "$gen_log" | grep -E "compiled:|interpreter only:|wait loops:|ROM streaming:|ROM packed:|--meta-pack:"
       echo "$build_log" | grep -E "^(RAM|Flash|flash):")" "bpi=$best" "pack=$PACK"
}


shopt -s nullglob nocaseglob
if [ -n "${SRC_SYSTEM:-}" ]; then
  case "$SRC_SYSTEM" in
    gb)  roms=("$SRC_DIR"/*.gb "$SRC_DIR"/*.gbc "$SRC_DIR"/gb/*.gb "$SRC_DIR"/gb/*.gbc) ;;
    sg)  roms=("$SRC_DIR"/*.sg "$SRC_DIR"/sg/*.sg) ;;
    gg)  roms=("$SRC_DIR"/*.gg "$SRC_DIR"/gg/*.gg) ;;
    sms) roms=("$SRC_DIR"/*.sms "$SRC_DIR"/sms/*.sms) ;;
    pico8) roms=("$SRC_DIR"/*.p8 "$SRC_DIR"/*.p8.png "$SRC_DIR"/pico8/*.p8 "$SRC_DIR"/pico8/*.p8.png "$SRC_DIR"/pico8/*/*.p8 "$SRC_DIR"/pico8/*/*.p8.png) ;;
    *)   echo "SRC_SYSTEM='$SRC_SYSTEM' inconnu (gb, sg, gg, sms ou pico8)."; exit 1 ;;
  esac
else
  roms=("$SRC_DIR"/*.gb "$SRC_DIR"/*.gbc "$SRC_DIR"/*.sg "$SRC_DIR"/*.gg "$SRC_DIR"/*.sms
        "$SRC_DIR"/gb/*.gb "$SRC_DIR"/gb/*.gbc "$SRC_DIR"/sg/*.sg "$SRC_DIR"/gg/*.gg "$SRC_DIR"/sms/*.sms)
  # PICO-8 (META, Pokitto, ESPboy) : une cartouche par fichier, ou par dossier sous pico8/
  roms+=("$SRC_DIR"/*.p8 "$SRC_DIR"/*.p8.png "$SRC_DIR"/pico8/*.p8 "$SRC_DIR"/pico8/*.p8.png
         "$SRC_DIR"/pico8/*/*.p8 "$SRC_DIR"/pico8/*/*.p8.png)
fi
shopt -u nullglob nocaseglob

if [ ${#roms[@]} -eq 0 ]; then
  echo "Aucun fichier .gb, .gbc, .sg, .gg, .sms ou .p8 trouvé dans '$SRC_DIR'."
  exit 0
fi

# ---- Inventaire : nom propre, préfixe C, sha1, état -----------------------

declare -a R_path R_base R_clean R_prefix R_sha R_status R_ver R_system
declare -a used_prefixes=() used_dirs=() used_shas=()
n=0
n_dup=0
for rom_path in "${roms[@]}"; do
  system=gameboy
  case "${rom_path##*.}" in sg|SG|sG|Sg) system=sg1000 ;; gg|GG|gG|Gg) system=gg ;; sms|SMS) system=sms ;; esac
  case "$rom_path" in *.p8|*.P8|*.p8.png|*.P8.PNG|*.p8.PNG) system=pico8 ;; esac
  sha=$(sha1_of "$rom_path")
  dup=0
  for ((i = 0; i < n; i++)); do
    [ "${R_sha[i]}" = "$sha" ] && [ "${R_system[i]}" = "$system" ] && { dup=1; break; }
  done
  if [ "$dup" = 1 ]; then
    echo "[-] Doublon (même sha1 que ${R_clean[i]}) : $(basename "$rom_path")"
    n_dup=$((n_dup + 1))
    continue
  fi

  long=$(sd_safe "$(clean_name "$(basename "$rom_path")")")
  [ -n "$long" ] || long=$(sd_safe "$(basename "$rom_path")")
  if [ "$system" = pico8 ]; then # celeste.p8.png -> celeste ; dans son dossier : le nom du dossier
    long=$(sd_safe "$(clean_name "$(basename "${rom_path%.[pP][nN][gG]}")")")
    parent=$(basename "$(dirname "$rom_path")")
    case "$parent" in pico8|PICO8|.) ;; *) [ "$(dirname "$rom_path")" != "$SRC_DIR" ] && long=$(sd_safe "$parent") ;; esac
  fi
  clean=$(loader_name "$long")
  sysdir=$(sys_dir "$system")
  dir="$clean"
  for ((k = 2; ; k++)); do
    busy=0
    for d in ${used_dirs[@]+"${used_dirs[@]}"}; do [ "$d" = "$sysdir/$dir" ] && busy=1; done
    [ "$busy" = 0 ] && break
    dir="$(loader_name "$clean" $((LOADER_NAME_MAX - 1 - ${#k}))) $k"
  done

  # Identifiant C : caractères spéciaux -> '_', jamais de chiffre en tête.
  # Tiré du nom complet, pas du nom raccourci pour le loader : il donne les
  # noms 8.3 des fichiers .GB/.SAV/.STA du jeu, qui ne doivent pas changer.
  prefix=$(printf '%s' "$long" | tr -c 'a-zA-Z0-9_' '_' | sed -E 's/_+$//')
  [ -n "$prefix" ] || prefix="game"
  case "$prefix" in [0-9]*) prefix="g$prefix" ;; esac
  while in_array "$sysdir:$prefix" ${used_prefixes[@]+"${used_prefixes[@]}"}; do prefix="${prefix}_2"; done

  assess "$sysdir/$dir" "$sha" "$system"
  R_system[n]="$system"
  R_path[n]="$rom_path"; R_base[n]="$(basename "$rom_path")"
  R_clean[n]="$dir"; R_prefix[n]="$prefix"; R_sha[n]="$sha"
  R_status[n]="$S_todo"; R_ver[n]="${S_tag}"
  used_prefixes+=("$sysdir:$prefix"); used_dirs+=("$sysdir/$dir"); used_shas+=("$sha")
  n=$((n + 1))
done

if [ "$n" -eq 0 ]; then
  echo "Rien à faire ($n_dup doublon(s) ignoré(s))."
  exit 0
fi

# ---- Choix des jeux --------------------------------------------------------

interactive=0
[ -t 0 ] && [ "$ALL" != 1 ] && interactive=1

declare -a SEL=() KEPT=()
todo_count=0
for ((i = 0; i < n; i++)); do
  [ "${R_status[i]}" != 0 ] && todo_count=$((todo_count + 1))
done

if [ "$interactive" = 1 ]; then
  echo; echo "ROMs de '$SRC_DIR' (gbrecomp v$CUR_VERSION) :"
  for ((i = 0; i < n; i++)); do
    extra=""
    [ "${R_clean[i]}" != "${R_base[i]%.*}" ] && extra="  <== ${R_base[i]}"
    printf " %2d. %-28s [%s]%s\n" $((i + 1)) "${R_clean[i]}" "${R_ver[i]}" "$extra"
  done
  [ "$n_dup" -gt 0 ] && echo "($n_dup doublon(s) ignoré(s))"
  echo
  if [ -n "${CHOICE:-}" ]; then
    [ "$CHOICE" = - ] && CHOICE=""
    ans="$CHOICE"
  else
    read -r -p "Jeux à traiter [Entrée = ce qui a à faire ($todo_count), a = tout, ex: 1,3-5, q = rien] : " ans
  fi
  case "$ans" in
    q|Q) exit 0 ;;
    "" ) for ((i = 0; i < n; i++)); do [ "${R_status[i]}" != 0 ] && SEL+=("$i"); done ;;
    a|A) for ((i = 0; i < n; i++)); do SEL+=("$i"); done ;;
    *  ) IFS=',' read -ra parts <<< "$ans"
         for part in "${parts[@]}"; do
           part=$(printf '%s' "$part" | tr -d '[:space:]')
           if [[ "$part" == *-* ]]; then
             from=${part%-*}; to=${part#*-}
             for ((j = from; j <= to; j++)); do
               [ "$j" -ge 1 ] && [ "$j" -le "$n" ] && SEL+=("$((j - 1))")
             done
           elif [[ "$part" =~ ^[0-9]+$ ]] && [ "$part" -ge 1 ] && [ "$part" -le "$n" ]; then
             SEL+=("$((part - 1))")
           fi
         done ;;
  esac

  # Proposer la mise à jour des firmwares faits par une autre version.
  upd=0
  for i in ${SEL[@]+"${SEL[@]}"}; do [ "${R_status[i]}" = 2 ] && upd=$((upd + 1)); done
  if [ "$upd" -gt 0 ]; then
    echo; echo "Firmware(s) fait(s) par une autre version (ou de version inconnue) :"
    for i in "${SEL[@]}"; do
      [ "${R_status[i]}" = 2 ] && echo "  - ${R_clean[i]}  [${R_ver[i]}]"
    done
    if [ -n "${CHOICE_UPD:-}" ]; then o="$CHOICE_UPD"
    else read -r -p "Reconstruire avec gbrecomp v$CUR_VERSION ? [O/n] " o; fi
    case "$o" in n|N*)
      rest=()
      for i in ${SEL[@]+"${SEL[@]}"}; do
        if [ "${R_status[i]}" = 2 ]; then KEPT+=("$i"); else rest+=("$i"); fi
      done
      SEL=(${rest[@]+"${rest[@]}"})
      ;;
    esac
  fi
  if [ "${CHOICE_CAPTURE:-0}" = 1 ]; then
    printf 'CHOICE=%s\nUPDANS=%s\n' "${ans:--}" "${o:-o}" > "${CHOICE_CAPTURE_OUT:?}"
    exit 0
  fi
else
  # Non interactif : ce qui a à faire, sauf mise à jour de version
  # (UPDATE=1 pour l'autoriser) et déjà-à-jour (FORCE=1 pour refaire).
  for ((i = 0; i < n; i++)); do
    case "${R_status[i]}" in
      1) SEL+=("$i") ;;
      2) if [ "$UPDATE" = 1 ]; then SEL+=("$i"); else KEPT+=("$i"); fi ;;
      0) SEL+=("$i") ;; # déjà à jour : icônes complétées sans recompiler (FORCE=1 refait)
    esac
  done
fi

# ---- Traitement ------------------------------------------------------------

[ "$PLAY" = "1" ] && JOBS=1
STATUS_DIR=$(mktemp -d)
trap 'rm -rf "$STATUS_DIR"' EXIT
running=0
echo
echo "${#SEL[@]} jeu(x) à traiter, ${JOBS} en parallèle, ${AUTO_SEEDS} parties automatiques par ROM (${NPROC} cœurs)"
if [ ${#KEPT[@]} -gt 0 ]; then
  echo "Firmware(s) conservé(s) sans reconstruction :"
  for i in "${KEPT[@]}"; do echo "  - ${R_clean[i]}  [${R_ver[i]}]"; done
  [ "$interactive" = 0 ] && echo "  (UPDATE=1 pour les reconstruire avec v$CUR_VERSION)"
fi

run_one() { # $1 rom, $2 préfixe, $3 dossier du jeu, $4 sha1, $5 index
  local i="$5"
  if build_game "$1" "$2" "$3" "$4"; then
    echo "$3" > "$STATUS_DIR/ok.$i"
  else
    echo "$3" > "$STATUS_DIR/fail.$i"
    return 1
  fi
}

for index in ${SEL[@]+"${SEL[@]}"}; do
  i="$index"
  if [ "${R_status[i]}" = 0 ] && [ "$FORCE" != 1 ]; then
    # Déjà à jour : on ne recompile pas ; icônes manquantes complétées
    # dans le zip du jeu.
    gbc=""
    case "${R_system[i]}" in
      gameboy) case "${R_base[i]##*.}" in gbc|GBC) gbc=gbc ;; esac ;;
      sg1000) gbc=sg ;;
      gg) gbc=gg ;;
      sms) gbc=sms ;;
    esac
    sysdir=$(sys_dir "${R_system[i]}")
    if [ "${R_system[i]}" = pico8 ]; then # pas d'icônes libretro
      echo "[=] ${R_clean[i]} : déjà à jour (v$CUR_VERSION), ignoré"
      echo "${R_clean[i]}" > "$STATUS_DIR/skip.$i"
      continue
    fi
    skip_zip="$DEST_DIR/$sysdir/${R_clean[i]}.zip"
    skip_tmp=$(mktemp -d)
    unzip_game "$skip_zip" "$skip_tmp"
    if (SRC_DIR="$(dirname "${R_path[i]}")"; fetch_icons "$skip_tmp/${R_clean[i]}" "${R_clean[i]}" "${R_base[i]}" "$gbc") &&
       zip_game "$skip_tmp/${R_clean[i]}" "$skip_zip"; then
      echo "[=] ${R_clean[i]} : déjà à jour (v$CUR_VERSION), ignoré"
    else
      echo "[=] ${R_clean[i]} : déjà à jour (v$CUR_VERSION), ignoré (icône absente)"
    fi
    rm -rf "$skip_tmp"
    echo "${R_clean[i]}" > "$STATUS_DIR/skip.$i"
    continue
  fi
  rom_path="${R_path[i]}"; prefix="${R_prefix[i]}"; dir="${R_clean[i]}"

  if [ "$JOBS" -le 1 ]; then
    echo "=========================================="
    echo "[+] $dir  (${R_base[i]})"
    echo "=========================================="
    if run_one "$rom_path" "$prefix" "$dir" "${R_sha[i]}" "$i"; then
      echo "[+] Terminé : $DEST_DIR/$(sys_dir "${R_system[i]}")/$dir.zip"
    else
      echo "[x] Échec pour ${R_base[i]} (ROM refusée ou firmware trop gros : voir le message ci-dessus)"
    fi
    echo ""
  else
    log_dir="$OUT_ROOT/$prefix"
    [ "${R_system[i]}" = sg1000 ] && log_dir="out/sg1000/$TARGET/$prefix"
    [ "${R_system[i]}" = gg ] && log_dir="out/gg/$TARGET/$prefix"
    [ "${R_system[i]}" = sms ] && log_dir="out/sms/$TARGET/$prefix"
    [ "${R_system[i]}" = pico8 ] && log_dir="out/pico8/$TARGET/$prefix"
    mkdir -p "$log_dir"
    (
      if run_one "$rom_path" "$prefix" "$dir" "${R_sha[i]}" "$i" > "$log_dir/compile.log" 2>&1; then
        echo "[+] $dir"
      else
        echo "[x] $dir (voir $log_dir/compile.log)"
      fi
    ) &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
      wait -n
      running=$((running - 1))
    fi
  fi
done
wait

n_ok=0 n_fail=0 n_skip=0
for f in "$STATUS_DIR"/ok.*;  do [ -f "$f" ] && n_ok=$((n_ok + 1)); done
for f in "$STATUS_DIR"/fail.*; do [ -f "$f" ] && n_fail=$((n_fail + 1)); done
for f in "$STATUS_DIR"/skip.*; do [ -f "$f" ] && n_skip=$((n_skip + 1)); done
echo "------------------------------------------"
echo "Réussis : $n_ok   Échecs : $n_fail   Déjà à jour : $n_skip   Conservés : ${#KEPT[@]}"
for f in "$STATUS_DIR"/fail.*; do [ -f "$f" ] && echo "  échec : $(cat "$f")"; done
if [ "$TARGET" = espboy ]; then
  echo "Jeux dans : $DEST_DIR/<gb|sg|gg|sms>/<Jeu>.zip (firmware.bin, + rom.bin au-delà de 256 Kio ; offsets dans FLASH.txt)"
else
  echo "Jeux dans : $DEST_DIR/<gb|sg|gg|sms>/<Jeu>.zip  (le zip contient le répertoire du jeu, à déposer sur la carte SD)"
fi
[ "$n_fail" -eq 0 ]
