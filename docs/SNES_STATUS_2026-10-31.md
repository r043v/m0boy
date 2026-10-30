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
