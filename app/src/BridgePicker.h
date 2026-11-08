/** @file BridgePicker.h
 *
 *  Picks the bridge with the standard Bluetooth device selection and
 *  saves its address in C:\System\Data\GameComms.ini, nothing else (no
 *  connection).  Needed for games that cannot show the system dialog
 *  themselves (Rifts): once a bridge is saved, GAMECOMMS.DLL connects to
 *  it without asking.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef BRIDGEPICKER_H
#define BRIDGEPICKER_H

#include <e32base.h>

class CMessageServiceSearcher;

class MBridgePickerObserver
{
  public:
    /** KErrNone: the address is saved; KErrCancel: the user cancelled. */
    virtual void BridgePicked(TInt aError) = 0;
};

class CBridgePicker : public CActive
{
  public:
    static CBridgePicker *NewL(MBridgePickerObserver &aObserver);
    ~CBridgePicker();

    /** Shows the device selection; the result goes to the observer. */
    void StartL();
    TBool IsBusy() const { return IsActive(); }

  private:
    CBridgePicker(MBridgePickerObserver &aObserver);
    void ConstructL();
    void RunL();
    void DoCancel();
    TInt RunError(TInt aError);

    MBridgePickerObserver &iObserver;
    CMessageServiceSearcher *iSearcher;
};

#endif /* BRIDGEPICKER_H */
