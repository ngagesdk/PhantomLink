/** @file PlHttpRequest.cpp
 *
 *  HttpDllRequest.dll replacement (UID3 0x10202464): the HTTP client
 *  that arenaframework.dll, arenabase.dll and cafmlogin.dll load by name
 *  (RLibrary::Load, ordinal 1) to talk to the Community web service
 *  (SOAP envelopes are built by httpinterface.dll helpers).
 *
 *  The concrete class has 21 virtual slots; its interface declares seven
 *  virtual functions before the virtual destructor (slot 0x24), so the
 *  same order is kept here.  Status: every slot is logged; once the slots
 *  that set the URL, headers and body and submit the request are
 *  confirmed by the log, they are forwarded as GCA HTTP_REQ and the
 *  answer is delivered through the observer (gcs_arena already answers
 *  authenticateUser2, checkImei and createUser).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "PlArena.h"
#include <e32base.h>

#define PL_DLL "http"
#define PL_SLOT(n)                                        \
    virtual TInt S##n(TInt a1, TInt a2, TInt a3, TInt a4) \
    {                                                     \
        PLA_TRACE(PL_DLL, 0x##n, a1, a2, a3, a4);         \
        return 0;                                         \
    }

/* Deliberately not derived from CBase: CBase's virtual destructor would
 * take the first slot.  The original's root class declares seven virtual
 * functions first and the virtual destructor in slot 0x24, and GCC 2.9
 * lays out a root class's vtable in declaration order. */
class CPlHttpRequest
{
  public:
    PL_SLOT(08) /* open session, string pool */
    PL_SLOT(0c) /* new transaction (URI, method) */
    PL_SLOT(10) /* add header */
    PL_SLOT(14) /* add header */
    PL_SLOT(18)
    PL_SLOT(1c)                /* set header / content type */
    PL_SLOT(20)                /* close */
    virtual ~CPlHttpRequest(); /* 0x24 */
    PL_SLOT(28)                /* body */
    PL_SLOT(2c)                /* body */
    PL_SLOT(30)                /* cancel */
    PL_SLOT(34)                /* submit */
    PL_SLOT(38)                /* submit */
    PL_SLOT(3c)
    PL_SLOT(40)
    PL_SLOT(44)
    PL_SLOT(48)
    PL_SLOT(4c)
    PL_SLOT(50)
    PL_SLOT(54)
    PL_SLOT(58)

    void Construct();
    static void OnMsg(void *aCtx, const gca_msg *aMsg);
    TBool iOpen;
};

void CPlHttpRequest::Construct()
{
    iOpen = pla_open() == KErrNone;
    if (iOpen) {
        pla_listen(PLA_SVC_HTTP, OnMsg, NULL, this);
        pla_log("http: created");
    }
}

CPlHttpRequest::~CPlHttpRequest()
{
    if (iOpen) {
        pla_log("http: deleted");
        pla_listen(PLA_SVC_HTTP, NULL, NULL, NULL);
        pla_close();
    }
}

void CPlHttpRequest::OnMsg(void * /*aCtx*/, const gca_msg *aMsg)
{
    pla_log("http: <- %s req %u status %u", gca_name(aMsg->type), (unsigned)aMsg->req, (unsigned)aMsg->value);
}

/* 1: factory; the original allocates without leaving and returns NULL on
 * failure */
extern "C" EXPORT_C CPlHttpRequest *PlHttpNew()
{
    CPlHttpRequest *self = new CPlHttpRequest;
    if (self) {
        self->Construct();
    }
    return self;
}

GLDEF_C TInt E32Dll(TDllReason /*aReason*/)
{
    return KErrNone;
}
