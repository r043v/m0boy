#ifndef M0BOY_GP32_BUNDLE_H
#define M0BOY_GP32_BUNDLE_H

#include "gp32_host.h"

/* Unpack the application embedded by bundle.py into a save directory next to
   the executable (the user's preference directory if that one is read only) and
   point `host` at it. An existing card is never overwritten: it holds the saves. */
bool gp32_bundle_install(Gp32Host *host);

#endif
