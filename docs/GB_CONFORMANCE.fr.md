# Fidélité de m0boy (Game Boy) sur les ROMs de test — tableau de passage

Mesure faite le 2026-11-04 avec `tests/gbconform.c` du dépôt deepeel (outil d'observation, hors `ctest`) :
il exécute chaque ROM sur le cœur GB de m0boy tel que deepeel le pilote (variante MBC choisie par l'en-tête,
état post-boot DMG, **aucune boot ROM**), sans fenêtre, jusqu'à un verdict ou 8 000 trames (≈ 133 s émulées ;
600 pour gbmicrotest). Ce sont des observations de l'outil sur un cœur DMG, pas des mesures de matériel.

Protocoles de verdict reconnus (rien d'autre n'est deviné) :

- **Mooneye / wilbertpol / same-suite / AGE** : registres B,C,D,E,H,L = 3,5,8,13,21,34 à `ld b,b`/`ld d,d`
  ou à l'arrêt (HALT) = passe ; tous à `0x42` = échoue.
- **Blargg** : texte série contenant `Passed`/`Failed`, ou, pour les ROMs à RAM de cartouche, signature
  `DE B0 61` en `$A001` et code d'état en `$A000` (0 = passe).
- **gbmicrotest** : `$FF82` = `01` passe, `FF` échoue.
- **Sans verdict** : la ROM juge à l'écran (capture à comparer : dmg-acid2, mealybug, gambatte, scribbltests,
  AGE…) ou ne termine pas dans la limite de trames. Ce n'est ni un passage ni un échec constaté.

Limites à lire avant les chiffres : m0boy est DMG seul (les ROMs `-cgb*`, `-C`, `-A`, tout `same-suite`
et `cgb_sound` ciblent le CGB, leur échec est attendu) ; il n'émule pas de boot ROM (les `boot_*` échouent par
construction) ; l'APU est approximative (les suites de son échouent ou ne terminent pas).

## Synthèse par suite
| Suite | ROMs | ✅ passe | ❌ échoue | ❔ sans verdict |
|---|---:|---:|---:|---:|
| age-test-roms | 47 | 0 | 0 | 47 |
| blargg | 58 | 20 | 20 | 18 |
| bully | 1 | 0 | 0 | 1 |
| gbmicrotest | 513 | 354 | 127 | 32 |
| little-things-gb | 2 | 0 | 0 | 2 |
| mbc3-tester | 1 | 0 | 0 | 1 |
| mooneye-test-suite-wilbertpol | 121 | 23 | 0 | 98 |
| mooneye-test-suite | 115 | 31 | 68 | 16 |
| rtc3test | 1 | 0 | 0 | 1 |
| same-suite | 78 | 2 | 76 | 0 |
| scribbltests | 8 | 0 | 0 | 8 |
| strikethrough | 1 | 0 | 0 | 1 |
| turtle-tests | 2 | 0 | 0 | 2 |
| **total** | 948 | 430 | 291 | 227 |

Non exécutées ici (verdict visuel, pas de protocole machine) : gambatte (3 524 ROMs), mealybug-tearoom-tests (35),
dmg-acid2, cgb-acid2, cgb-acid-hell. Elles demandent une comparaison de capture d'écran avec une référence ;
voir « Reste ».


## Mode de référence `GBL_HOST_REF` (2026-11-04)

`-DGBL_HOST_REF=1` (hôte seulement, jamais sur firmware ; `gbl_options.h`) active deux modèles que les ROMs
de test observent et que les jeux n'exigent pas ; chacun se règle seul :

- `GBL_OAM_DMA_BUS` : le DMA OAM démarre deux M-cycles après l'écriture de `$FF46` (le précédent court jusque-là),
  dure 160 M-cycles ; pendant ce temps le CPU voit l'octet transféré sur le bus du DMA, ne peut pas y écrire, et
  trouve l'OAM fermée ; la copie vers l'OAM se fait au début effectif du transfert.
- `GBL_STACK_TIMING` : PUSH/CALL/RST écrivent, POP/RET/RETI lisent la pile dans leurs propres M-cycles, et les
  opérandes d'instruction sont lus à leur M-cycle (opcode +0, premier opérande +4, second +8).
- `GBL_TIMER_EDGE` (hôte, sans `GBL_TIMING_ASM`) : une écriture de DIV, ou de TAC qui éteint le signal, fait
  avancer TIMA sur le front descendant ; après un dépassement TIMA vaut 0 pendant un M-cycle avant le rechargement
  depuis TMA (écriture de TIMA avant : annule ; pendant : ignorée ; écriture de TMA pendant : TIMA la prend aussi).
- Entrée d'interruption (avec `GBL_STACK_TIMING`) : deux M-cycles à vide, octet haut, puis le vecteur est choisi
  d'après IE&IF, puis octet bas ; une écriture de IE pendant le push haut peut annuler (PC = $0000). Hors drapeau,
  l'interpréteur ne retarde plus l'IRQ d'un `EI` exécuté alors que IME vaut déjà 1 (`ei_sequence`).

Mesure (outil gbconform, 600 trames, cœur DMG sans boot ROM) : mooneye-test-suite 31 → 48 ROMs qui passent, aucune
régression ; gagnées : `add_sp_e_timing`, `call_*timing*`, `jp_*timing`, `ld_hl_sp_e_timing`, `oam_dma_restart/start/timing`,
`pop/push/ret*/reti/rst_timing`. Restent en échec parmi les timings : `di_timing-GS`, `halt_ime1_timing2-GS`
(synchronisation fine de l'IRQ). Avec `GBL_TIMER_EDGE`, 8 ROMs de plus (`timer/rapid_toggle`, `tim*_div_trigger`, `tima_reload`,
`tima_write_reloading`, `tma_write_reloading`) : 56 en tout, aucune régression ; `ei_sequence` et `interrupts/ie_push` portent le total à 58. Blargg `cpu_instrs` et `instr_timing` passent toujours. Observations de l'outil,
pas du matériel. Drapeau éteint (valeur par défaut), l'objet `gbl_core.c` pour Cortex-M0+ garde la même taille
(une instruction diffère par l'allocation d'un registre) ; je n'ai pas comparé de `firmware.bin` complet.

Tests `emulator-only` (MBC1/2/5) : ils passent tous (78 ROMs mooneye en tout, aucune régression, blargg inchangé).
Il fallait (1) le repli des banques à la taille réelle de la ROM : le harnais hôte définit `GBL_ROM_BANKS_VAR`
(`gbl_rom_banks`, par ROM), le firmware garde la constante ; (2) le registre ROMB0 du MBC5 démarre à 1 (il
démarrait à 0 : `$4000` lisait la banque 0) ; (3) le câblage multicart MBC1M (`gbl_mbc1m`, décalage 4, posé par
l'outil hôte d'après le logo dans les banques 0x10/0x20/0x30) ; (4) le RAM 32 Kio du MBC1 côté harnais. Le
firmware n'a que (2) et le correctif d'`EI` : `targets/meta/VERSION` passe à 0.10.61.

