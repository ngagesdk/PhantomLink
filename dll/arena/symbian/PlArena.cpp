/** @file PlArena.cpp
 *
 *  PLARENA.DLL (see PlArena.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "PlArena.h"
#include "GcConfig.h"
#include "GcTransport.h"
#include <e32math.h>
#include <e32svr.h>
#include <f32file.h>

/* euser.lib of the 6.1 SDK lacks Dll::Tls (see GcEngine.cpp); the key
 * is this DLL's UID3 */
static const TInt KPlaTlsHandle = 0x0FA4E7A1;

_LIT(KPlaLogFile, "C:\\System\\Data\\PlArena.log");
const TInt KPlaLogMax = 64 * 1024;
const TInt KPlaTickUs = 50000;    /* link timers */
const TInt KPlaRetryUs = 3000000; /* bridge reconnect interval */

class CPlArena : public CBase, public MGcTransportObserver
{
  public:
    static CPlArena *NewL();
    ~CPlArena();
    void Log(const TDesC8 &aLine);

    // MGcTransportObserver
    void TransportUp();
    void TransportData(const TDesC8 &aData);
    void TransportDown(TInt aError);
    void TransportSelecting(TBool /*aShowing*/) {}
    void TransportProgress(const TDesC8 &aStep, TInt aValue);

    static TInt Tick(TAny *aSelf);

    TInt iRefs;
    pla_core iCore;
    TGcConfig iConfig;
    CGcTransport *iTransport;
    CPeriodic *iTimer;
    TInt iRetryTicks;
    RFs iFs;
    RFile iLog;
    TBool iLogOpen;
    TInt iLogSize;

  private:
    void ConstructL();
};

static CPlArena *Instance()
{
    return (CPlArena *)UserSvr::DllTls(KPlaTlsHandle);
}

static gc_u32 NowMs()
{
    /* the N-Gage tick is 1/64 s; resolution is fine for link timers */
    return (gc_u32)User::TickCount() * 1000u / 64u;
}

static void CoreWrite(void *aCtx, const gc_u8 *aData, size_t aLen)
{
    CPlArena *self = (CPlArena *)aCtx;
    if (self->iTransport && self->iTransport->IsConnected()) {
        self->iTransport->Write(TPtrC8(aData, (TInt)aLen));
    }
}

CPlArena *CPlArena::NewL()
{
    CPlArena *self = new (ELeave) CPlArena;
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
}

void CPlArena::ConstructL()
{
    pla_io io;
    gc_u8 id[GC_DEVICE_ID_LEN];
    TInt64 seed = User::TickCount();
    TTime t;
    TBuf8<GC_MAX_NAME + 1> name;
    gc_u32 linkId;
    TInt i;

    if (iFs.Connect() == KErrNone) {
        TInt err = iLog.Open(iFs, KPlaLogFile, EFileWrite | EFileShareAny);
        if (err == KErrNotFound) {
            err = iLog.Create(iFs, KPlaLogFile, EFileWrite | EFileShareAny);
        }
        if (err == KErrNone) {
            TInt pos = 0;
            iLog.Size(iLogSize);
            if (iLogSize > KPlaLogMax) {
                iLog.SetSize(0);
                iLogSize = 0;
            }
            iLog.Seek(ESeekEnd, pos);
            iLogOpen = ETrue;
        }
    }
    iConfig.Load();
    /* link id and device id: no secure RNG on 6.1, mix time sources */
    t.HomeTime();
    seed += t.Int64();
    linkId = (gc_u32)Math::Rand(seed);
    id[0] = 0x02; /* locally administered, not a real Bluetooth address */
    for (i = 1; i < GC_DEVICE_ID_LEN; i++) {
        id[i] = (gc_u8)Math::Rand(seed);
    }
    name.Copy(iConfig.iDeviceName.Length() ? iConfig.iDeviceName : _L8("N-Gage"));
    name.ZeroTerminate();
    io.write = CoreWrite;
    io.ctx = this;
    pla_init(&iCore, &io, (const char *)name.Ptr(), id, linkId);
    iTransport = CGcTransport::NewL(*this, iConfig);
    iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
    iTimer->Start(KPlaTickUs, KPlaTickUs, TCallBack(Tick, this));
    Log(_L8("PlArena started"));
    iTransport->ConnectL();
}

CPlArena::~CPlArena()
{
    delete iTimer;
    delete iTransport;
    if (iLogOpen) {
        iLog.Close();
    }
    iFs.Close();
}

void CPlArena::Log(const TDesC8 &aLine)
{
    if (!iLogOpen || iLogSize > KPlaLogMax) {
        return;
    }
    TBuf8<16> stamp;
    stamp.Format(_L8("%u "), (TUint)NowMs());
    iLog.Write(stamp);
    iLog.Write(aLine);
    iLog.Write(_L8("\r\n"));
    iLogSize += stamp.Length() + aLine.Length() + 2;
}

