/** @file ArenaSetup.h
 *
 *  N-Gage Arena over Bluetooth (retail cartridges, docs/ARENA.md):
 *
 *   - turns the user created access point "PhantomLink" (Settings >
 *     Connection > Access points, data bearer GSM data, any number) into
 *     a direct PPP connection over the Bluetooth serial port BTCOMM::0,
 *     the way gnubox does on Symbian 6.1: empty phone number (no dialling,
 *     PPP starts at once), no login script, and the data modem switched
 *     to BTCOMM.  The previous modem settings are saved and can be
 *     restored, because on 6.1 the modem record is shared by all data
 *     call access points.
 *   - writes C:\System\Data\hosts so that the Arena server names resolve
 *     to the PhantomLink bridge (Symbian 6.1-9.0 read that file).
 *
 *  BTCOMM::0's remote device is set to the bridge through CBTRegistry::
 *  SetDefaultCommPort (btregistry.lib, part of the 6.1 SDK).  Not verified
 *  on hardware yet.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef ARENASETUP_H
#define ARENASETUP_H

#include <bttypes.h>
#include <e32base.h>

class ArenaSetup
{
  public:
    /** Configure the access point; appends a report to aLog.  Leaves with
     *  KErrNotFound if there is no access point called "PhantomLink". */
    static void EnableL(const TBTDevAddr &aBridge, TDes &aLog);
    /** Restore the saved modem settings and remove the hosts entries. */
    static void DisableL(TDes &aLog);
    /** Write the Arena host names to C:\System\Data\hosts, mapped to
     *  aAddr (dotted IPv4).  Other lines of the file are kept. */
    static void WriteHostsL(const TDesC8 &aAddr, TDes &aLog);
    static void RemoveHostsL(TDes &aLog);
};

#endif /* ARENASETUP_H */
