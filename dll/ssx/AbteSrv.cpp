/** @file AbteSrv.cpp
 *
 *  PhantomLink's C:\System\Libs\abtesrv.dll for SSX Out of Bounds
 *  (docs/SSX.md).
 *
 *  SSX's Arena code ("GSBLIB" in 6R65.APP) does not know the address of
 *  its game server.  It asks the discovery server (discovery.cng.n-gage.com,
 *  UDP 41001), which hands out this "billing DLL"; the game writes it to
 *  C:\System\Libs, loads it with RLibrary::Load and calls ordinal 1.  The
 *  object it gets is used through these virtual slots (6R65.APP
 *  0x100b6e74 and the GSB state machine at 0x100b8000):
 *
 *    0x08  ConstructL()
 *    0x10  upload/download of a server object with an upload type   -> 0x98
 *    0x14  after the logon: account/billing check (2 buffers)       -> 150 (ok)
 *    0x18  download by name                                         -> 0x98
 *    0x1c  (status)                                                 -> 0x99
 *    0x20  Cancel()
 *    0x24  after a plug-in download                                 -> 0x9a
 *    0x28  a 16-bit value the account messages carry
 *    0x2c  name of the DLL (TDesC8, up to 17 bytes, sent in discovery)
 *    0x30  game server host name: a TBuf16<72> copied as 0x98 raw bytes
 *    0x34  game server port
 *
 *  The game deletes the object with User::Free, without a destructor:
 *  hence no CBase.  The vtable pointer must be at offset 0: the interface
 *  has no data, the data lives in the derived class.
 *
 *  This one sends the game back to the server it came from: the
 *  discovery host name (which the PhantomLink app puts in the hosts file
 *  and the bridges resolve to themselves) and port 41001; the PhantomLink
 *  server answers both discovery and game messages there.  Every check
 *  passes at once.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include <e32base.h>

_LIT(KAbteHost, "discovery.cng.n-gage.com");
_LIT8(KAbteName, "PhantomLink");
const TInt KAbtePort = 41001;

/* completion codes the GSB state machine expects (0x100b8000) */
const TInt KAbteObjectOk = 0x98;
const TInt KAbteFailed = 0x99;
const TInt KAbteFileOk = 0x9a;
const TInt KAbteLogonOk = 150;

class MAbteSrv
{
  public:
    virtual void ConstructL() = 0;                                                        /* 0x08 */
    virtual TInt Slot0c(TInt a1, TInt a2, TInt a3) = 0;                                   /* 0x0c */
    virtual void ObjectL(TInt a1, TInt aId, TInt aType, TInt a4, TRequestStatus *aS) = 0; /* 0x10 */
    virtual void LogonCheckL(TAny *a1, TAny *a2, TRequestStatus *aS) = 0;                 /* 0x14 */
    virtual void ByNameL(TAny *aName, TRequestStatus *aS) = 0;                            /* 0x18 */
    virtual void Request1cL(TRequestStatus *aS) = 0;                                      /* 0x1c */
    virtual void Cancel() = 0;                                                            /* 0x20 */
    virtual void FileDoneL(TRequestStatus *aS) = 0;                                       /* 0x24 */
    virtual TInt Value() = 0;                                                             /* 0x28 */
    virtual const TDesC8 *Name() = 0;                                                     /* 0x2c */
    virtual const TAny *Host() = 0;                                                       /* 0x30 */
    virtual TInt Port() = 0;                                                              /* 0x34 */
};

class CAbteSrv : public MAbteSrv
{
  public:
    void ConstructL()
    {
        iHost.Copy(KAbteHost);
        iName.Copy(KAbteName);
    }
    TInt Slot0c(TInt, TInt, TInt)
    {
        return KErrNone;
    }
    void ObjectL(TInt, TInt, TInt, TInt, TRequestStatus *aS)
    {
        Done(aS, KAbteObjectOk);
    }
    void LogonCheckL(TAny *, TAny *, TRequestStatus *aS)
    {
        Done(aS, KAbteLogonOk);
    }
    void ByNameL(TAny *, TRequestStatus *aS)
    {
        Done(aS, KAbteObjectOk);
    }
    void Request1cL(TRequestStatus *aS)
    {
        Done(aS, KAbteFailed);
    }
    void Cancel()
    {
    }
    void FileDoneL(TRequestStatus *aS)
    {
        Done(aS, KAbteFileOk);
    }
    TInt Value()
    {
        return 0;
    }
    const TDesC8 *Name()
    {
        return &iName;
    }
    const TAny *Host()
    {
        return &iHost;
    }
    TInt Port()
    {
        return KAbtePort;
    }

  private:
    static void Done(TRequestStatus *aS, TInt aCode)
    {
        if (aS) {
            *aS = KRequestPending;
            User::RequestComplete(aS, aCode);
        }
    }
    TBuf<72> iHost; /* the game copies 0x98 bytes: TBuf16<72> exactly */
    TBuf8<17> iName;
};

/* ordinal 1: called without arguments under a TRAP */
extern "C" EXPORT_C MAbteSrv *AbteSrvNewL()
{
    return new (ELeave) CAbteSrv;
}

GLDEF_C TInt E32Dll(TDllReason /*aReason*/)
{
    return KErrNone;
}
