# aka_headless (tools/AKA_Emulator) — problèmes rencontrés

Remontée d'octobre 2026, m0boy (conversions Game Boy / Mega Drive / PC Engine
vers Gamebuino AKA). Version testée : l'AKA_Emulator.exe / aka_headless du
dépôt outils d'octobre 2026. Ce qui suit a été observé en débuguant l'écran
noir d'un firmware PC Engine, avec `--watch`, `--itrace` et des builds
instrumentés (printf UART).

## 1. Les transferts DMA asynchrones ne se terminent jamais

`esp_lcd_panel_io_tx_color()` en file (le chemin « wide » : `trans_queue_depth
> 0`, plusieurs transferts en vol, fenêtre 0x2A/0x2B posée une fois par frame
puis 0x2C + 0x3C) ne produit jamais de callback `on_color_trans_done` :

- Mega Drive (chemin wide d'origine, validé sur carte réelle) : `display: on,
  0 frames written` en 6 s émulées, la tâche qui attend `color_done` ne se
  réveille jamais.
- PC Engine (même chemin) : 240 paquets (le remplissage initial) puis
  immobile ; l'émulation file ensuite à « 4.24x real time » car plus rien
  n'attend le LCD.

Le chemin **synchrone** (un `tx_color`, attente du sémaphore, transfert
suivant — le chemin de `m0_lcd_send_rows`) fonctionne parfaitement, callback
compris. Les paquets 0x3C (continuation de fenêtre) semblent aussi concernés :
une variante « une seule requête en vol » avec 0x2C puis des 0x3C s'arrête
après ~30 paquets, alors que la même chose avec une fenêtre + 0x2C par ligne
coule normalement.

Conséquence m0boy : le firmware livré garde le chemin asynchrone (le vrai
comportement de la carte) ; un define de build `AKA_EMU_SYNC=1` bascule le
frontend PCE sur le chemin synchrone pour les passes headless.

## 2. Les tâches épinglées au cœur 0 ne vivent pas

`xTaskCreatePinnedToCore(t, ..., 0)` : la tâche démarre (elle exécute ses
premières itérations) mais un réveil par sémaphore donné depuis une ISR du
cœur 1 ne la sort jamais de `xSemaphoreTake` : la tâche de rendu du Mega
Drive ne produit rien du tout ; celle du PC Engine a dessiné ~4 batches de 8
lignes en 15 s (quelques réveils arrivés par d'autres événements) puis plus
rien. Le blocage final montre les deux cœurs dans le même spinlock ESP-IDF
(pc 0x4037c2xx, `memw`, scan de la table par cœur), INTLEVEL élevé des deux
côtés.

## 3. Le temps émulé avance quand les tâches sont bloquées

`emulated 15.00 s in 3.54 s: 4.24x real time` signifiait ici « la boucle
principale est bloquée sur un sémaphore » (esp_timer est un périphérique, il
tourne quoi qu'il arrive), pas « le firmware va 4x plus vite ». À prendre en
compte avant de lire les x real time comme des performances.

## 4. --watch : une seule adresse par run

Deux `--watch A --watch B` sur la même ligne : seule la dernière adresse est
surveillée (11,2 M lignes pour B, zéro pour A). Il faut relancer un run par
adresse et corréler à la main. (`--watch` n'accepte que des adresses hex —
les noms de symboles du .sym ne sont pas résolus, ce qui est documenté mais
vaut le rappel.)

## 5. Divers

- Le champ `LCD_CAM user` de la ligne finale reflète le chemin LCD choisi
  (0x65082000 pour le synchrone, 0x65002000 pour le wide) — utile pour
  savoir quelle variante tourne.
- `--itrace` + `--watch` + les adresses reconstruites par probes `offsetof`
  (compilées avec le GCC Xtensa du SDK) suffisent à débugger tout le chemin
  synchrone : l'écran noir d'origine (un `machine.row` jamais posé dans un
  projet généré périmé) a été trouvé ainsi.
- Les timings mesurés dans l'émulateur (pce_frame ~54 ms etc.) restent des
  estimations : la vraie carte seule tranche.

## Ce qui aiderait (liste de souhaits)

1. Compléter les transferts DMA en file (callback + réveil de la tâche
   en attente), et les paquets 0x3C.
2. Réveiller les tâches du cœur 0 depuis les ISR du cœur 1 (cross-core INT).
3. Plusieurs `--watch` par run.
