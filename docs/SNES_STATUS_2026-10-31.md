# SNES : état au 2026-10-31

Bilan de la série de sessions « SNES complet », avec les preuves réellement
collectées (aucune compatibilité prétendue au-delà), et le plan de la
prochaine session. Le détail du cœur reste dans `docs/SNES.fr.md`.

## Ce qui est en place (avec preuve)

Quatre commits, tous sur `main` :

1. `438d9f0` — Dr. Mario démarre : unité mult/div $4202-$4217, amorce IPL
   conforme au protocole réel, vecteurs BRK/COP/NMI/IRQ selon le mode,
   JMP indirects dans le banc programme, sortie de région sur changement de
   contexte, BCD du code généré aligné sur l'interpréteur (les deux
   largeurs), `tools/snes_sweep.py`.
2. `6331cc4` — le DMA et le HDMA paient leur temps de bus (`dma_stolen`,
   payé aux frontières de tranches, total par trame exact), le H-IRQ tire
   à son dot en dessous de 256.
3. `453e277` — les fenêtres ($2123-$212F), la math de couleur
   ($2130-$2132, modèle ccmm--sd du wiki superfamicom) et la mosaïque
   ($2106) : chaque ligne compose un écran principal et un sous-écran.
4. `4ce1c62` — l'écho du DSP (tampon A-RAM, FIR, feedback, ECEN), le bruit
   (LFSR $4000), l'interpolation gaussienne, le PMON ; bits mute/reset de
   $6C corrigés (0x40/0x80, étaient 0x10/0x20).

Preuves :

- Contrat `--meta-precise` natif == interp (run_hash, image, audio,
  **cycles**) : Dr. Mario 900 images scriptées `9daadc39/f2a41fc5/e6f1d555`
  (`logs/dm_play.txt`, titre → options → partie), Bucket `fcd7e14c`,
  CPUADC `8c3b5971`, CPUMOV `0bb94aeb`, CPUMSC `147af44c` à 120 images,
  balayage `tools/snes_sweep.py` 1/1 sur la collection locale.
