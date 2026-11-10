/** @file PlSnapComm.cpp
 *
 *  SnapComm.dll replacement (UID3 0x101fde38): the SNAP game server
 *  client of the N-Gage Arena stack.  arenabase.dll loads it with
 *  RLibrary::Load + UID check and calls ordinal 1 (NewLC).
 *
 *  The original is a C++ facade (38 virtual slots, docs/ARENA.md) over a
 *  portable C SNAP client.  Status of this replacement: every slot is
 *  logged with its arguments and caller; Login (slot 0x10) is forwarded
 *  as GCA LOGIN; the callback observers registered through slot 0x98 are
 *  stored but not called yet, because the layouts of the event structs
 *  they expect are not known.  Slot 0x9c returns the last error.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "PlArena.h"
#include <e32base.h>

#define PL_DLL "snap"
#define PL_SLOT(n)                                        \
    virtual TInt S##n(TInt a1, TInt a2, TInt a3, TInt a4) \
    {                                                     \
        PLA_TRACE(PL_DLL, 0x##n, a1, a2, a3, a4);         \
        return 0;                                         \
    }

const TInt KPlSnapCallbacks = 64; /* callback ids seen: 0..0x23 */

class CPlSnapComm : public CBase
{
  public:
    CPlSnapComm();
    virtual ~CPlSnapComm(); /* slot 0x08 */
    /* 0x0c: called by NewL(a1..a4): server address and options */
    PL_SLOT(0c)
    /* 0x10: login.  arenabase passes four C strings (r1, r2, r3 and the
     * first stack word), then the server TInetAddr, a value, three
     * integers and its observer. */
    virtual TInt S10(const char *a1, const char *a2, const char *a3, const char *a4);
    PL_SLOT(14)
    PL_SLOT(18)
    PL_SLOT(1c)
    PL_SLOT(20)
    PL_SLOT(24)
    PL_SLOT(28)
    PL_SLOT(2c)
    PL_SLOT(30)
    PL_SLOT(34)
    PL_SLOT(38)
    PL_SLOT(3c)
    PL_SLOT(40)
    PL_SLOT(44)
    PL_SLOT(48)
    PL_SLOT(4c)
    PL_SLOT(50)
    PL_SLOT(54)
    PL_SLOT(58)
    PL_SLOT(5c)
    PL_SLOT(60)
    PL_SLOT(64)
    PL_SLOT(68)
    PL_SLOT(6c)
    PL_SLOT(70)
    PL_SLOT(74)
    PL_SLOT(78)
    PL_SLOT(7c)
    PL_SLOT(80)
    PL_SLOT(84)
    PL_SLOT(88)
    PL_SLOT(8c)
    PL_SLOT(90) /* logout / disconnect */
    PL_SLOT(94) /* clears all callbacks */
    /* 0x98: SetCallback(id, observer), returns the previous observer */
    virtual TAny *S98(TInt aId, TAny *aObserver);
    /* 0x9c: last error */
    virtual TInt S9c();

    static void OnMsg(void *aCtx, const gca_msg *aMsg);
    static void OnState(void *aCtx, int aState);

    TBool iOpen;
    TInt iLastError;
    TAny *iCallbacks[KPlSnapCallbacks];
};

CPlSnapComm::CPlSnapComm()
{
    iOpen = pla_open() == KErrNone;
    if (iOpen) {
        pla_listen(PLA_SVC_SNAP, OnMsg, OnState, this);
        pla_log("snap: created");
    }
}

CPlSnapComm::~CPlSnapComm()
{
    if (iOpen) {
        pla_log("snap: deleted");
        pla_listen(PLA_SVC_SNAP, NULL, NULL, NULL);
        pla_close();
    }
}

static void LogStr(const char *aWhat, const char *aStr)
{
    pla_log("snap:   %s = '%s'", aWhat, aStr ? aStr : "(null)");
}

TInt CPlSnapComm::S10(const char *a1, const char *a2, const char *a3, const char *a4)
{
    gca_msg m;
    TInt n;

    PLA_TRACE(PL_DLL, 0x10, a1, a2, a3, a4);
    LogStr("r1", a1);
    LogStr("r2", a2);
    LogStr("r3", a3);
    LogStr("sp0", a4);
    Mem::FillZ(&m, sizeof(m));
    m.type = GCA_MT_LOGIN;
    for (n = 0; a1 && a1[n] && n < GCA_MAX_NAME; n++) {
        m.name[n] = a1[n];
    }
    for (n = 0; a2 && a2[n] && n < GCA_MAX_TEXT; n++) {
        m.text[n] = a2[n];
    }
    n = pla_submit(&m);
    pla_log("snap:   LOGIN req %u -> %d", (unsigned)m.req, n);
    iLastError = 0;
    return 0; /* arenabase treats non-zero as failure */
}

TAny *CPlSnapComm::S98(TInt aId, TAny *aObserver)
{
    TAny *old = NULL;
    PLA_TRACE(PL_DLL, 0x98, aId, aObserver, 0, 0);
    if (aId >= 0 && aId < KPlSnapCallbacks) {
        old = iCallbacks[aId];
        iCallbacks[aId] = aObserver;
    }
    return old;
}

TInt CPlSnapComm::S9c()
{
    return iLastError;
}

void CPlSnapComm::OnMsg(void * /*aCtx*/, const gca_msg *aMsg)
{
    pla_log("snap: <- %s req %u result %d id %x '%s'", gca_name(aMsg->type), (unsigned)aMsg->req,
            (int)aMsg->result, (unsigned)aMsg->id, aMsg->name);
}

void CPlSnapComm::OnState(void * /*aCtx*/, int aState)
{
    pla_log("snap: link state %d", aState);
}

/* ---- exports (snapcomm.def) ------------------------------------------ */

/* 1: NewLC() - arenabase pops it itself */
extern "C" EXPORT_C CPlSnapComm *PlSnapNewLC()
{
    CPlSnapComm *self = new (ELeave) CPlSnapComm;
    CleanupStack::PushL(self);
    return self;
}

/* 2: NewL(a1, a2, a3, a4): NewLC + slot 0x0c + Pop */
extern "C" EXPORT_C CPlSnapComm *PlSnapNewL(TInt a1, TInt a2, TInt a3, TInt a4)
{
    CPlSnapComm *self = PlSnapNewLC();
    self->S0c(a1, a2, a3, a4);
    CleanupStack::Pop(self);
    return self;
}

/* 3..5: empty functions in the original */
extern "C" EXPORT_C void PlSnapNop3() {}
extern "C" EXPORT_C void PlSnapNop4() {}
extern "C" EXPORT_C void PlSnapNop5() {}

GLDEF_C TInt E32Dll(TDllReason /*aReason*/)
{
    return KErrNone;
}