void CPlArena::TransportUp()
{
    Log(_L8("bridge connected"));
    pla_transport_up(&iCore, NowMs());
}

void CPlArena::TransportData(const TDesC8 &aData)
{
    pla_input(&iCore, aData.Ptr(), (size_t)aData.Length(), NowMs());
}

void CPlArena::TransportDown(TInt aError)
{
    TBuf8<40> l;
    l.Format(_L8("bridge lost (%d)"), aError);
    Log(l);
    pla_transport_down(&iCore, NowMs());
    iRetryTicks = KPlaRetryUs / KPlaTickUs;
}

void CPlArena::TransportProgress(const TDesC8 &aStep, TInt aValue)
{
    TBuf8<80> l;
    l.Copy(aStep.Left(60));
    l.AppendFormat(_L8(" %d"), aValue);
    Log(l);
}

TInt CPlArena::Tick(TAny *aSelf)
{
    CPlArena *self = (CPlArena *)aSelf;
    pla_poll(&self->iCore, NowMs());
    if (self->iRetryTicks > 0 && --self->iRetryTicks == 0 && !self->iTransport->IsBusy()) {
        TRAPD(err, self->iTransport->ConnectL());
        if (err != KErrNone) {
            self->iRetryTicks = KPlaRetryUs / KPlaTickUs;
        }
    }
    return 1;
}

/* ---- exports --------------------------------------------------------- */

EXPORT_C int pla_open(void)
{
    CPlArena *self = Instance();
    if (!self) {
        TRAPD(err, self = CPlArena::NewL());
        if (err != KErrNone) {
            return err;
        }
        if (UserSvr::DllSetTls(KPlaTlsHandle, self) != KErrNone) {
            delete self;
            return KErrNoMemory;
        }
    }
    self->iRefs++;
    return KErrNone;
}

EXPORT_C void pla_close(void)
{
    CPlArena *self = Instance();
    if (self && --self->iRefs <= 0) {
        UserSvr::DllSetTls(KPlaTlsHandle, NULL);
        delete self;
    }
}

EXPORT_C void pla_listen(int aSvc, pla_listener aFn, pla_state_fn aState, void *aCtx)
{
    CPlArena *self = Instance();
    if (self) {
        pla_set_listener(&self->iCore, aSvc, aFn, aState, aCtx);
    }
}

EXPORT_C int pla_submit(gca_msg *aMsg)
{
    CPlArena *self = Instance();
    return self ? pla_send(&self->iCore, aMsg, NowMs()) : KErrNotReady;
}

EXPORT_C int pla_state(void)
{
    CPlArena *self = Instance();
    return self ? self->iCore.state : PLA_ST_DOWN;
}

EXPORT_C void pla_log(const char *aFmt, ...)
{
    CPlArena *self = Instance();
    TBuf8<256> line;
    VA_LIST ap;
    const char *p;

    if (!self || !self->iLogOpen) {
        return;
    }
    VA_START(ap, aFmt);
    for (p = aFmt; *p && line.Length() < 200; p++) {
        if (*p != '%' || !p[1]) {
            line.Append((TChar)*p);
            continue;
        }
        p++;
        if (*p == 's') {
            const char *s = VA_ARG(ap, const char *);
            TInt n = 0;
            while (s && s[n] && n < 80) {
                n++;
            }
            if (s) {
                line.Append(TPtrC8((const TUint8 *)s, n));
            }
        } else if (*p == 'd') {
            line.AppendNum((TInt)VA_ARG(ap, int));
        } else if (*p == 'x') {
            line.AppendNum((TUint)VA_ARG(ap, unsigned), EHex);
        } else if (*p == 'u') {
            line.AppendNum((TUint)VA_ARG(ap, unsigned));
        } else {
            line.Append((TChar)*p);
        }
    }
    VA_END(ap);
    self->Log(line);
}

EXPORT_C void pla_log_hex(const char *aWhat, const void *aData, int aLen)
{
    CPlArena *self = Instance();
    TBuf8<220> line;
    TInt i, n = aLen < 64 ? aLen : 64;

    if (!self || !aData || aLen < 0) {
        return;
    }
    line.Copy(TPtrC8((const TUint8 *)aWhat, User::StringLength((const TUint8 *)aWhat)).Left(24));
    line.Append(':');
    for (i = 0; i < n; i++) {
        line.AppendFormat(_L8(" %02x"), ((const TUint8 *)aData)[i]);
    }
    self->Log(line);
}

GLDEF_C TInt E32Dll(TDllReason /*aReason*/)
{
    return KErrNone;
}