`GBL_PPU_REF` (hôte, ligne PPU en C) : le mode 2 de STAT se déclenche aussi au début de la ligne 144, et le
mode 0 commence un point plus tard que dans le modèle de lecture de STAT. Gagnés : `ppu/vblank_stat_intr-GS`,
`intr_2_0_timing`, `intr_2_mode0_timing`, `intr_2_oam_ok_timing`, `intr_2_mode3_timing` (mode 3 aussi un point plus tard), `intr_1_2_timing-GS` (ligne 153 de 460 points, ligne 0 de 452) : 84 ROMs mooneye en tout, aucune régression,
blargg : en plus `oam_bug/1-lcd_sync`. Restent : `intr_2_mode0_timing_sprites`, `hblank_ly_scx`, `lcdon_*`,
`stat_lyc_onoff`.

## mooneye-test-suite

| ROM | Verdict | Trames |
|---|---|---:|
| `acceptance/add_sp_e_timing.gb` | ❌ échoue | 13 |
| `acceptance/bits/mem_oam.gb` | ✅ passe | 10 |
| `acceptance/bits/reg_f.gb` | ✅ passe | 10 |
| `acceptance/bits/unused_hwio-GS.gb` | ❌ échoue | 10 |
| `acceptance/boot_div2-S.gb` | ❌ échoue | 10 |
| `acceptance/boot_div-dmg0.gb` | ❌ échoue | 10 |
| `acceptance/boot_div-dmgABCmgb.gb` | ✅ passe | 10 |
| `acceptance/boot_div-S.gb` | ❌ échoue | 10 |
| `acceptance/boot_hwio-dmg0.gb` | ❌ échoue | 10 |
| `acceptance/boot_hwio-dmgABCmgb.gb` | ❌ échoue | 10 |
| `acceptance/boot_hwio-S.gb` | ❌ échoue | 10 |
| `acceptance/boot_regs-dmg0.gb` | ❌ échoue | 10 |
| `acceptance/boot_regs-dmgABC.gb` | ✅ passe | 10 |
| `acceptance/boot_regs-mgb.gb` | ❌ échoue | 10 |
| `acceptance/boot_regs-sgb2.gb` | ❌ échoue | 10 |
| `acceptance/boot_regs-sgb.gb` | ❌ échoue | 10 |
| `acceptance/call_cc_timing2.gb` | ❌ échoue | 14 |
| `acceptance/call_cc_timing.gb` | ❌ échoue | 12 |
| `acceptance/call_timing2.gb` | ❌ échoue | 14 |
| `acceptance/call_timing.gb` | ❌ échoue | 12 |
| `acceptance/di_timing-GS.gb` | ❌ échoue | 15 |
| `acceptance/div_timing.gb` | ✅ passe | 10 |
| `acceptance/ei_sequence.gb` | ❌ échoue | 10 |
| `acceptance/ei_timing.gb` | ✅ passe | 10 |
| `acceptance/halt_ime0_ei.gb` | ✅ passe | 10 |
| `acceptance/halt_ime0_nointr_timing.gb` | ✅ passe | 14 |
| `acceptance/halt_ime1_timing2-GS.gb` | ❌ échoue | 18 |
| `acceptance/halt_ime1_timing.gb` | ✅ passe | 10 |
| `acceptance/if_ie_registers.gb` | ✅ passe | 10 |
| `acceptance/instr/daa.gb` | ✅ passe | 22 |
| `acceptance/interrupts/ie_push.gb` | ❌ échoue | 10 |
| `acceptance/intr_timing.gb` | ✅ passe | 10 |
| `acceptance/jp_cc_timing.gb` | ❌ échoue | 12 |
| `acceptance/jp_timing.gb` | ❌ échoue | 12 |
| `acceptance/ld_hl_sp_e_timing.gb` | ❌ échoue | 13 |
| `acceptance/oam_dma/basic.gb` | ✅ passe | 10 |
| `acceptance/oam_dma/reg_read.gb` | ✅ passe | 10 |
| `acceptance/oam_dma_restart.gb` | ❌ échoue | 13 |
| `acceptance/oam_dma/sources-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/oam_dma_start.gb` | ❌ échoue | 12 |
| `acceptance/oam_dma_timing.gb` | ❌ échoue | 13 |
| `acceptance/pop_timing.gb` | ❌ échoue | 10 |
| `acceptance/ppu/hblank_ly_scx_timing-GS.gb` | ❌ échoue | 17 |
| `acceptance/ppu/intr_1_2_timing-GS.gb` | ❌ échoue | 13 |
| `acceptance/ppu/intr_2_0_timing.gb` | ❌ échoue | 12 |
| `acceptance/ppu/intr_2_mode0_timing.gb` | ❌ échoue | 12 |
| `acceptance/ppu/intr_2_mode0_timing_sprites.gb` | ❌ échoue | 19 |
| `acceptance/ppu/intr_2_mode3_timing.gb` | ❌ échoue | 12 |
| `acceptance/ppu/intr_2_oam_ok_timing.gb` | ❌ échoue | 12 |
| `acceptance/ppu/lcdon_timing-GS.gb` | ❌ échoue | 18 |
| `acceptance/ppu/lcdon_write_timing-GS.gb` | ❌ échoue | 34 |
| `acceptance/ppu/stat_irq_blocking.gb` | ✅ passe | 11 |
| `acceptance/ppu/stat_lyc_onoff.gb` | ❌ échoue | 10 |
| `acceptance/ppu/vblank_stat_intr-GS.gb` | ❌ échoue | 18 |
| `acceptance/push_timing.gb` | ❌ échoue | 13 |
| `acceptance/rapid_di_ei.gb` | ✅ passe | 10 |
| `acceptance/ret_cc_timing.gb` | ❌ échoue | 12 |
| `acceptance/reti_intr_timing.gb` | ✅ passe | 10 |
| `acceptance/reti_timing.gb` | ❌ échoue | 12 |
| `acceptance/ret_timing.gb` | ❌ échoue | 12 |
| `acceptance/rst_timing.gb` | ❌ échoue | 13 |
| `acceptance/serial/boot_sclk_align-dmgABCmgb.gb` | ❌ échoue | 10 |
| `acceptance/timer/div_write.gb` | ✅ passe | 51 |
| `acceptance/timer/rapid_toggle.gb` | ❌ échoue | 10 |
| `acceptance/timer/tim00_div_trigger.gb` | ❌ échoue | 10 |
| `acceptance/timer/tim00.gb` | ✅ passe | 10 |
| `acceptance/timer/tim01_div_trigger.gb` | ❌ échoue | 10 |
| `acceptance/timer/tim01.gb` | ✅ passe | 10 |
| `acceptance/timer/tim10_div_trigger.gb` | ❌ échoue | 10 |
| `acceptance/timer/tim10.gb` | ✅ passe | 10 |
| `acceptance/timer/tim11_div_trigger.gb` | ❌ échoue | 10 |
| `acceptance/timer/tim11.gb` | ✅ passe | 10 |
| `acceptance/timer/tima_reload.gb` | ❌ échoue | 10 |
| `acceptance/timer/tima_write_reloading.gb` | ❌ échoue | 10 |
| `acceptance/timer/tma_write_reloading.gb` | ❌ échoue | 10 |
| `emulator-only/mbc1/bits_bank1.gb` | ✅ passe | 178 |
| `emulator-only/mbc1/bits_bank2.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc1/bits_mode.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc1/bits_ramg.gb` | ✅ passe | 348 |
| `emulator-only/mbc1/multicart_rom_8Mb.gb` | ❌ échoue | 11 |
| `emulator-only/mbc1/ram_256kb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc1/ram_64kb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc1/rom_16Mb.gb` | ✅ passe | 13 |
| `emulator-only/mbc1/rom_1Mb.gb` | ❌ échoue | 11 |
| `emulator-only/mbc1/rom_2Mb.gb` | ❌ échoue | 11 |
| `emulator-only/mbc1/rom_4Mb.gb` | ❌ échoue | 11 |
| `emulator-only/mbc1/rom_512kb.gb` | ❌ échoue | 11 |
| `emulator-only/mbc1/rom_8Mb.gb` | ❌ échoue | 11 |
| `emulator-only/mbc2/bits_ramg.gb` | ✅ passe | 355 |
| `emulator-only/mbc2/bits_romb.gb` | ✅ passe | 186 |
| `emulator-only/mbc2/bits_unused.gb` | ✅ passe | 174 |
| `emulator-only/mbc2/ram.gb` | ✅ passe | 28 |
| `emulator-only/mbc2/rom_1Mb.gb` | ❌ échoue | 10 |
| `emulator-only/mbc2/rom_2Mb.gb` | ✅ passe | 10 |
| `emulator-only/mbc2/rom_512kb.gb` | ❌ échoue | 10 |
| `emulator-only/mbc5/rom_16Mb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_1Mb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_2Mb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_32Mb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_4Mb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_512kb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_64Mb.gb` | ❔ sans verdict | 8000 |
| `emulator-only/mbc5/rom_8Mb.gb` | ❔ sans verdict | 8000 |
| `madness/mgb_oam_dma_halt_sprites.gb` | ❔ sans verdict | 19 |
| `manual-only/sprite_priority.gb` | ❔ sans verdict | 8000 |
| `misc/bits/unused_hwio-C.gb` | ❌ échoue | 10 |
| `misc/boot_div-A.gb` | ❌ échoue | 10 |
| `misc/boot_div-cgb0.gb` | ❌ échoue | 10 |
| `misc/boot_div-cgbABCDE.gb` | ❌ échoue | 10 |
| `misc/boot_hwio-C.gb` | ❌ échoue | 10 |
| `misc/boot_regs-A.gb` | ❌ échoue | 10 |
| `misc/boot_regs-cgb.gb` | ❌ échoue | 10 |
| `misc/ppu/vblank_stat_intr-C.gb` | ❌ échoue | 18 |
| `utils/bootrom_dumper.gb` | ❔ sans verdict | 12 |
| `utils/dump_boot_hwio.gb` | ✅ passe | 10 |

