/** @file GcEngine.cpp
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "GcEngine.h"
#include "GcListControl.h"
#include <badesca.h>
#include <coemain.h>
#include <e32hal.h>
#include <e32svr.h>
#include <eikappui.h>
#include <eikenv.h>

/* euser.lib of the 6.1 SDK lacks Dll::Tls; use the underlying UserSvr calls, keyed by the DLL's UID3 */
static const TInt KGcTlsHandle = 0x101f5ee2;

_LIT(KGcLogFile, "C:\\System\\Data\\GameComms.log"); /* game cards are read only */
/* C: is small (games quit when it is full): the log starts over beyond this */
const TInt KGcLogMax = 32 * 1024;
/* private heap for the engine state (gcd_dev ~64 KB + one event) */
const TInt KGcHeapMin = 0x1000;
const TInt KGcHeapMax = 0x40000;
_LIT(KGcSelectHost, "Select game");
_LIT(KGcConfirmPlayers, "Players");

const TInt KGcTimerUs = 20000;    /* 50 Hz engine tick while active */
const TInt KGcRetryUs = 3000000;  /* bridge reconnect interval      */
const TInt KGcReleaseUs = 750000; /* release: leave, then close     */
const TInt KGcNameTicks = 100;    /* re-read the BT name every 2 s while waiting */

/* Every live engine of the thread, newest last.  A game may create a
 * short-lived GameComms object next to its main one (Pathway to Glory
 * does, for the Bluetooth power state); deleting it must not make the
 * helper classes lose the main engine. */
const TInt KGcMaxEngines = 8;
struct TGcEngineList
{
    TInt iCount;
    CGcEngine *iItems[KGcMaxEngines];
};

static void RegisterEngine(CGcEngine *aEngine)
{
    TGcEngineList *list = (TGcEngineList *)UserSvr::DllTls(KGcTlsHandle);
    if (!list) {
        list = (TGcEngineList *)User::Alloc(sizeof(TGcEngineList));
        if (!list) {
            return;
        }
        list->iCount = 0;
        if (UserSvr::DllSetTls(KGcTlsHandle, list) != KErrNone) {
            User::Free(list);
            return;
        }
    }
    if (list->iCount < KGcMaxEngines) {
        list->iItems[list->iCount++] = aEngine;
    }
}

static void UnregisterEngine(CGcEngine *aEngine)
{
    TGcEngineList *list = (TGcEngineList *)UserSvr::DllTls(KGcTlsHandle);
    TInt i;
    if (!list) {
        return;
    }
    for (i = 0; i < list->iCount; i++) {
        if (list->iItems[i] == aEngine) {
            TInt k;
            for (k = i; k + 1 < list->iCount; k++) {
                list->iItems[k] = list->iItems[k + 1];
            }
            list->iCount--;
            break;
        }
    }
    if (list->iCount == 0) {
        UserSvr::DllSetTls(KGcTlsHandle, NULL);
        User::Free(list);
    }
}

/* the name as the protocol carries it: up to 32 bytes, non-ASCII as '?'
 * (same mapping as SetLocalDeviceName always used) */
static void NameFromDes(const TDesC &aName, char *aOut)
{
    TInt i;
    TPtrC n = aName.Left(GC_MAX_NAME);
    for (i = 0; i < n.Length(); i++) {
        aOut[i] = (char)(n[i] < 0x80 ? n[i] : '?');
    }
    aOut[i] = '\0';
}

/* the phone's current Bluetooth device name */
static TInt ReadBluetoothName(THostName &aName)
{
    RSocketServ ss;
    TInt err = ss.Connect();
    if (err != KErrNone) {
        return err;
    }
    _LIT(KBTLinkManager, "BTLinkManager");
    TProtocolDesc pd;
    err = ss.FindProtocol(KBTLinkManager(), pd);
    if (err == KErrNone) {
        RHostResolver hr;
        err = hr.Open(ss, pd.iAddrFamily, pd.iProtocol);
        if (err == KErrNone) {
            err = hr.GetHostName(aName);
            hr.Close();
        }
    }
    ss.Close();
    return err;
}

