// libm0recomp: the whole m0recomp command line as a function.
//
// m0recomp_main(argc, argv) behaves exactly like the `m0recomp` executable run
// with the same arguments (`m0recomp ROM --system gg --target pc -o dir ...`) and
// returns its exit status; argv[0] is the program name.  The generators write to
// stdout/stderr and keep process-wide state: call it once per process, or in a
// forked child (as m0boy does).
#pragma once
int m0recomp_main(int argc, char* argv[]);
