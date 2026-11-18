# meta-compiler

Convertit les ROMs Game Boy (.gb/.gbc) en jeux Gamebuino META.
Voir l'en-tête de `compile.mjs` pour le fonctionnement détaillé.

## Version web (web.mjs)

    node tools/meta-compiler/web.mjs [port]     # défaut 8642, écoute 127.0.0.1

Une page locale : on glisse un `.gb`/`.gbc`, le serveur le convertit avec
le pipeline existant (`compile.mjs`, donc m0recomp + PIO — mêmes profils,
même cache d'icônes, mêmes décisions) et le navigateur reçoit le `.zip`
du répertoire de jeu complet, prêt à déposer sur la carte SD ou dans
l'émulateur (`tools/meta-emu-sdl`, version wasm comprise).

- Le `.bin` exige la cross-compilation ARM (m0recomp + pio) : la conversion
  tourne sur la machine locale, pas dans le navigateur ; rien ne sort de
  `127.0.0.1`.
- Un `.gb` par conversion ; les demandes sont traitées en série.
- Le zip reproduit le format de release : le répertoire du jeu à la
  racine, avec son entrée de dossier, `.gbrmeta.json` compris — prêt à
  déposer sur la carte SD, dans l'émulateur (`tools/meta-emu-sdl`,
  version wasm comprise) ou à partager tel quel.
- Prérequis : les mêmes que `compile.mjs` (m0recomp compilé dans
  `build/bin`, pio dans le PATH) — la page affiche un bandeau s'il en
  manque.