CGcEngine *CGcEngine::NewL(MGcEventSink &aSink, TUint8 aApi, TUint8 aVariant, TUint32 aGameUid)
{
    CGcEngine *self = new (ELeave) CGcEngine(aSink);
    CleanupStack::PushL(self);
    self->ConstructL(aApi, aVariant, aGameUid);
    CleanupStack::Pop();
    return self;
}

CGcEngine::CGcEngine(MGcEventSink &aSink) : iSink(aSink)
{
}

static void GetBluetoothNameL(TDes8 &aName)
{
    /* the local Bluetooth name is the natural "DID" of a device */
    RSocketServ ss;
    User::LeaveIfError(ss.Connect());
    CleanupClosePushL(ss);
    RHostResolver hr;
    _LIT(KBTLinkManager, "BTLinkManager");
    TProtocolDesc pd;
    User::LeaveIfError(ss.FindProtocol(KBTLinkManager(), pd));
    User::LeaveIfError(hr.Open(ss, pd.iAddrFamily, pd.iProtocol));
    CleanupClosePushL(hr);
    THostName name;
    User::LeaveIfError(hr.GetHostName(name));
    aName.Copy(name.Left(32));
    CleanupStack::PopAndDestroy(2);
}

void CGcEngine::ConstructL(TUint8 aApi, TUint8 aVariant, TUint32 aGameUid)
{
    iConfig.Load();
    if (iConfig.iLog && iFs.Connect() == KErrNone) {
        TParsePtrC logPath(KGcLogFile);
        iFs.MkDirAll(logPath.DriveAndPath()); /* KErrAlreadyExists is fine */
        /* append, so that several GameComms objects of one game share the log */
        TInt err = iLogFile.Open(iFs, KGcLogFile, EFileWrite | EFileShareAny);
        if (err == KErrNotFound) {
            err = iLogFile.Create(iFs, KGcLogFile, EFileWrite | EFileShareAny);
        }
        if (err == KErrNone) {
            TInt size = 0;
            iLogFile.Size(size);
            if (size >= KGcLogMax) {
                iLogFile.SetSize(0); /* start over */
                size = 0;
            }
            TInt pos = 0;
            iLogFile.Seek(ESeekEnd, pos);
            iLogSize = size;
            iLogOpen = ETrue;
        }
    }

    gcd_config cfg;
    gcd_ops ops;
    gcd_default_config(&cfg);
    cfg.api = aApi;
    cfg.variant = aVariant;
    cfg.game_uid = aGameUid;
    cfg.auto_join = iConfig.iAutoJoin ? 1 : 0;
    TBuf8<33> name = iConfig.iDeviceName;
    if (name.Length() == 0) {
        TRAPD(err, GetBluetoothNameL(name));
        if (err != KErrNone || name.Length() == 0) {
            name = _L8("N-Gage");
        }
    }
    TInt i;
    for (i = 0; i < name.Length() && i < GC_MAX_NAME; i++) {
        cfg.name[i] = (char)name[i];
    }
    cfg.name[i] = '\0';
    TTime t;
    t.UniversalTime();
    cfg.link_id = t.Int64().Low() ^ (TUint32)this ^ 0x5A5A0000;
    if (cfg.link_id == 0) {
        cfg.link_id = 1;
    }
    ops.write = OpWrite;
    ops.log = OpLog;
    ops.wake = OpWake;

    /* gcd_dev is ~64 KB.  Apps get a 1 MB heap and Warhammer fills almost
     * all of it at start-up (its game object alone is ~460 KB): taking our
     * state from there made it quit with "insufficient memory".  The
     * original GameComms needs only a few KB, so our state goes into a
     * local chunk of its own; the game's heap is used only as fallback. */
    iHeap = UserHeap::ChunkHeap(NULL, KGcHeapMin, KGcHeapMax);
    iOwnHeap = iHeap != NULL;
    if (!iOwnHeap) {
        iHeap = &User::Heap();
    }
    iDev = (gcd_dev *)iHeap->Alloc(sizeof(gcd_dev));
    iEvent = (gcd_event *)iHeap->Alloc(sizeof(gcd_event));
    if (!iDev || !iEvent) {
        User::Leave(KErrNoMemory);
    }
    Mem::FillZ(iDev, sizeof(gcd_dev));
    Mem::FillZ(iEvent, sizeof(gcd_event));
    gcd_init(iDev, &cfg, &ops, this);

    iTransport = CGcTransport::NewL(*this, iConfig);
    iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
    iRetry = CPeriodic::NewL(CActive::EPriorityIdle);
    iRelease = CPeriodic::NewL(CActive::EPriorityIdle);
    iBtNameKnown = ReadBluetoothName(iBtName) == KErrNone && iBtName.Length() > 0;
    TCallBack dispatch(DispatchCallBack, this);
    iDispatcher = new (ELeave) CAsyncCallBack(dispatch, CActive::EPriorityStandard);

    TTimeIntervalMicroSeconds32 period;
    UserHal::TickPeriod(period);
    iTickPeriodUs = period.Int();
    iLastTick = User::TickCount();
    iTimer->Start(KGcTimerUs, KGcTimerUs, TCallBack(TimerCallBack, this));
    RegisterEngine(this); /* for the helper classes (no writable static data in DLLs) */

    TBuf8<96> line;
    line.Copy(_L8("GameComms replacement, api "));
    line.AppendNum((TInt)aApi);
    line.Append(_L8(" variant "));
    line.AppendNum((TInt)aVariant);
    line.Append(_L8(" game uid "));
    line.AppendNum(aGameUid, EHex);
    line.Append(_L8(" bridge saved "));
    line.AppendNum(iConfig.iHasBridge ? 1 : 0);
    Log(line);
}

