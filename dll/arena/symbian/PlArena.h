/** @file PlArena.h
 *
 *  PLARENA.DLL: the Bluetooth link to the bridge shared by the three
 *  PhantomLink Arena plug-ins (SnapComm.dll, sc_lib.dll,
 *  HttpDllRequest.dll).  All of them run in the game's thread; the first
 *  one opens the shared instance, the last one to close it ends the
 *  connection.  The plug-ins import PLARENA.DLL, so its code stays
 *  loaded as long as any of them is.
 *
 *  The interface is a set of extern "C" functions so that the plug-ins
 *  do not depend on GCC 2.9 name mangling (ordinals in plarena.def).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef PLARENA_H
#define PLARENA_H

#include "pla_core.h"
#include <e32std.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Open the shared instance (created on first use).  Returns KErrNone.
 *  Every successful call needs one pla_close(). */
IMPORT_C int pla_open(void);
IMPORT_C void pla_close(void);
IMPORT_C void pla_listen(int aSvc, pla_listener aFn, pla_state_fn aState, void *aCtx);
/** Send a GCA message; see pla_send() in pla_core.h. */
IMPORT_C int pla_submit(gca_msg *aMsg);
/** Current PLA_ST_* state. */
IMPORT_C int pla_state(void);
/** printf style line in C:\System\Data\PlArena.log (%s %d %x %u only). */
IMPORT_C void pla_log(const char *aFmt, ...);
/** Hex dump of up to 64 bytes in the log. */
IMPORT_C void pla_log_hex(const char *aWhat, const void *aData, int aLen);

#ifdef __cplusplus
}
#endif

/** Plug-in helper: one log line per call of an unidentified virtual
 *  function, with the arguments in r1..r3 and the first stack word and
 *  the caller's return address (an address in arenabase.dll). */
#define PLA_TRACE(dll, slot, a1, a2, a3, a4)                                                                 \
    pla_log("%s slot %x(%x, %x, %x, %x) from %x", dll, slot, (unsigned)(size_t)(a1), (unsigned)(size_t)(a2), \
            (unsigned)(size_t)(a3), (unsigned)(size_t)(a4), (unsigned)(size_t)__builtin_return_address(0))

#endif /* PLARENA_H */
