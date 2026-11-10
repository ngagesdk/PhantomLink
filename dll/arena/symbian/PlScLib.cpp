/** @file PlScLib.cpp
 *
 *  sc_lib.dll replacement (UID3 0x101fde44): the SegaChat (Jabber)
 *  client of the N-Gage Arena stack.  arenabase.dll loads it
 *  dynamically; arenaframework.dll and cafmjabber.dll import ordinal 3.
 *
 *  The original client object has 26 virtual slots (docs/ARENA.md lists
 *  the SegaChat request each one builds).  Status: every slot is logged
 *  with its arguments and caller; nothing is forwarded yet because the
 *  argument layouts are still unverified.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "PlArena.h"
#include <e32base.h>

#define PL_DLL "chat"
#define PL_SLOT(n)                                        \
    virtual TInt S##n(TInt a1, TInt a2, TInt a3, TInt a4) \
    {                                                     \
        PLA_TRACE(PL_DLL, 0x##n, a1, a2, a3, a4);         \
        return 0;                                         \
    }

class CPlScLib : public CBase
{
  public:
    virtual ~CPlScLib(); /* 0x08 */
    PL_SLOT(0c)          /* ConstructL, called by NewLC */
    PL_SLOT(10)          /* connect/login */
    PL_SLOT(14)          /* startup, send_event */
    PL_SLOT(18)          /* send_event variants (reporter: match results) */
    PL_SLOT(1c)
    PL_SLOT(20)
    PL_SLOT(24) /* get roster */
    PL_SLOT(28) /* message: chat/groupchat/headline */
    PL_SLOT(2c) /* presence */
    PL_SLOT(30) /* buddy accept/reject */
    PL_SLOT(34)
    PL_SLOT(38) /* roster item */
    PL_SLOT(3c) /* remove buddy */
    PL_SLOT(40) /* send_event */
    PL_SLOT(44) /* retrieve request (rankings) */
    PL_SLOT(48) /* browse jid */
    PL_SLOT(4c) /* number of offline messages */
    PL_SLOT(50) /* offline headers */
    PL_SLOT(54) /* fetch offline messages */
    PL_SLOT(58)
    PL_SLOT(5c) /* purge offline messages */
    PL_SLOT(60)
    PL_SLOT(64) /* buddy request (subscribe) */
    PL_SLOT(68) /* data message */
    PL_SLOT(6c) /* service request */

    void ConstructL();
    static void OnMsg(void *aCtx, const gca_msg *aMsg);
    TBool iOpen;
};

void CPlScLib::ConstructL()
{
    iOpen = pla_open() == KErrNone;
    if (iOpen) {
        pla_listen(PLA_SVC_CHAT, OnMsg, NULL, this);
        pla_log("chat: created");
    }
}

CPlScLib::~CPlScLib()
{
    if (iOpen) {
        pla_log("chat: deleted");
        pla_listen(PLA_SVC_CHAT, NULL, NULL, NULL);
        pla_close();
    }
}

void CPlScLib::OnMsg(void * /*aCtx*/, const gca_msg *aMsg)
{
    pla_log("chat: <- %s '%s'", gca_name(aMsg->type), aMsg->name);
}

/* ---- exports (sc_lib.def) -------------------------------------------- */

/* 1: NewLC() */
extern "C" EXPORT_C CPlScLib *PlChatNewLC()
{
    CPlScLib *self = new (ELeave) CPlScLib;
    CleanupStack::PushL(self);
    self->ConstructL();
    return self;
}

/* 2: returns 0 in the original */
extern "C" EXPORT_C TInt PlChatZero()
{
    return 0;
}

/* 3: NewL() */
extern "C" EXPORT_C CPlScLib *PlChatNewL()
{
    CPlScLib *self = PlChatNewLC();
    CleanupStack::Pop(self);
    return self;
}

GLDEF_C TInt E32Dll(TDllReason /*aReason*/)
{
    return KErrNone;
}