CGcEngine::~CGcEngine()
{
    UnregisterEngine(this);
    delete iRelease;
    iRelease = NULL;
    if (iDev) {
        gcd_end_session(iDev, Now());
        gc_link_close(&iDev->link, GC_LR_LOCAL);
    }
    CloseList();
    delete iDoomedList;
    delete iDispatcher;
    delete iRetry;
    delete iTimer;
    delete iTransport;
    if (iHeap) {
        iHeap->Free(iEvent);
        iHeap->Free(iDev);
        if (iOwnHeap) {
            iHeap->Close(); /* frees the chunk */
        }
    }
    iEvent = NULL;
    iDev = NULL;
    if (iLogOpen) {
        iLogFile.Close();
    }
    iFs.Close();
}

gc_u32 CGcEngine::Now()
{
    TUint tick = User::TickCount();
    iMs += (gc_u32)((tick - iLastTick) * (TUint)iTickPeriodUs / 1000);
    iLastTick = tick;
    return iMs;
}

void CGcEngine::Log(const TDesC8 &aText)
{
    if (!iLogOpen) {
        return;
    }
    if (iLogSize + aText.Length() + 18 > KGcLogMax) {
        /* keep the newest lines: start the file over */
        TInt pos = 0;
        iLogFile.SetSize(0);
        iLogFile.Seek(ESeekStart, pos);
        iLogSize = 0;
    }
    TBuf8<16> ts;
    ts.Num(iMs);
    ts.Append(' ');
    if (iLogFile.Write(ts) != KErrNone) {
        iLogOpen = EFalse; /* e.g. C: full: stop logging, do not fill it up */
        return;
    }
    iLogFile.Write(aText);
    iLogFile.Write(_L8("\r\n"));
    iLogSize += ts.Length() + aText.Length() + 2;
}

void CGcEngine::LogNum(const TDesC8 &aText, TInt aValue)
{
    TBuf8<96> t;
    t.Copy(aText.Left(80));
    t.Append(' ');
    t.AppendNum(aValue);
    Log(t);
}

void CGcEngine::EnsureTransport()
{
    if (iRelease) {
        iRelease->Cancel();
    }
    if (iReleaseStage) {
        gcd_resume(iDev, Now()); /* left the server, but kept the link */
    }
    iReleaseStage = 0;
    iWantTransport = ETrue;
    LogNum(_L8("bridge connection wanted, busy"), iTransport->IsBusy() ? 1 : 0);
    if (!iTransport->IsBusy()) {
        TRAPD(err, iTransport->ConnectL());
        if (err != KErrNone) {
            TransportDown(err);
        }
    }
}

void CGcEngine::Kick()
{
    gcd_poll(iDev, Now());
    if (iDev->ev_count && !iDispatcher->IsActive()) {
        iDispatcher->CallBack();
    }
}

