/** Message board client over the bridge's normal GCL link. */
#ifndef BOARDCLIENT_H
#define BOARDCLIENT_H

#include "GcTransport.h"
#include "gc_msg.h"

class MBoardObserver
{
  public:
    virtual void BoardFinished(TInt aError, const gc_msg &aReply) = 0;
};

class CBoardClient : public CBase, public MGcTransportObserver
{
  public:
    static CBoardClient *NewL(MBoardObserver &aObserver);
    ~CBoardClient();
    void StartL(const TDesC8 &aName, const TDesC8 &aText, TUint32 aIndex, TBool aPost);
    void Close();
    TBool IsBusy() const { return iBusy; }
    void TransportUp();
    void TransportData(const TDesC8 &aData);
    void TransportDown(TInt aError);
    void TransportSelecting(TBool aShowing);
    void TransportProgress(const TDesC8 &aStep, TInt aValue);

  private:
    CBoardClient(MBoardObserver &aObserver);
    void ConstructL();
    void Finish(TInt aError);
    void Send(const gc_msg &aMessage);
    void Register();
    void SendRequest();
    static void Write(void *aSelf, const gc_u8 *aData, size_t aLength);
    static void Message(void *aSelf, const gc_u8 *aData, size_t aLength);
    static void State(void *aSelf, int aEvent, int aReason);
    static TInt Tick(TAny *aSelf);
    static TInt Notify(TAny *aSelf);

    MBoardObserver &iObserver;
    TGcConfig iConfig;
    CGcTransport *iTransport;
    CPeriodic *iTimer;
    CIdle *iNotifier;
    gc_link *iLink;
    gc_msg *iReply;
    TBuf8<GC_MAX_NAME> iName;
    TBuf8<GC_MAX_NAME> iRegisteredName;
    TBuf8<GC_BOARD_TEXT_MAX> iText;
    TUint32 iIndex;
    TUint32 iNow;
    TInt iResult;
    TInt iIdleTicks;
    TInt iRegisterRetryTicks;
    TBool iPost;
    TBool iBusy;
    TBool iActive;
    TBool iRegistered;
    TBool iAwaitRegister;
    TBool iFinishing;
    TBool iRequestSent;
    TBool iSelecting;
};
#endif