## mooneye-test-suite-wilbertpol

| ROM | Verdict | Trames |
|---|---|---:|
| `acceptance/add_sp_e_timing.gb` | ❔ sans verdict | 19 |
| `acceptance/bits/mem_oam.gb` | ✅ passe | 17 |
| `acceptance/bits/reg_f.gb` | ✅ passe | 16 |
| `acceptance/bits/unused_hwio-GS.gb` | ❔ sans verdict | 16 |
| `acceptance/boot_hwio-G.gb` | ❔ sans verdict | 16 |
| `acceptance/boot_regs-dmg.gb` | ✅ passe | 16 |
| `acceptance/call_cc_timing2.gb` | ❔ sans verdict | 20 |
| `acceptance/call_cc_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/call_timing2.gb` | ❔ sans verdict | 20 |
| `acceptance/call_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/di_timing-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/div_timing.gb` | ✅ passe | 16 |
| `acceptance/ei_timing.gb` | ✅ passe | 16 |
| `acceptance/gpu/hblank_ly_scx_timing-C.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/hblank_ly_scx_timing-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/hblank_ly_scx_timing_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/hblank_ly_scx_timing_variant_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_0_timing.gb` | ❔ sans verdict | 20 |
| `acceptance/gpu/intr_1_2_timing-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_1_timing.gb` | ❔ sans verdict | 21 |
| `acceptance/gpu/intr_2_0_timing.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_scx1_timing_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_scx2_timing_nops.gb` | ✅ passe | 8000 |
| `acceptance/gpu/intr_2_mode0_scx3_timing_nops.gb` | ✅ passe | 8000 |
| `acceptance/gpu/intr_2_mode0_scx4_timing_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_scx5_timing_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_scx6_timing_nops.gb` | ✅ passe | 8000 |
| `acceptance/gpu/intr_2_mode0_scx7_timing_nops.gb` | ✅ passe | 8000 |
| `acceptance/gpu/intr_2_mode0_scx8_timing_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing_sprites.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing_sprites_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing_sprites_scx1_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing_sprites_scx2_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing_sprites_scx3_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode0_timing_sprites_scx4_nops.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_mode3_timing.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_oam_ok_timing.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/intr_2_timing.gb` | ❔ sans verdict | 24 |
| `acceptance/gpu/lcdon_mode_timing.gb` | ❔ sans verdict | 24 |
| `acceptance/gpu/ly00_01_mode0_2.gb` | ❔ sans verdict | 28 |
| `acceptance/gpu/ly00_mode0_2-GS.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly00_mode1_0-GS.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly00_mode1_2-C.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly00_mode2_3.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly00_mode3_0.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly143_144_145.gb` | ❔ sans verdict | 28 |
| `acceptance/gpu/ly143_144_152_153.gb` | ❔ sans verdict | 28 |
| `acceptance/gpu/ly143_144_mode0_1.gb` | ❔ sans verdict | 25 |
| `acceptance/gpu/ly143_144_mode3_0.gb` | ❔ sans verdict | 25 |
| `acceptance/gpu/ly_lyc_0-C.gb` | ❔ sans verdict | 40 |
| `acceptance/gpu/ly_lyc_0-GS.gb` | ❔ sans verdict | 40 |
| `acceptance/gpu/ly_lyc_0_write-C.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/ly_lyc_0_write-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/ly_lyc_144-C.gb` | ❔ sans verdict | 26 |
| `acceptance/gpu/ly_lyc_144-GS.gb` | ❔ sans verdict | 26 |
| `acceptance/gpu/ly_lyc_153-C.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly_lyc_153-GS.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly_lyc_153_write-C.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/ly_lyc_153_write-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/ly_lyc-C.gb` | ❔ sans verdict | 24 |
| `acceptance/gpu/ly_lyc-GS.gb` | ❔ sans verdict | 24 |
| `acceptance/gpu/ly_lyc_write-C.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/ly_lyc_write-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/ly_new_frame-C.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/ly_new_frame-GS.gb` | ❔ sans verdict | 32 |
| `acceptance/gpu/stat_irq_blocking.gb` | ✅ passe | 8000 |
| `acceptance/gpu/stat_write_if-C.gb` | ❔ sans verdict | 37 |
| `acceptance/gpu/stat_write_if-GS.gb` | ❔ sans verdict | 24 |
| `acceptance/gpu/vblank_if_timing.gb` | ❔ sans verdict | 8000 |
| `acceptance/gpu/vblank_stat_intr-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/halt_ime0_ei.gb` | ✅ passe | 8000 |
| `acceptance/halt_ime0_nointr_timing.gb` | ✅ passe | 8000 |
| `acceptance/halt_ime1_timing2-GS.gb` | ❔ sans verdict | 8000 |
| `acceptance/halt_ime1_timing.gb` | ✅ passe | 8000 |
| `acceptance/if_ie_registers.gb` | ✅ passe | 16 |
| `acceptance/intr_timing.gb` | ✅ passe | 16 |
| `acceptance/jp_cc_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/jp_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/ld_hl_sp_e_timing.gb` | ❔ sans verdict | 19 |
| `acceptance/oam_dma_restart.gb` | ❔ sans verdict | 19 |
| `acceptance/oam_dma_start.gb` | ❔ sans verdict | 20 |
| `acceptance/oam_dma_timing.gb` | ❔ sans verdict | 19 |
| `acceptance/pop_timing.gb` | ❔ sans verdict | 16 |
| `acceptance/push_timing.gb` | ❔ sans verdict | 19 |
| `acceptance/rapid_di_ei.gb` | ✅ passe | 16 |
| `acceptance/ret_cc_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/reti_intr_timing.gb` | ✅ passe | 8000 |
| `acceptance/reti_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/ret_timing.gb` | ❔ sans verdict | 18 |
| `acceptance/rst_timing.gb` | ❔ sans verdict | 19 |
| `acceptance/timer/div_write.gb` | ✅ passe | 8000 |
| `acceptance/timer/rapid_toggle.gb` | ❔ sans verdict | 8000 |
| `acceptance/timer/tim00_div_trigger.gb` | ❔ sans verdict | 16 |
| `acceptance/timer/tim00.gb` | ✅ passe | 16 |
| `acceptance/timer/tim01_div_trigger.gb` | ❔ sans verdict | 16 |
| `acceptance/timer/tim01.gb` | ✅ passe | 16 |
| `acceptance/timer/tim10_div_trigger.gb` | ❔ sans verdict | 16 |
| `acceptance/timer/tim10.gb` | ✅ passe | 16 |
| `acceptance/timer/tim11_div_trigger.gb` | ❔ sans verdict | 16 |
| `acceptance/timer/tim11.gb` | ✅ passe | 16 |
| `acceptance/timer/tima_reload.gb` | ❔ sans verdict | 16 |
| `acceptance/timer/tima_write_reloading.gb` | ❔ sans verdict | 16 |
| `acceptance/timer/timer_if.gb` | ✅ passe | 8000 |
| `acceptance/timer/tma_write_reloading.gb` | ❔ sans verdict | 16 |
| `emulator-only/mbc1_rom_4banks.gb` | ❔ sans verdict | 16 |
| `logic-analysis/external-bus/read_timing/read_timing.gb` | ❔ sans verdict | 9 |
| `logic-analysis/external-bus/write_timing/write_timing.gb` | ❔ sans verdict | 9 |
| `logic-analysis/ppu/simple_scx/simple_scx.gb` | ❔ sans verdict | 8000 |
| `madness/mgb_oam_dma_halt_sprites.gb` | ❔ sans verdict | 18 |
| `manual-only/sprite_priority.gb` | ❔ sans verdict | 15 |
| `misc/bits/unused_hwio-C.gb` | ❔ sans verdict | 16 |
| `misc/boot_hwio-C.gb` | ❔ sans verdict | 16 |
| `misc/boot_hwio-S.gb` | ❔ sans verdict | 16 |
| `misc/boot_regs-A.gb` | ❔ sans verdict | 16 |
| `misc/boot_regs-cgb.gb` | ❔ sans verdict | 16 |
| `misc/boot_regs-mgb.gb` | ❔ sans verdict | 16 |
| `misc/boot_regs-sgb2.gb` | ❔ sans verdict | 16 |
| `misc/boot_regs-sgb.gb` | ❔ sans verdict | 16 |
| `misc/gpu/vblank_stat_intr-C.gb` | ❔ sans verdict | 8000 |
| `utils/dump_boot_hwio.gb` | ❔ sans verdict | 15 |