- ctest 86/86 vert à chaque étape ; `window_math_test` (masques, math,
  mosaïque), `timing_test` (l'addition CPU + volé = trame), `apu_test`
  étendu (l'écho écrit son tampon et s'arrête avec ECEN, le bruit remplit
  le ring des deux signes), `muldiv_test`.
- Captures de gameplay dans `logs/` (`dm10`/`dm11`/`dm12_900.ppm`).

## Les limites actuelles (dites telles quelles dans docs/SNES.fr.md)

- **Échantillonnage audio ≈ 1000 Hz** : un échantillon stéréo tous les
  1024 cycles SPC ; le WAV du harnais étiquette 32000 Hz. Le rythme du
  ring n'est pas celui d'une SNS — c'est le principal écart sonore
  structurel restant.
- **Coprocesseurs absents** : DSP-1/2, SuperFX, SA-1, C4, SDD-1, OBC-1 ;
  cartouches LoROM/HiROM simples seulement, pas d'ExHiROM ni de BS-X.
- **PPU** : modes 0-4 et 7 seulement (pas de 5/6 ni d'EXBG) ; fenêtres et
  math modélisées, mais le half sur l'addend fixe n'est pas tranché
  (le modèle halve toujours le second opérande).
- **Timing** : DMA lancé en plein milieu d'une tranche atterrit sur la
  frontière ; HTIME au-delà du dot 256 reste sur la frontière après le
  HDMA ; pas de comparaison contre une machine ou un émulateur de
  référence (toutes les empreintes valent le contrat interne natif ==
  interp, rien de plus).
- **SPC700** interprété uniquement (suffit au contrat) ; les jeux qui
  remettent le S-SMP à zéro en cours de jeu ne sont pas gérés ; le DIV en
  dépassement n'est pas exact.
- Collection locale : un seul ROM (Dr. Mario). Toute généralisation à
  « la collection » serait une affirmation sans preuve.

## Plan de la prochaine session

Dans l'ordre, un commit par pas, le contrat natif == interp et ctest vert
à chaque pas :

1. **Horloge du DSP à 32000 Hz** (le pas le plus rentable avant toute
   comparaison sonore) : `snes_smp.c:682` génère un échantillon tous les
   1024 cycles SPC — il en faut un tous les 32 (1024000/32000). À caler
   avec la partition SPC/65816 (le DSP tourne en parallèle, les ~17
   échantillons par trame deviennent ~533) ; le ring 8192 devient étroit
   (15 trames), l'agrandir si nécessaire. `apu_test` et les empreintes
   audio à re-enregistrer ; vérifier que les timers SPC ne glissent pas
   (ils partagent le compteur de ticks). Référence possible : le rapport
   cycles/échantillon de bsnes (`sfc/dsp/dsp.cpp`, un échantillon tous les
   2048 SPC ticks à 32000 Hz avec sa propre horloge — à relire avant).
2. **Coprocesseurs, DSP-1 d'abord** : détection du cart type $03 dans
   l'inspection de cartouche (`recompiler/src/system/snes/generator.cpp`
   et le `rom_offset`/cartouche du runtime), fenêtre $6000-$7FFF routée
   vers le DSP (DR sur $6000, SR sur $6001, le protocole de commandes),
   les commandes mathématiques calculées directement (multiplications de
   matrices, projections, MPU arithmétique) sur la référence
   Anomie/DSP-1. `dsp1_test` unitaire (les commandes les plus usitées :
   0x01 MPU, 0x0F projection, 0x0D paramètres). Puis ExHiROM dans
   `detect_hirom`/`header_map` de `snes_host.c` et son pendant du
   générateur. SuperFX et SA-1 restent derrière (de vrais cœurs).
3. **Comparatif de référence** (le précédent mednafen PCE se transpose :
   `logs/pce_mednafen_ref.c`, le bilan dans docs/PCE.fr.md) : un frontend
   libretro jetable dlopen'ant beetle-snes, dumps PPM aux mêmes
   checkpoints d'input rejoué, diff pixel contre le harnais sur Dr. Mario
   et Bucket. L'alignement d'input domine les écarts — mêmes checkpoints
   des deux côtés. Le bilan (matchs et écarts) va dans docs/SNES.fr.md,
   sans prétendre au-delà des captures.
4. **Derrière** : SuperFX/SA-1, la recompilation SPC700, les modes PPU
   5/6 et l'EXBG.

Règles de maison rappelées : ROMs et artefacts hors dépôt
(`output/`, `roms/`, `logs/` ignorés), empreintes jamais citées au-delà
de ce qui a tourné, commit auteur r043v + trailer
`Co-Authored-By: GLM <noreply@z.ai>`.

## Séance du 31 octobre — le pas 1 du plan, plus un écart trouvé sur le chemin

1. **Horloge du DSP à 32000 Hz** : `snes_smp.c` rend un échantillon
   stéréo tous les 32 cycles SPC (1024000/32000) au lieu de 1024 — le
   ring tourne au rythme d'une SNS, le WAV du harnais est honnête. Les
   timers 8/64 kHz ne glissent pas (leurs échelles sont indépendantes).
   Sur le chemin, `apu_test` a révélé un vrai bug du décodeur BRR : au
   bout d'un bloc bouclé (drapeau end+loop), la lecture repartait de
   l'adresse de boucle **plus 9 octets** — la voix tournait sur les
   données du bloc de boucle au lieu de son début. Resté invisible avec
   ~17 échantillons par trame ; les données du test bouclent désormais
   sur un bloc qui sonne pareil. Empreintes audio re-enregistrées
   (les images inchangées) :
   Dr. Mario 900 images avec `logs/dm_play.txt` :
   `run_hash=37b5db72 image_hash=f2a41fc5 audio_hash=873178f5` ;
   Bucket 120 images : `8fa1030b/b295c090/1e120d65` ;
   CPUADC `6494f940/a713d1ab/f071ecf5`, CPUMOV
   `4bba5727/53b0775f/d76df505`, CPUMSC `4289fb73/78729dc5/c2812a45` ;
   balayage Dr. Mario 300 images `run_hash=535acb2e`.
2. **Écart trouvé et fermé : la marche de contexte du recompilateur**.
   Le balayage sur les cartouches locales montrait CPUADC et bucket en
   désaccord natif/interp (un écart antérieur à la séance, les projets
   des cartouches de diagnostic n'avaient pas été rejoués après les
   chantiers 1-3). La cause : le groupement des régions enchaînait
   l'instance suivante sous le contexte *d'entrée* de l'instruction
   précédente — après un `SEP #$20`, la région enchaînait l'instance M
   clair de l'instruction suivante quand la marche l'avait aussi
   visitée (un `PLA` 16 bits après un SEP qui force 8 bits, ici).
   Correction : chaque instance porte son contexte *résultant*
   (`follow_ctx`) et les régions comme les `go()` du fin de région
   n'enchaînent que lui. Les quatre cartouches locales et Dr. Mario
   reviennent natif == interp, cycles compris (`logs/snes_sweep.txt`).
   Prochaine séance : reprendre le plan au pas 2 (DSP-1, puis ExHiROM).

## Séance du 30 octobre (suite) — le DSP-1 et l'ExHiROM

Chantier 4.1. Le DSP-1 (cart type $23/$30/$31 dans l'octet de map de
l'en-tête) : sa fenêtre de registres répond au protocole DR/SR (LoROM
banques $30-$3F/$B0-$BF dès $8000, HiROM banques $00-$0F/$80-$8F à
$6000-$7FFF ; octet pair DR, octet impair SR) avec la machine d'états
DRC/DRS/RQM, les commandes figées et le mode continu du raster ; les
commandes mathématiques sont calculées directement sur la
rétro-ingénierie publiée de la puce (Overload, The Dumper, Neviksti,
Andreas Naive) — multiplication, triangle, rayon/distance/range,
rotate/polar, les matrices d'attitude A/B/C et objective/subjective/
scalar, gyrate, et la projection complète (parameter/raster/target/
project) avec les tables de la ROM de données de la puce (constantes
publiées, `targets/snes/runtime/snes_dsp1.c`). `dsp1_test` couvre la
multiplication exacte, le triangle, le rayon, la commande de détection
$0F et deux invariants structurels (target(0,0) == (Cx,Cy) ; la
projection du point centre écran en (0,0)). Le balayage de la collection
re-tourné : Dr. Mario toujours `535acb2e`, cycles identiques (le DSP-1
n'y touche pas). **Aucune ROM DSP-1 dans la collection locale : le
protocole n'a pas tourné dans un jeu réel — dit tel quel dans
docs/SNES.fr.md.**

L'ExHiROM en même pas : `detect_map` (générateur et harnais) reconnaît
l'en-tête à $40FFC0 (checksum apparié, nibble de map 5), `snes_init`
prend le map 0/1/2, les fenêtres ($C0-$FF au début du fichier,
$00-$3F/$80-$BF à $400000) sont celles de la référence, la marche
statique de l'émetteur les lit pareil. Pas de ROM ExHiROM locale non
plus : non testé sur jeu réel.

La version d'état est à jour ; le prochain pas du plan reste SuperFX
(puis SA-1), derrière eux le comparatif de référence.

## Séance du 30 octobre (suite) — le SuperFX

Chantier 4.2 : le GSU interprété (`targets/snes/runtime/snes_fx.c`,
sémantiques publiées, le cœur bsnes comme référence de lecture) — les
seize registres, SFR, le pipeline, le cache d'instructions 512 octets,
les tampons ROM/RAM latents, le tracé (COLOR/PLOT/RPIX, cache de pixels,
dither, transparence) et la RAM 64 Kio du GSU avec sa fenêtre CPU
$70-$7F. Détection par l'en-tête (type $15, map $13/$14) dans le harnais
et le générateur (`SNES_FX`), fenêtre de registres $6000-$7FFF sur le
bus, IRQ du STOP sur la ligne du CPU, collisions de bus quand le GSU
tient le bus (le motif vecteur sur la ROM). Horloge déterministe : six
coups GSU par cycle CPU, crédités aux tranches de ligne et aux accès de
registre — le contrat native == interpréteur reste hashable. Le mode
précis sort des régions sur $6000-$7FFF (et désormais sur la fenêtre
DSP-1 $8000+ des banques $30-$3F, banque prise en compte). `fx_test`
passe (IWT/FROM/MULT/COLOR/PLOT/STOP à travers la fenêtre), `ctest` vert,
balayage 4/4, un projet généré Yoshi imprime les mêmes empreintes que le
harnais du dépôt.

**Ouvert** : Yoshi's Island n'amorce pas — le chargement du programme
S-SMP boucle sur le premier bloc du protocole IPL (32 octets/trame, des
$50 à l'infini, aucun terminate) ; la même boucle sans le GSU attaché,
donc le modèle S-SMP/IPL est en cause, pas le GSU. À creuser avec le
chantier 5 (le comparatif de référence tranchera ce que le vrai ROM IPL
répond). Suite du plan : SA-1, puis le comparatif.