CGcEngine *CGcEngine::Current()
{
    TGcEngineList *list = (TGcEngineList *)UserSvr::DllTls(KGcTlsHandle);
    return list && list->iCount > 0 ? list->iItems[list->iCount - 1] : NULL;
}

/* ---- local name ---- */

void CGcEngine::RefreshName()
{
    if (!iConfig.iFollowBtName) {
        return;
    }
    THostName live;
    if (ReadBluetoothName(live) != KErrNone || live.Length() == 0) {
        return;
    }
    if (iBtNameKnown && live == iBtName) {
        return; /* not renamed: keep DeviceName= / SetLocalDeviceName */
    }
    TBool first = !iBtNameKnown;
    iBtName = live;
    iBtNameKnown = ETrue;
    if (first) {
        return; /* nothing to compare with */
    }
    char name[GC_MAX_NAME + 1];
    NameFromDes(live, name);
    if (gcd_set_name(iDev, name, Now())) {
        TBuf8<64> t;
        t.Copy(_L8("game renamed the phone: "));
        t.Append(TPtrC8((const TUint8 *)name, User::StringLength((const TUint8 *)name)));
        Log(t);
        Kick();
    }
}

void CGcEngine::SetLocalName(const TDesC &aName)
{
    char name[GC_MAX_NAME + 1];
    NameFromDes(aName, name);
    if (gcd_set_name(iDev, name, Now())) {
        Log(_L8("local name set by the game"));
        Kick();
    }
}

/* ---- bridge link release ---- */

void CGcEngine::RequestRelease()
{
    if (!iConfig.iReleaseBridge || !iWantTransport) {
        return;
    }
    iRelease->Cancel();
    iReleaseStage = 0;
    iRelease->Start(KGcReleaseUs, KGcReleaseUs, TCallBack(ReleaseCallBack, this));
}

TInt CGcEngine::ReleaseCallBack(TAny *aSelf)
{
    ((CGcEngine *)aSelf)->DoRelease();
    return 0;
}

void CGcEngine::DoRelease()
{
    if (!gcd_is_quiet(iDev) || iList || iSelectingBridge || iListObserver) {
        iRelease->Cancel(); /* in use again */
        if (iReleaseStage) {
            gcd_resume(iDev, Now());
        }
        iReleaseStage = 0;
        return;
    }
    if (iReleaseStage == 0) {
        /* first leave the server session while the link is still up */
        gcd_release(iDev, Now());
        Kick();
        iReleaseStage = 1;
        return;
    }
    iRelease->Cancel();
    iReleaseStage = 0;
    Log(_L8("bridge link released, nothing needs it"));
    iWantTransport = EFalse;
    iRetry->Cancel();
    iTransport->Disconnect();
    gcd_transport_down(iDev, Now());
}

void CGcEngine::RequestSessionList(MGcSessionListObserver *aObserver)
{
    iListObserver = aObserver;
    Log(_L8("game searches for hosts"));
    EnsureTransport();
    gcd_discover_once(iDev, Now());
    Kick();
}

void CGcEngine::CancelSessionList(MGcSessionListObserver *aObserver)
{
    if (iListObserver == aObserver) {
        iListObserver = NULL;
    }
}

TBool CGcEngine::IsShowingDialog() const
{
    return iList != NULL || iSelectingBridge;
}

/* ---- gcd_ops ---- */

void CGcEngine::OpWrite(void *aCtx, const gc_u8 *aData, size_t aLen)
{
    CGcEngine *self = (CGcEngine *)aCtx;
    if (self->iTransport->Write(TPtrC8(aData, (TInt)aLen)) != KErrNone) {
        self->Log(_L8("write buffer overflow (link layer will retransmit)"));
    }
}

void CGcEngine::OpLog(void *aCtx, const char *aLine)
{
    CGcEngine *self = (CGcEngine *)aCtx;
    self->Log(TPtrC8((const TUint8 *)aLine, User::StringLength((const TUint8 *)aLine)));
}

void CGcEngine::OpWake(void *aCtx)
{
    CGcEngine *self = (CGcEngine *)aCtx;
    if (!self->iDispatcher->IsActive()) {
        self->iDispatcher->CallBack();
    }
}