## same-suite

| ROM | Verdict | Trames |
|---|---|---:|
| `apu/channel_1/channel_1_align_cpu.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_align.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_delay.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_duty_delay.gb` | ❌ échoue | 5 |
| `apu/channel_1/channel_1_duty.gb` | ❌ échoue | 5 |
| `apu/channel_1/channel_1_extra_length_clocking-cgb0B.gb` | ❌ échoue | 9 |
| `apu/channel_1/channel_1_freq_change.gb` | ❌ échoue | 5 |
| `apu/channel_1/channel_1_freq_change_timing-A.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_freq_change_timing-cgb0BC.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_freq_change_timing-cgbDE.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_nrx2_glitch.gb` | ❌ échoue | 20 |
| `apu/channel_1/channel_1_nrx2_speed_change.gb` | ❌ échoue | 156 |
| `apu/channel_1/channel_1_restart.gb` | ❌ échoue | 5 |
| `apu/channel_1/channel_1_restart_nrx2_glitch.gb` | ❌ échoue | 4 |
| `apu/channel_1/channel_1_stop_div.gb` | ❌ échoue | 18 |
| `apu/channel_1/channel_1_stop_restart.gb` | ❌ échoue | 5 |
| `apu/channel_1/channel_1_sweep.gb` | ❌ échoue | 59 |
| `apu/channel_1/channel_1_sweep_restart_2.gb` | ❌ échoue | 49 |
| `apu/channel_1/channel_1_sweep_restart.gb` | ❌ échoue | 55 |
| `apu/channel_1/channel_1_volume_div.gb` | ❌ échoue | 97 |
| `apu/channel_1/channel_1_volume.gb` | ❌ échoue | 5 |
| `apu/channel_2/channel_2_align_cpu.gb` | ❌ échoue | 4 |
| `apu/channel_2/channel_2_align.gb` | ❌ échoue | 4 |
| `apu/channel_2/channel_2_delay.gb` | ❌ échoue | 4 |
| `apu/channel_2/channel_2_duty_delay.gb` | ❌ échoue | 5 |
| `apu/channel_2/channel_2_duty.gb` | ❌ échoue | 5 |
| `apu/channel_2/channel_2_extra_length_clocking-cgb0B.gb` | ❌ échoue | 9 |
| `apu/channel_2/channel_2_freq_change.gb` | ❌ échoue | 5 |
| `apu/channel_2/channel_2_nrx2_glitch.gb` | ❌ échoue | 20 |
| `apu/channel_2/channel_2_nrx2_speed_change.gb` | ❌ échoue | 156 |
| `apu/channel_2/channel_2_restart.gb` | ❌ échoue | 5 |
| `apu/channel_2/channel_2_restart_nrx2_glitch.gb` | ❌ échoue | 4 |
| `apu/channel_2/channel_2_stop_div.gb` | ❌ échoue | 18 |
| `apu/channel_2/channel_2_stop_restart.gb` | ❌ échoue | 5 |
| `apu/channel_2/channel_2_volume_div.gb` | ❌ échoue | 49 |
| `apu/channel_2/channel_2_volume.gb` | ❌ échoue | 6 |
| `apu/channel_3/channel_3_and_glitch.gb` | ❌ échoue | 5 |
| `apu/channel_3/channel_3_delay.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_extra_length_clocking-cgb0.gb` | ❌ échoue | 11 |
| `apu/channel_3/channel_3_extra_length_clocking-cgbB.gb` | ❌ échoue | 11 |
| `apu/channel_3/channel_3_first_sample.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_freq_change_delay.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_restart_delay.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_restart_during_delay.gb` | ❌ échoue | 5 |
| `apu/channel_3/channel_3_restart_stop_delay.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_shift_delay.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_shift_skip_delay.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_stop_delay.gb` | ✅ passe | 4 |
| `apu/channel_3/channel_3_stop_div.gb` | ❌ échoue | 35 |
| `apu/channel_3/channel_3_wave_ram_dac_on_rw.gb` | ✅ passe | 4 |
| `apu/channel_3/channel_3_wave_ram_locked_write.gb` | ❌ échoue | 4 |
| `apu/channel_3/channel_3_wave_ram_sync.gb` | ❌ échoue | 4 |
| `apu/channel_4/channel_4_align.gb` | ❌ échoue | 4 |
| `apu/channel_4/channel_4_delay.gb` | ❌ échoue | 4 |
| `apu/channel_4/channel_4_equivalent_frequencies.gb` | ❌ échoue | 6 |
| `apu/channel_4/channel_4_extra_length_clocking-cgb0B.gb` | ❌ échoue | 9 |
| `apu/channel_4/channel_4_freq_change.gb` | ❌ échoue | 5 |
| `apu/channel_4/channel_4_frequency_alignment.gb` | ❌ échoue | 5 |
| `apu/channel_4/channel_4_lfsr_15_7.gb` | ❌ échoue | 7 |
| `apu/channel_4/channel_4_lfsr15.gb` | ❌ échoue | 6 |
| `apu/channel_4/channel_4_lfsr_7_15.gb` | ❌ échoue | 7 |
| `apu/channel_4/channel_4_lfsr.gb` | ❌ échoue | 6 |
| `apu/channel_4/channel_4_lfsr_restart_fast.gb` | ❌ échoue | 6 |
| `apu/channel_4/channel_4_lfsr_restart.gb` | ❌ échoue | 7 |
| `apu/channel_4/channel_4_volume_div.gb` | ❌ échoue | 97 |
| `apu/div_trigger_volume_10.gb` | ❌ échoue | 77 |
| `apu/div_write_trigger_10.gb` | ❌ échoue | 62 |
| `apu/div_write_trigger.gb` | ❌ échoue | 61 |
| `apu/div_write_trigger_volume_10.gb` | ❌ échoue | 48 |
| `apu/div_write_trigger_volume.gb` | ❌ échoue | 48 |
| `dma/gbc_dma_cont.gb` | ❌ échoue | 5 |
| `dma/gdma_addr_mask.gb` | ❌ échoue | 5 |
| `dma/hdma_lcd_off.gb` | ❌ échoue | 5 |
| `dma/hdma_mode0.gb` | ❌ échoue | 7 |
| `interrupt/ei_delay_halt.gb` | ❌ échoue | 4 |
| `ppu/blocking_bgpi_increase.gb` | ❌ échoue | 6 |
| `sgb/command_mlt_req_1_incrementing.gb` | ❌ échoue | 8 |
| `sgb/command_mlt_req.gb` | ❌ échoue | 183 |

