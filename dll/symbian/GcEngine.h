/** @file GcEngine.h
 *
 *  Symbian side of the GameComms engine: owns the portable gcd_dev
 *  state machine, the Bluetooth link to the bridge, the timer and the
 *  asynchronous event delivery to the game.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCENGINE_H
#define GCENGINE_H

#include "GcConfig.h"
#include "GcPlatform.h"
#include "GcTransport.h"
#include <e32base.h>
#include <es_sock.h>
#include <f32file.h>

class CGcListControl;
class CEikAppUi;

/** Receives engine events (implemented by the CGameBTComms classes). */
class MGcEventSink
{
  public:
    virtual void HandleGcEventL(const gcd_event &aEvent) = 0;
};

class MGcListObserver
{
  public:
    virtual void ListItemSelected(TInt aIndex) = 0;
    virtual void ListCancelled() = 0;
};

/** Receives the answer to CGcEngine::RequestSessionList() (open sessions
 *  of this game, for the game's own host search). */
class MGcSessionListObserver
{
  public:
    virtual void SessionList(const gcd_device *aSessions, TInt aCount) = 0;
};

class CGcEngine : public CBase, public MGcTransportObserver, public MGcListObserver
{
  public:
    static CGcEngine *NewL(MGcEventSink &aSink, TUint8 aApi, TUint8 aVariant, TUint32 aGameUid);
    ~CGcEngine();

    gcd_dev &Dev() { return *iDev; }
    TBool IsRegistered() const { return iDev && iDev->registered; }
    gc_u32 Now();

    /** Bring up the bridge connection if necessary (cancels a pending
     *  release, see RequestRelease). */
    void EnsureTransport();
    /** Close the bridge connection once nothing needs it any more
     *  (after Disconnect / EndSession).  Many 2.x games refuse to start
     *  a multiplayer game while any Bluetooth link exists ("Bluetooth
     *  busy"); with the original library there was none between games. */
    void RequestRelease();

    /** The game may rename the phone's Bluetooth device itself to tag it
     *  for its host search (RHostResolver::SetHostName, e.g. Rifts,
     *  SSX).  Picks up such a rename; called by the API functions after
     *  which the name matters. */
    void RefreshName();
    /** SetLocalDeviceName(). */
    void SetLocalName(const TDesC &aName);
    /** Writes "<aWhat>: <name other players see>" to the log. */
    void LogName(const TDesC8 &aWhat);
    /** Run the engine after an API call (timers, event delivery). */
    void Kick();

    /** Built-in UI (host selection / confirm list). */
    void SetAppUi(CEikAppUi *aAppUi) { iAppUi = aAppUi; }
    TBool IsShowingDialog() const;

    const TGcConfig &Config() const { return iConfig; }

    /** The engine of the current thread (the DLL's thread-local storage),
     *  NULL if no GameComms object has started yet.  Used by the helper
     *  classes, which the game creates independently. */
    static CGcEngine *Current();

    /** Ask the server once for the open sessions of this game; the answer
     *  goes to aObserver (one request at a time). */
    void RequestSessionList(MGcSessionListObserver *aObserver);
    void CancelSessionList(MGcSessionListObserver *aObserver);
    void Log(const TDesC8 &aText);
    void LogNum(const TDesC8 &aText, TInt aValue);

    /* MGcTransportObserver */
    void TransportUp();
    void TransportData(const TDesC8 &aData);
    void TransportDown(TInt aError);
    void TransportSelecting(TBool aShowing);
    void TransportProgress(const TDesC8 &aStep, TInt aValue);

    /* MGcListObserver */
    void ListItemSelected(TInt aIndex);
    void ListCancelled();

  private:
    CGcEngine(MGcEventSink &aSink);
    void ConstructL(TUint8 aApi, TUint8 aVariant, TUint32 aGameUid);
    static TInt TimerCallBack(TAny *aSelf);
    static TInt DispatchCallBack(TAny *aSelf);
    static TInt RetryCallBack(TAny *aSelf);
    static TInt ReleaseCallBack(TAny *aSelf);
    void DoRelease();
    void Tick();
    void Dispatch();
    void ShowListL(const gcd_event &aEvent, TBool aHostConfirm);
    void CloseList();
    void RefreshListL();

    /* gcd_ops trampolines */
    static void OpWrite(void *aCtx, const gc_u8 *aData, size_t aLen);
    static void OpLog(void *aCtx, const char *aLine);
    static void OpWake(void *aCtx);

    MGcEventSink &iSink;
    TGcConfig iConfig;
    gcd_dev *iDev;
    CGcTransport *iTransport;
    CPeriodic *iTimer;
    CPeriodic *iRetry;
    CPeriodic *iRelease;
    TInt iReleaseStage;
    THostName iBtName; /* Bluetooth name as last seen */
    TBool iBtNameKnown;
    TInt iNameTicks;
    CAsyncCallBack *iDispatcher;
    CGcListControl *iList;
    CGcListControl *iDoomedList;
    TBool iListIsHostConfirm;
    gcd_device iListItems[GCD_MAX_SESSIONS];
    TInt iListCount;
    CEikAppUi *iAppUi;
    TBool iSelectingBridge;
    TBool iWantTransport;
    TInt iTickPeriodUs;
    TUint iLastTick;
    gc_u32 iMs;
    RFs iFs;
    RFile iLogFile;
    TBool iLogOpen;
    TInt iLogSize; /* bytes in the log file, kept below KGcLogMax */
    TBool iDispatching;
    gcd_event *iEvent; /* scratch, heap: events are ~1.1 KB */
    /* iDev and iEvent live in a heap of our own: the game's heap is only
     * 1 MB and some games (Warhammer) fill it almost completely */
    RHeap *iHeap;
    TBool iOwnHeap;
    MGcSessionListObserver *iListObserver;
};

#endif /* GCENGINE_H */