/* ---- timers ---- */

TInt CGcEngine::TimerCallBack(TAny *aSelf)
{
    ((CGcEngine *)aSelf)->Tick();
    return 1;
}

TInt CGcEngine::RetryCallBack(TAny *aSelf)
{
    CGcEngine *self = (CGcEngine *)aSelf;
    self->iRetry->Cancel();
    if (self->iWantTransport) {
        self->EnsureTransport();
    }
    return 0;
}

void CGcEngine::Tick()
{
    if (iDoomedList) {
        delete iDoomedList;
        iDoomedList = NULL;
    }
    gcd_poll(iDev, Now());
    /* A game may rename the phone a moment before or after StartHostL /
     * StartClientL, and the Bluetooth stack may report the new name late.
     * Keep looking while a host waits for its first player or a client
     * has not joined yet: only then does the name decide who finds whom. */
    if (iWantTransport && ++iNameTicks >= KGcNameTicks) {
        iNameTicks = 0;
        TInt role = gcd_role(iDev);
        TBool waiting = EFalse;
        if (role == GCD_ROLE_CLIENT) {
            waiting = !iDev->session_open && !iDev->joining;
        } else if (role == GCD_ROLE_HOST && iDev->conn_state == GCD_CS_NOT_CONNECTED) {
            TInt i;
            waiting = ETrue;
            for (i = 1; i < GC_MAX_PLAYERS; i++) {
                if (iDev->peer[i].present) {
                    waiting = EFalse;
                }
            }
        }
        if (waiting) {
            RefreshName();
        }
    }
}

void CGcEngine::LogName(const TDesC8 &aWhat)
{
    TBuf8<80> t;
    t.Copy(aWhat);
    t.Append(_L8(": "));
    t.Append(TPtrC8((const TUint8 *)iDev->cfg.name, User::StringLength((const TUint8 *)iDev->cfg.name)));
    Log(t);
}

/* ---- transport ---- */

void CGcEngine::TransportUp()
{
    Log(_L8("bridge connected"));
    gcd_transport_up(iDev, Now());
}

void CGcEngine::TransportData(const TDesC8 &aData)
{
    gcd_transport_input(iDev, aData.Ptr(), (size_t)aData.Length(), Now());
}

void CGcEngine::TransportDown(TInt aError)
{
    TBuf8<48> t;
    t.Copy(_L8("bridge connection failed/lost: "));
    t.AppendNum(aError);
    Log(t);
    gcd_transport_down(iDev, Now());
    if (aError == KErrCancel && iDev->role == GCD_ROLE_IDLE) {
        iWantTransport = EFalse;
    }
    if (aError == KErrCancel && iDev->selecting) {
        /* user cancelled the bridge selection: cancel the host selection */
        gcd_select_session(iDev, 0, Now());
        iWantTransport = EFalse;
    }
    if (iWantTransport && !iRetry->IsActive()) {
        iRetry->Start(KGcRetryUs, KGcRetryUs, TCallBack(RetryCallBack, this));
    }
}

void CGcEngine::TransportSelecting(TBool aShowing)
{
    iSelectingBridge = aShowing;
    LogNum(_L8("device selection shown"), aShowing ? 1 : 0);
}

void CGcEngine::TransportProgress(const TDesC8 &aStep, TInt aValue)
{
    LogNum(aStep, aValue);
}

/* ---- event dispatch ---- */

TInt CGcEngine::DispatchCallBack(TAny *aSelf)
{
    ((CGcEngine *)aSelf)->Dispatch();
    return 0;
}