## age-test-roms

| ROM | Verdict | Trames |
|---|---|---:|
| `halt/ei-halt-dmgC-cgbBCE.gb` | ❔ sans verdict | 12 |
| `halt/halt-m0-interrupt-dmgC-cgbBCE.gb` | ❔ sans verdict | 13 |
| `halt/halt-prefetch-dmgC-cgbBCE.gb` | ❔ sans verdict | 8000 |
| `lcd-align-ly/lcd-align-ly-cgbBC.gb` | ❔ sans verdict | 12 |
| `lcd-align-ly/lcd-align-ly-cgbE.gb` | ❔ sans verdict | 12 |
| `ly/ly-cgbE.gb` | ❔ sans verdict | 20 |
| `ly/ly-dmgC-cgbBC.gb` | ❔ sans verdict | 20 |
| `ly/ly-ncmBC.gb` | ❔ sans verdict | 12 |
| `ly/ly-ncmE.gb` | ❔ sans verdict | 12 |
| `m3-bg-bgp/m3-bg-bgp.gb` | ❔ sans verdict | 8000 |
| `m3-bg-lcdc/m3-bg-lcdc-ds.gb` | ❔ sans verdict | 12 |
| `m3-bg-lcdc/m3-bg-lcdc.gb` | ❔ sans verdict | 8000 |
| `m3-bg-lcdc/m3-bg-lcdc-nocgb.gb` | ❔ sans verdict | 8000 |
| `m3-bg-scx/m3-bg-scx-ds.gb` | ❔ sans verdict | 12 |
| `m3-bg-scx/m3-bg-scx.gb` | ❔ sans verdict | 8000 |
| `m3-bg-scx/m3-bg-scx-nocgb.gb` | ❔ sans verdict | 8000 |
| `oam/oam-read-cgbE.gb` | ❔ sans verdict | 92 |
| `oam/oam-read-dmgC-cgbBC.gb` | ❔ sans verdict | 92 |
| `oam/oam-read-ncmBC.gb` | ❔ sans verdict | 12 |
| `oam/oam-read-ncmE.gb` | ❔ sans verdict | 12 |
| `oam/oam-write-cgbBCE.gb` | ❔ sans verdict | 93 |
| `oam/oam-write-dmgC.gb` | ❔ sans verdict | 93 |
| `oam/oam-write-ncmBCE.gb` | ❔ sans verdict | 12 |
| `speed-switch/caution/spsw-interrupts-cgbBC.gb` | ❔ sans verdict | 12 |
| `speed-switch/caution/spsw-interrupts-cgbE.gb` | ❔ sans verdict | 12 |
| `speed-switch/spsw-ch2-lc-delay-cgbBCE.gb` | ❔ sans verdict | 12 |
| `speed-switch/spsw-div-cgbBCE.gb` | ❔ sans verdict | 12 |
| `speed-switch/spsw-mode0-cgbBCE.gb` | ❔ sans verdict | 12 |
| `speed-switch/spsw-stop-prefetch-cgbBCE.gb` | ❔ sans verdict | 12 |
| `speed-switch/spsw-tima-cgbBC.gb` | ❔ sans verdict | 12 |
| `speed-switch/spsw-tima-cgbE.gb` | ❔ sans verdict | 12 |
| `stat-interrupt/stat-int-dmgC-cgbBCE.gb` | ❔ sans verdict | 44 |
| `stat-interrupt/stat-int-ncmBCE.gb` | ❔ sans verdict | 12 |
| `stat-mode-sprites/stat-mode-sprites-dmgC-cgbBCE.gb` | ❔ sans verdict | 18 |
| `stat-mode-sprites/stat-mode-sprites-ds-cgbBCE.gb` | ❔ sans verdict | 12 |
| `stat-mode/stat-mode-cgbE.gb` | ❔ sans verdict | 93 |
| `stat-mode/stat-mode-dmgC-cgbBC.gb` | ❔ sans verdict | 93 |
| `stat-mode/stat-mode-ds-cgbBCE.gb` | ❔ sans verdict | 12 |
| `stat-mode/stat-mode-ncmBC.gb` | ❔ sans verdict | 12 |
| `stat-mode/stat-mode-ncmE.gb` | ❔ sans verdict | 12 |
| `stat-mode-window/stat-mode-window-cgbBCE.gb` | ❔ sans verdict | 29 |
| `stat-mode-window/stat-mode-window-dmgC.gb` | ❔ sans verdict | 29 |
| `stat-mode-window/stat-mode-window-ds-cgbBCE.gb` | ❔ sans verdict | 12 |
| `stat-mode-window/stat-mode-window-ncmBCE.gb` | ❔ sans verdict | 29 |
| `vram/vram-read-cgbBCE.gb` | ❔ sans verdict | 93 |
| `vram/vram-read-dmgC.gb` | ❔ sans verdict | 93 |
| `vram/vram-read-ncmBCE.gb` | ❔ sans verdict | 12 |

