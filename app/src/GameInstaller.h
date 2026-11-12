/** @file GameInstaller.h
 *
 *  Copies the replacement GAMECOMMS.DLL to a game-specific directory, or
 *  C:\System\Libs by default.
 *  The application ships one full DLL in <app dir>\dlls\gcvN.dll (the base
 *  variant) and a patch per other variant in <app dir>\dlls\gcvN.pat (see
 *  dll/variants/gcpatch.py); the variant is rebuilt here before the copy.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GAMEINSTALLER_H
#define GAMEINSTALLER_H

#include <e32std.h>

class GameInstaller
{
  public:
    /** Returns KErrNone or a system-wide error code (for example KErrInUse
     *  while the game is running, KErrNotFound without memory card, KErrCorrupt for a damaged
     *  patch, KErrDiskFull when less than 128 KB would stay free on C:). */
    static TInt Install(TInt aVariant, const TDesC &aAppFullName);
    /** Deletes gamecomms.dll from the default target directory. */
    static TInt RemoveDefault();
};

#endif /* GAMEINSTALLER_H */
