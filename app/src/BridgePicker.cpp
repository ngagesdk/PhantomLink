/** @file BridgePicker.cpp
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "BridgePicker.h"
#include "GcConfig.h"
#include "MessageServiceSearcher.h"

CBridgePicker *CBridgePicker::NewL(MBridgePickerObserver &aObserver)
{
    CBridgePicker *self = new (ELeave) CBridgePicker(aObserver);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop();
    return self;
}

CBridgePicker::CBridgePicker(MBridgePickerObserver &aObserver)
    : CActive(EPriorityStandard), iObserver(aObserver)
{
    CActiveScheduler::Add(this);
}

void CBridgePicker::ConstructL()
{
    /* same service filter as GAMECOMMS.DLL: devices with a serial port */
    iSearcher = CMessageServiceSearcher::NewL();
}

CBridgePicker::~CBridgePicker()
{
    Cancel();
    delete iSearcher;
}

void CBridgePicker::StartL()
{
    if (IsActive()) {
        return;
    }
    iSearcher->SelectDeviceByDiscoveryL(iStatus);
    SetActive();
}

void CBridgePicker::RunL()
{
    TInt err = iStatus.Int();
    /* take the address before releasing the notifier: the release may
       clear the response, and a zero address gives KErrRfcommBadAddress
       (-6352) when a game connects */
    TBTDevAddr addr = iSearcher->BTDevAddr();
    /* release the notifier, otherwise the next selection in this session
       is refused */
    iSearcher->CancelSelection();
    if (err == KErrNone && addr == TBTDevAddr()) {
        err = KErrNotFound; /* nothing usable was selected */
    }
    if (err == KErrNone) {
        TGcConfig config;
        config.Load();
        config.SaveBridgeAddress(addr);
        TGcConfig check; /* did it reach the file? */
        check.Load();
        if (!check.iHasBridge) {
            err = KErrGeneral;
        }
    }
    iObserver.BridgePicked(err);
}

void CBridgePicker::DoCancel()
{
    iSearcher->CancelSelection();
}

TInt CBridgePicker::RunError(TInt aError)
{
    iObserver.BridgePicked(aError);
    return KErrNone;
}