## blargg

| ROM | Verdict | Trames |
|---|---|---:|
| `cgb_sound/cgb_sound.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/01-registers.gb` | ❌ échoue | 59 |
| `cgb_sound/rom_singles/02-len ctr.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/03-trigger.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/04-sweep.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/05-sweep details.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/06-overflow on trigger.gb` | ✅ passe | 59 |
| `cgb_sound/rom_singles/07-len sweep period sync.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/08-len ctr during power.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/09-wave read while on.gb` | ❌ échoue | 59 |
| `cgb_sound/rom_singles/10-wave trigger while on.gb` | ✅ passe | 239 |
| `cgb_sound/rom_singles/11-regs after power.gb` | ❔ sans verdict | 8000 |
| `cgb_sound/rom_singles/12-wave.gb` | ❌ échoue | 29 |
| `cpu_instrs/cpu_instrs.gb` | ✅ passe | 3193 |
| `cpu_instrs/individual/01-special.gb` | ✅ passe | 138 |
| `cpu_instrs/individual/02-interrupts.gb` | ✅ passe | 24 |
| `cpu_instrs/individual/03-op sp,hl.gb` | ✅ passe | 139 |
| `cpu_instrs/individual/04-op r,imm.gb` | ✅ passe | 162 |
| `cpu_instrs/individual/05-op rp.gb` | ✅ passe | 223 |
| `cpu_instrs/individual/06-ld r,r.gb` | ✅ passe | 33 |
| `cpu_instrs/individual/07-jr,jp,call,ret,rst.gb` | ✅ passe | 41 |
| `cpu_instrs/individual/08-misc instrs.gb` | ✅ passe | 31 |
| `cpu_instrs/individual/09-op r,r.gb` | ✅ passe | 544 |
| `cpu_instrs/individual/10-bit ops.gb` | ✅ passe | 827 |
| `cpu_instrs/individual/11-op a,(hl).gb` | ✅ passe | 1049 |
| `dmg_sound/dmg_sound.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/01-registers.gb` | ❌ échoue | 59 |
| `dmg_sound/rom_singles/02-len ctr.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/03-trigger.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/04-sweep.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/05-sweep details.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/06-overflow on trigger.gb` | ✅ passe | 59 |
| `dmg_sound/rom_singles/07-len sweep period sync.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/08-len ctr during power.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/09-wave read while on.gb` | ❌ échoue | 59 |
| `dmg_sound/rom_singles/10-wave trigger while on.gb` | ❌ échoue | 239 |
| `dmg_sound/rom_singles/11-regs after power.gb` | ❔ sans verdict | 8000 |
| `dmg_sound/rom_singles/12-wave write while on.gb` | ❌ échoue | 239 |
| `halt_bug.gb` | ❔ sans verdict | 8000 |
| `instr_timing/instr_timing.gb` | ✅ passe | 37 |
| `interrupt_time/interrupt_time.gb` | ❔ sans verdict | 8000 |
| `mem_timing-2/mem_timing.gb` | ❌ échoue | 179 |
| `mem_timing-2/rom_singles/01-read_timing.gb` | ❌ échoue | 59 |
| `mem_timing-2/rom_singles/02-write_timing.gb` | ✅ passe | 29 |
| `mem_timing-2/rom_singles/03-modify_timing.gb` | ❌ échoue | 59 |
| `mem_timing/individual/01-read_timing.gb` | ❌ échoue | 41 |
| `mem_timing/individual/02-write_timing.gb` | ✅ passe | 25 |
| `mem_timing/individual/03-modify_timing.gb` | ❌ échoue | 102 |
| `mem_timing/mem_timing.gb` | ❌ échoue | 97 |
| `oam_bug/oam_bug.gb` | ❌ échoue | 779 |
| `oam_bug/rom_singles/1-lcd_sync.gb` | ❌ échoue | 59 |
| `oam_bug/rom_singles/2-causes.gb` | ❌ échoue | 59 |
| `oam_bug/rom_singles/3-non_causes.gb` | ✅ passe | 119 |
| `oam_bug/rom_singles/4-scanline_timing.gb` | ❌ échoue | 59 |
| `oam_bug/rom_singles/5-timing_bug.gb` | ❌ échoue | 59 |
| `oam_bug/rom_singles/6-timing_no_bug.gb` | ✅ passe | 89 |
| `oam_bug/rom_singles/7-timing_effect.gb` | ❌ échoue | 389 |
| `oam_bug/rom_singles/8-instr_effect.gb` | ❌ échoue | 59 |

## bully

| ROM | Verdict | Trames |
|---|---|---:|
| `bully.gb` | ❔ sans verdict | 8000 |

## strikethrough

| ROM | Verdict | Trames |
|---|---|---:|
| `strikethrough.gb` | ❔ sans verdict | 8000 |

## turtle-tests

| ROM | Verdict | Trames |
|---|---|---:|
| `window_y_trigger/window_y_trigger.gb` | ❔ sans verdict | 8000 |
| `window_y_trigger_wx_offscreen/window_y_trigger_wx_offscreen.gb` | ❔ sans verdict | 8000 |

## little-things-gb

| ROM | Verdict | Trames |
|---|---|---:|
| `firstwhite.gb` | ❔ sans verdict | 8000 |
| `tellinglys.gb` | ❔ sans verdict | 8000 |

## mbc3-tester

| ROM | Verdict | Trames |
|---|---|---:|
| `mbc3-tester.gb` | ❔ sans verdict | 8000 |

## rtc3test

| ROM | Verdict | Trames |
|---|---|---:|
| `rtc3test.gb` | ❔ sans verdict | 8000 |

## scribbltests

| ROM | Verdict | Trames |
|---|---|---:|
| `fairylake/fairylake.gb` | ❔ sans verdict | 8000 |
| `lycscx/lycscx.gb` | ❔ sans verdict | 8000 |
| `lycscy/lycscy.gb` | ❔ sans verdict | 8000 |
| `palettely/palettely.gb` | ❔ sans verdict | 8000 |
| `scxly/scxly.gb` | ❔ sans verdict | 8000 |
| `statcount/statcount-auto.gb` | ❔ sans verdict | 8000 |
| `statcount/statcount.gb` | ❔ sans verdict | 8000 |
| `winpos/winpos.gb` | ❔ sans verdict | 8000 |

