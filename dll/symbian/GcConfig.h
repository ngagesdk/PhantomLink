/** @file GcConfig.h
 *
 *  Optional settings file.  Looked up in this order:
 *    C:\System\Data\GameComms.ini   (written by PhantomLink and the DLL)
 *    E:\GameComms.ini                 (read only; retail game cards are
 *                                     write protected, so changes always
 *                                     go to C: and take precedence)
 *
 *  [Config]
 *  DeviceName=Maxwell        ; name shown to other players (default: phone BT name)
 *  AutoJoin=0                ; 1 = clients join the first open session without asking
 *  Log=0                     ; 1 = write C:\System\Data\GameComms.log
 *  ReleaseBridge=1           ; 0 = keep the bridge connection between games
 *                            ;     (1: closed after Disconnect/EndSession, as
 *                            ;     games refuse to start while a link exists)
 *  FollowBtName=1            ; 0 = ignore renames of the phone's Bluetooth
 *                            ;     name made by the game itself
 *  [Bridge]
 *  Address=0002ee112233      ; Bluetooth address of the ESP32 bridge (saved automatically)
 *  Channel=0                 ; RFCOMM channel, 0 = SDP lookup
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCCONFIG_H
#define GCCONFIG_H

#include <bttypes.h>
#include <e32base.h>

class TGcConfig
{
  public:
    TGcConfig();
    /** Reads the settings file; missing file/keys keep the defaults. */
    void Load();
    /** Persists the bridge address after the first successful selection. */
    void SaveBridgeAddress(const TBTDevAddr &aAddr);
    /** Sets aKey in aSection ("[Config]", "[Bridge]") and writes the file
     *  to C: (WritePath), keeping all other lines.  An empty aValue removes
     *  the key.  Call Load() afterwards to refresh the members. */
    TInt SetValue(const TDesC8 &aSection, const TDesC8 &aKey, const TDesC8 &aValue);
    /** Removes aSection ("[Bridge]") with all of its keys and writes the
     *  file to C:.  KErrNone if the section did not exist. */
    TInt RemoveSection(const TDesC8 &aSection);
    /** The file every change is written to. */
    static const TDesC &WritePath();

  public:
    TBuf8<33> iDeviceName; /* UTF-8, empty = use Bluetooth name */
    TBool iAutoJoin;
    TBool iLog;
    TBool iReleaseBridge;
    TBool iFollowBtName;
    TBool iHasBridge;
    TBTDevAddr iBridge;
    TInt iChannel;
    TFileName iPath; /* file that was read */
};

#endif /* GCCONFIG_H */