void CGcEngine::Dispatch()
{
    if (iDispatching) {
        return;
    }
    iDispatching = ETrue;
    /* deliver one batch; callbacks may call back into the API */
    TInt budget = GCD_EVENT_QUEUE;
    while (budget-- > 0 && gcd_pop_event(iDev, iEvent)) {
        switch (iEvent->type) {
        case GCD_EV_SESSION_LIST:
            TRAPD(e1, ShowListL(*iEvent, EFalse));
            (void)e1;
            break;
        case GCD_EV_UI_ADD_DEVICE:
            TRAPD(e2, ShowListL(*iEvent, ETrue));
            (void)e2;
            break;
        case GCD_EV_UI_CLOSE:
            CloseList();
            break;
        case GCD_EV_TRANSPORT:
            break;
        case GCD_EV_DISCOVERED:
        {
            MGcSessionListObserver *obs = iListObserver;
            iListObserver = NULL;
            LogNum(_L8("open games found"), iEvent->id);
            {
                TInt k;
                const gcd_device *found = (const gcd_device *)iEvent->data;
                for (k = 0; k < iEvent->id; k++) {
                    TBuf8<80> t;
                    t.Copy(_L8("  game: "));
                    t.Append(TPtrC8((const TUint8 *)found[k].name, User::StringLength((const TUint8 *)found[k].name)));
                    Log(t);
                }
            }
            if (obs) {
                obs->SessionList((const gcd_device *)iEvent->data, iEvent->id);
            }
            break;
        }
        default:
        {
            TRAPD(err, iSink.HandleGcEventL(*iEvent));
            if (err != KErrNone) {
                Log(_L8("game callback left"));
            }
            break;
        }
        }
    }
    iDispatching = EFalse;
    if (iDev->ev_count && !iDispatcher->IsActive()) {
        iDispatcher->CallBack();
    }
}

/* ---- built-in UI ---- */

void CGcEngine::ShowListL(const gcd_event &aEvent, TBool aHostConfirm)
{
    if (!iList) {
        TRect rect;
        CCoeEnv *env = CCoeEnv::Static();
        CEikAppUi *ui = iAppUi;
        if (!ui && env) {
            ui = (CEikAppUi *)env->AppUi();
        }
        if (ui) {
            rect = ui->ClientRect();
        } else {
            rect = TRect(0, 0, 176, 208);
        }
        iList = CGcListControl::NewL(*this, aHostConfirm ? KGcConfirmPlayers() : KGcSelectHost(), rect);
        iListIsHostConfirm = aHostConfirm;
        iListCount = 0;
    }
    if (aHostConfirm) {
        if (iListCount < GCD_MAX_SESSIONS) {
            iListItems[iListCount++] = aEvent.dev;
        }
    } else {
        TInt n = aEvent.id;
        if (n > GCD_MAX_SESSIONS) {
            n = GCD_MAX_SESSIONS;
        }
        Mem::Copy(iListItems, aEvent.data, n * (TInt)sizeof(gcd_device));
        iListCount = n;
    }
    RefreshListL();
}

void CGcEngine::RefreshListL()
{
    if (!iList) {
        return;
    }
    CDesCArrayFlat *items = new (ELeave) CDesCArrayFlat(4);
    CleanupStack::PushL(items);
    TInt i;
    for (i = 0; i < iListCount; i++) {
        TBuf<GC_MAX_NAME + 1> name;
        name.Copy(TPtrC8((const TUint8 *)iListItems[i].name, User::StringLength((const TUint8 *)iListItems[i].name)));
        items->AppendL(name);
    }
    iList->SetItemsL(*items);
    CleanupStack::PopAndDestroy(items);
}

void CGcEngine::CloseList()
{
    /* The list may be closed from inside its own OfferKeyEventL(): hide
     * it now, delete it from the next timer tick. */
    if (iList) {
        iList->Dismiss();
        delete iDoomedList;
        iDoomedList = iList;
        iList = NULL;
    }
    iListCount = 0;
}

void CGcEngine::ListItemSelected(TInt aIndex)
{
    if (aIndex < 0 || aIndex >= iListCount) {
        return;
    }
    gcd_device dev = iListItems[aIndex];
    if (iListIsHostConfirm) {
        /* accept this player, keep the list for further players */
        TInt i;
        for (i = aIndex; i + 1 < iListCount; i++) {
            iListItems[i] = iListItems[i + 1];
        }
        iListCount--;
        gcd_v2_declare(iDev, &dev, Now());
        TRAPD(err, RefreshListL());
        (void)err;
    } else {
        CloseList();
        gcd_select_session(iDev, dev.session_id, Now());
    }
    Kick();
}

void CGcEngine::ListCancelled()
{
    CloseList();
    if (iDev->selecting) {
        gcd_select_session(iDev, 0, Now());
    }
    Kick();
}