## gbmicrotest — échecs et ROMs sans verdict

| ROM | Verdict |
|---|---|
| `000-oam_lock.gb` | ❔ sans verdict |
| `000-write_to_x8000.gb` | ❔ sans verdict |
| `001-vram_unlocked.gb` | ❔ sans verdict |
| `002-vram_locked.gb` | ❔ sans verdict |
| `004-tima_boot_phase.gb` | ❔ sans verdict |
| `004-tima_cycle_timer.gb` | ❔ sans verdict |
| `007-lcd_on_stat.gb` | ❔ sans verdict |
| `400-dma.gb` | ❔ sans verdict |
| `500-scx-timing.gb` | ❔ sans verdict |
| `800-ppu-latch-scx.gb` | ❔ sans verdict |
| `801-ppu-latch-scy.gb` | ❔ sans verdict |
| `802-ppu-latch-tileselect.gb` | ❔ sans verdict |
| `803-ppu-latch-bgdisplay.gb` | ❔ sans verdict |
| `audio_testbench.gb` | ❔ sans verdict |
| `cpu_bus_1.gb` | ❔ sans verdict |
| `dma_basic.gb` | ❔ sans verdict |
| `dma_timing_a.gb` | ❌ échoue |
| `flood_vram.gb` | ❔ sans verdict |
| `halt_op_dupe_delay.gb` | ❌ échoue |
| `halt_op_dupe.gb` | ❌ échoue |
| `hblank_int_scx0_if_a.gb` | ❌ échoue |
| `hblank_int_scx0_if_b.gb` | ❌ échoue |
| `hblank_int_scx0_if_c.gb` | ❌ échoue |
| `hblank_int_scx1_if_a.gb` | ❌ échoue |
| `hblank_int_scx1_if_b.gb` | ❌ échoue |
| `hblank_int_scx1_if_c.gb` | ❌ échoue |
| `hblank_int_scx1_nops_b.gb` | ❌ échoue |
| `hblank_int_scx2_if_a.gb` | ❌ échoue |
| `hblank_int_scx2_if_b.gb` | ❌ échoue |
| `hblank_int_scx2_if_c.gb` | ❌ échoue |
| `hblank_int_scx2_nops_b.gb` | ❌ échoue |
| `hblank_int_scx3_if_a.gb` | ❌ échoue |
| `hblank_int_scx3_if_b.gb` | ❌ échoue |
| `hblank_int_scx3_if_c.gb` | ❌ échoue |
| `hblank_int_scx3_nops_b.gb` | ❌ échoue |
| `hblank_int_scx4_if_a.gb` | ❌ échoue |
| `hblank_int_scx4_if_b.gb` | ❌ échoue |
| `hblank_int_scx4_if_c.gb` | ❌ échoue |
| `hblank_int_scx4_nops_b.gb` | ❌ échoue |
| `hblank_int_scx5_if_a.gb` | ❌ échoue |
| `hblank_int_scx5_if_b.gb` | ❌ échoue |
| `hblank_int_scx5_if_c.gb` | ❌ échoue |
| `hblank_int_scx5_nops_b.gb` | ❌ échoue |
| `hblank_int_scx6_if_a.gb` | ❌ échoue |
| `hblank_int_scx6_if_b.gb` | ❌ échoue |
| `hblank_int_scx6_if_c.gb` | ❌ échoue |
| `hblank_int_scx6_nops_b.gb` | ❌ échoue |
| `hblank_int_scx7.gb` | ❌ échoue |
| `hblank_int_scx7_if_a.gb` | ❌ échoue |
| `hblank_int_scx7_if_b.gb` | ❌ échoue |
| `hblank_int_scx7_if_c.gb` | ❌ échoue |
| `hblank_int_scx7_nops_b.gb` | ❌ échoue |
| `hblank_scx3_if_a.gb` | ❌ échoue |
| `hblank_scx3_if_b.gb` | ❌ échoue |
| `hblank_scx3_if_c.gb` | ❌ échoue |
| `hblank_scx3_int_a.gb` | ❌ échoue |
| `int_hblank_halt_bug_b.gb` | ❌ échoue |
| `int_hblank_halt_scx3.gb` | ❌ échoue |
| `int_hblank_halt_scx7.gb` | ❌ échoue |
| `int_hblank_incs_scx1.gb` | ❌ échoue |
| `int_hblank_incs_scx5.gb` | ❌ échoue |
| `int_hblank_nops_scx1.gb` | ❌ échoue |
| `int_hblank_nops_scx5.gb` | ❌ échoue |
| `int_oam_incs.gb` | ❌ échoue |
| `int_oam_nops.gb` | ❌ échoue |
| `lcdon_to_if_oam_b.gb` | ❌ échoue |
| `lcdon_to_ly1_b.gb` | ❌ échoue |
| `lcdon_to_ly2_b.gb` | ❌ échoue |
| `lcdon_to_ly3_b.gb` | ❌ échoue |
| `lcdon_to_oam_int_l0.gb` | ❌ échoue |
| `lcdon_to_oam_int_l1.gb` | ❌ échoue |
| `lcdon_to_oam_int_l2.gb` | ❌ échoue |
| `lcdon_to_oam_unlock_a.gb` | ❌ échoue |
| `lcdon_to_stat1_d.gb` | ❌ échoue |
| `lcdon_to_stat2_a.gb` | ❌ échoue |
| `lcdon_to_stat3_a.gb` | ❌ échoue |
| `lcdon_write_timing.gb` | ❔ sans verdict |
| `line_144_oam_int_b.gb` | ❌ échoue |
| `line_144_oam_int_c.gb` | ❌ échoue |
| `line_144_oam_int_d.gb` | ❌ échoue |
| `line_153_ly_b.gb` | ❌ échoue |
| `line_153_lyc0_stat_timing_f.gb` | ❌ échoue |
| `line_153_lyc0_stat_timing_m.gb` | ❌ échoue |
| `line_153_lyc153_stat_timing_c.gb` | ❌ échoue |
| `line_153_lyc153_stat_timing_e.gb` | ❌ échoue |
| `line_153_lyc_c.gb` | ❌ échoue |
| `line_153_ly_c.gb` | ❌ échoue |
| `line_153_lyc_int_a.gb` | ❌ échoue |
| `line_153_ly_d.gb` | ❌ échoue |
| `line_153_ly_f.gb` | ❌ échoue |
| `line_65_ly.gb` | ❌ échoue |
| `lyc1_write_timing_d.gb` | ❌ échoue |
| `ly_while_lcd_off.gb` | ❔ sans verdict |
| `mbc1_ram_banks.gb` | ❔ sans verdict |
| `minimal.gb` | ❔ sans verdict |
| `mode2_stat_int_to_oam_unlock.gb` | ❔ sans verdict |
| `oam_int_halt_a.gb` | ❌ échoue |
| `oam_int_halt_b.gb` | ❌ échoue |
| `oam_int_if_edge_b.gb` | ❌ échoue |
| `oam_int_if_edge_d.gb` | ❌ échoue |
| `oam_int_if_level_d.gb` | ❌ échoue |
| `oam_int_inc_sled.gb` | ❌ échoue |
| `oam_int_nops_a.gb` | ❌ échoue |
| `oam_int_nops_b.gb` | ❌ échoue |
| `oam_read_l0_a.gb` | ❌ échoue |
| `oam_read_l1_a.gb` | ❌ échoue |
| `oam_read_l1_f.gb` | ❌ échoue |
| `oam_sprite_trashing.gb` | ❔ sans verdict |
| `oam_write_l0_a.gb` | ❌ échoue |
| `oam_write_l1_c.gb` | ❌ échoue |
| `poweron_dma_000.gb` | ❌ échoue |
| `poweron.gb` | ❔ sans verdict |
| `poweron_joy_000.gb` | ❌ échoue |
| `poweron_ly_119.gb` | ❌ échoue |
| `poweron_ly_233.gb` | ❌ échoue |
| `poweron_oam_000.gb` | ❌ échoue |
| `poweron_oam_005.gb` | ❌ échoue |
| `poweron_oam_069.gb` | ❌ échoue |
| `poweron_oam_119.gb` | ❌ échoue |
| `poweron_oam_183.gb` | ❌ échoue |
| `poweron_oam_233.gb` | ❌ échoue |
| `poweron_stat_000.gb` | ❌ échoue |
| `poweron_stat_005.gb` | ❌ échoue |
| `poweron_stat_006.gb` | ❌ échoue |
| `poweron_stat_026.gb` | ❌ échoue |
| `poweron_stat_069.gb` | ❌ échoue |
| `poweron_stat_119.gb` | ❌ échoue |
| `poweron_stat_120.gb` | ❌ échoue |
| `poweron_stat_140.gb` | ❌ échoue |
| `poweron_stat_183.gb` | ❌ échoue |
| `poweron_stat_234.gb` | ❌ échoue |
| `poweron_vram_025.gb` | ❌ échoue |
| `poweron_vram_069.gb` | ❌ échoue |
| `poweron_vram_139.gb` | ❌ échoue |
| `poweron_vram_183.gb` | ❌ échoue |
| `ppu_scx_vs_bgp.gb` | ❔ sans verdict |
| `ppu_sprite_testbench.gb` | ❔ sans verdict |
| `ppu_spritex_vs_scx.gb` | ❔ sans verdict |
| `ppu_win_vs_wx.gb` | ❔ sans verdict |
| `ppu_wx_early.gb` | ❔ sans verdict |
| `sprite_0_b.gb` | ❌ échoue |
| `stat_write_glitch_l0_a.gb` | ❌ échoue |
| `stat_write_glitch_l0_b.gb` | ❌ échoue |
| `stat_write_glitch_l143_b.gb` | ❌ échoue |
| `stat_write_glitch_l143_c.gb` | ❌ échoue |
| `stat_write_glitch_l143_d.gb` | ❌ échoue |
| `stat_write_glitch_l154_a.gb` | ❌ échoue |
| `stat_write_glitch_l154_b.gb` | ❌ échoue |
| `stat_write_glitch_l154_d.gb` | ❌ échoue |
| `stat_write_glitch_l1_b.gb` | ❌ échoue |
| `stat_write_glitch_l1_c.gb` | ❌ échoue |
| `temp.gb` | ❔ sans verdict |
| `timer_tima_phase_f.gb` | ❌ échoue |
| `timer_tima_reload_256k_c.gb` | ❌ échoue |
| `timer_tima_write_e.gb` | ❌ échoue |
| `timer_tma_write_a.gb` | ❌ échoue |
| `toggle_lcdc.gb` | ❔ sans verdict |
| `vram_read_l1_b.gb` | ❌ échoue |
| `wave_write_to_0xC003.gb` | ❔ sans verdict |

