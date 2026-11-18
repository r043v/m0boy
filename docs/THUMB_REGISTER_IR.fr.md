# Génération Thumb à partir d'une représentation des registres

Le backend expérimental `--meta-thumb` fonctionne pour les cibles META et
Pokitto. Il optimise des suites de chargements de registres Game Boy avant
leur traduction en Thumb-1. Il reste désactivé par défaut : aucun gain global
sur un jeu commercial ou une console réelle n'a encore été mesuré.

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
./build/bin/m0recomp jeu.gb --target meta --coverage jeu.cov --meta-thumb -o output/jeu
# Même option avec --target pokitto.
```

La couverture reste nécessaire. Sans couverture, le projet fonctionne avec
l'interpréteur et ne contient aucun bloc à optimiser. La sortie du générateur
indique le nombre de blocs simplifiés et ceux ayant une traduction Thumb.

## Conversion

`RegisterIR` représente chacun des sept registres octet par sa valeur à
l'entrée du bloc ou par une constante. Un `LD` copie cette valeur symbolique ;
il ne produit pas immédiatement une instruction cible. Une nouvelle écriture
remplace la précédente. La représentation conserve ainsi les dépendances
sans table de combinaisons d'opcodes.

La première version admet `NOP`, `LD r,r`, `LD r,n` (sans `(HL)`) et
`LD BC/DE/HL,nn`. Elle ne modifie aucun flag Game Boy. Les autres opérations
restent produites par le générateur C existant ; il ne s'agit pas encore d'un
backend assembleur pour toute la CPU.

À la sortie, les copies de paires complètes deviennent des chargements et
stockages de demi-mots. Les constantes des deux octets sont regroupées. Les
valeurs d'entrée nécessaires sont toutes chargées **avant** les écritures,
ce qui conserve les anciennes valeurs lors des copies croisées.

Exemple :

```asm
; Game Boy
LD B,0
LD C,0
LD B,0

; Corps Thumb, avec un registre contenant déjà &gbr
movs r3,#0
strh r3,[base,#0]       ; BC = 0
```

La modification de BC demande ici deux instructions ARM, dont une seule
pour produire la valeur zéro. La comptabilité des 24 cycles Game Boy, le
chargement de l'adresse de l'état et les contrôles d'événements restent à
prendre en compte. « Trois instructions deviennent une » n'est donc pas une
mesure du coût total du bloc.

Le code natif est intégré au C généré par un bloc d'assembleur GNU avec les
contraintes de registres bas, les clobbers réellement utilisés et une barrière
mémoire. Les offsets de la structure CPU sont vérifiés à la compilation.
Les blocs qui nécessitent plus de trois sources distinctes utilisent la
traduction C symbolique, pour éviter une pression excessive sur les registres
Thumb-1. Sur l'hôte, la même traduction C permet les tests différentiels.

## Limites d'observation

Une fusion ne traverse ni label/point d'entrée, ni trou dans la couverture,
ni accès mémoire, ni opération arithmétique, ni changement de contrôle.
Les boucles accélérées et l'instruction suivant EI conservent leur chemin
habituel. Le mode `--meta-precise` désactive la fusion et garde les points de
synchronisation par instruction.

Le préfixe de chaque instruction du générateur existant reste exécuté lors
de la génération : ses cycles sont accumulés même si son écriture disparaît.
Les transferts et les sorties de région conservent leur traitement original.
Le mode rapide garde sa politique existante de synchronisation par régions ;
ce changement ne lui attribue pas la précision du mode `--meta-precise`.

## Validation

`meta_register_ir_semantics` compare les valeurs symboliques à l'exécution
séquentielle de 20 000 suites aléatoires et vérifie les frontières des opcodes
acceptés. `meta_register_ir_native` compile 256 fonctions avec CMake/Ninja et
le compilateur ARM, puis compare sous Unicorn leur état CPU complet avec une
version C séquentielle, pour 32 états initiaux et trois niveaux d'optimisation
(`-O0`, `-O2`, `-Os`). Ce test est explicitement marqué ignoré si ses dépendances
optionnelles (`arm-none-eabi-gcc`, Unicorn, pyelftools) sont absentes.

Le test de bout en bout synthétique compare également les variantes
`--meta-thumb` et C, en mode rapide et précis, avec flags, mémoire, banques,
interruptions et sorties vers l'interpréteur. Les performances du runtime
complet doivent être évaluées séparément avec `m0_estimate.py` et sur console.

Les extensions suivantes pourront enrichir cette représentation avec les
opérations arithmétiques, la durée de vie des flags et l'allocation des
registres entre plusieurs opérations. Elles devront conserver les mêmes
frontières d'observation et être validées avant une activation par défaut.

## Résultat de la première comparaison complète

Le 11 octobre 2026, avec le compilateur ARM GCC 14.2.1, le programme
synthétique du test de bout en bout (graine 1989, couverture statique) a été
exécuté pendant huit trames dans l'estimateur Cortex-M0+. Les deux builds
portaient `BENCH_PIXEL_HASH` et `BENCH_STATE_HASH` ; le coût de ces instruments
est inclus, donc ces chiffres ne représentent pas les performances d'un jeu.

| Variante | Hash pixels | Hash état | `.text` | Cycles estimés/trame |
| --- | --- | --- | ---: | ---: |
| C existant | `ade26958` | `e5523b5b` | 127 652 octets | 927 984 |
| `--meta-thumb` | `ade26958` | `e5523b5b` | 127 816 octets | 928 015 |

Le générateur a simplifié 48 blocs, dont 36 avec émission Thumb. Les images
et états sont identiques. Cette première comparaison ne montre **aucun gain
global** : le coût est pratiquement identique, avec 164 octets supplémentaires
de code. GCC optimise déjà beaucoup de ces chargements en C. L'intérêt de
cette version est une base validée pour étendre l'IR et conserver des valeurs
en registres à travers des opérations arithmétiques ; elle ne justifie pas
une activation par défaut.

Les treize tests CTest ont été validés pendant cette implémentation, après
correction de la syntaxe assembleur : les tests ARM ont été relancés avec
succès. Le test de bout en bout a comparé 260 trames ; le test Pokitto a
également lié le firmware avec `--meta-thumb` et vérifié ses limites de
mémoire et son format de démarrage. Aucun essai sur console physique n’a
été effectué.