## CGB (option `GBL_CGB`, hôte et ESP32 seulement)

`GBL_CGB=1` ajoute le mode couleur ; la valeur par défaut (0) laisse le code
DMG et le firmware META inchangés (désassemblage de `gbl_core`, `gbl_ppu`,
`gbl_cpu`, `gbl_apu` comparé à des références, hôte et Thumb-1). Les chemins
assembleur sont coupés avec `GBL_CGB=1`.

Fait : banques VRAM/WRAM (FF4F, FF70), démarrage CGB selon `$143` bit 7,
KEY1 et double vitesse (`STOP` armé), palettes BCPS/BCPD/OCPS/OCPD, attributs
de tuile (palette, banque, retournements, priorité) et de sprite, priorité
par index d'OAM, rendu RGB565 pixel par pixel (`draw_line_cgb`).
Pas encore : HDMA/GDMA (FF51-FF55), registres de palette DMG-sur-CGB, accès
VRAM/palette bloqués en mode 3, correction de couleur.

Mesures (build deepeel `-DDP_GBL_CGB=1`, outil `gbshot`) :
`cgb-acid2` = image de référence, 0 pixel de différence ; `cgb-acid-hell` :
2 pixels de différence en (80, 68-69) ; Tetris DX (`roms/cgb/`) : copyright,
titre, menus, saisie de nom et début de partie en couleur, sans verdict
automatique ; mooneye et blargg : mêmes résultats avec et sans `GBL_CGB`.

## Reste
- Comparaison de captures (dmg-acid2, mealybug, gambatte, scribbltests) : demande un hash d'écran de référence par ROM.
- Les « sans verdict » de Blargg qui ne terminent pas (sons, `interrupt_time`, `halt_bug`) bloquent sur un comportement
  APU/timing à diagnostiquer ; les `emulator-only/mbc*` retombent sur `rst $38` (exécution hors programme) : bogue de
  mapper/taille à ouvrir côté m0boy.
- Reproduire : `cmake --build build --target gbconform` puis `build/tests/gbconform 8000 <rom>...` (deepeel) ;
  `GBC_END=1` affiche l'état final, `GBC_MICRO=1` active le protocole gbmicrotest.

- CGB, HDMA/GDMA (FF51-FF55) : GDMA immédiat, HDMA un bloc de 16 octets par HBlank (annulation par écriture bit 7 = 0, lecture de FF55). Implémenté, sans ROM de test dédiée : seule la non-régression est mesurée (mooneye 84, `cgb-acid2` 0 pixel, code DMG identique).

- Registres non connectés (FF03, FF08-FF0E, FF4C-FF7F en DMG) : lecture à 0xFF (mooneye `unused_hwio-GS` passe, 85 mooneye). Change le firmware DMG (gbl_core seulement) : META 0.10.62.

- Mode de référence hôte (`GBL_HOST_REF`) : l'événement VBlank du dispatcher est retardé de 2 cycles (`GBL_VBL_DELAY`, 1 et 2 passent, 3 échoue) : mooneye `di_timing-GS` et `halt_ime1_timing2-GS` passent (87/115). Firmware inchangé (identité DMG : 8× identique).

- Mode de référence : STAT garde le bit LY=LYC quand le LCD est éteint, et l'activation ne lève pas d'interruption STAT si la ligne était déjà haute (mooneye `stat_lyc_onoff` passe, 88/115). Firmware inchangé (8× identique).

- Mode de référence : la longueur du mode 3 due à SCX%8 s'arrondit à 4 points (1-4 → 4, 5-7 → 8) : mooneye `hblank_ly_scx_timing-GS` passe (89/115). Firmware inchangé (8× identique).
