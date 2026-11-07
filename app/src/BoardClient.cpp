/** Message board client over the bridge's normal GCL link. */
#include "BoardClient.h"
#include <string.h>

CBoardClient::CBoardClient(MBoardObserver &aObserver) : iObserver(aObserver) {}

CBoardClient *CBoardClient::NewL(MBoardObserver &aObserver)
{
    CBoardClient *self = new (ELeave) CBoardClient(aObserver);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop();
    return self;
}

void CBoardClient::ConstructL()
{
    iTransport = CGcTransport::NewL(*this, iConfig);
    iTimer = CPeriodic::NewL(CActive::EPriorityLow);
    iNotifier = CIdle::NewL(CActive::EPriorityIdle);
    iLink = new (ELeave) gc_link;
    iReply = new (ELeave) gc_msg;
}

CBoardClient::~CBoardClient()
{
    Close();
    delete iNotifier;
    delete iTimer;
    delete iTransport;
    delete iLink;
    delete iReply;
}

void CBoardClient::Close()
{
    if (iNotifier) {
        iNotifier->Cancel();
    }
    if (iTimer) {
        iTimer->Cancel();
    }
    if (iTransport) {
        iTransport->Disconnect();
    }
    iActive = EFalse;
    iBusy = EFalse;
    iRegistered = EFalse;
    iAwaitRegister = EFalse;
    iFinishing = EFalse;
}

void CBoardClient::StartL(const TDesC8 &aName, const TDesC8 &aText, TUint32 aIndex, TBool aPost)
{
    if (iBusy) {
        User::Leave(KErrInUse);
    }
    if (aName.Length() > GC_MAX_NAME || aText.Length() > GC_BOARD_TEXT_MAX) {
        User::Leave(KErrTooBig);
    }
    iName.Copy(aName);
    iText.Copy(aText);
    iIndex = aIndex;
    iPost = aPost;
    iIdleTicks = 0;
    iRegisterRetryTicks = 0;
    iFinishing = EFalse;
    iRequestSent = EFalse;
    memset(iReply, 0, sizeof(*iReply));
    if (iActive) {
        iBusy = ETrue;
        if (!iRegistered || iName.Compare(iRegisteredName) != 0) {
            Register();
        } else {
            SendRequest();
        }
        return;
    }
    iNow = 0;
    iSelecting = EFalse;
    iRegistered = EFalse;
    iAwaitRegister = EFalse;
    gc_link_cfg cfg;
    gc_link_ops ops;
    gc_link_default_cfg(&cfg, GC_ROLE_DEVICE, 1);
    memset(&ops, 0, sizeof(ops));
    ops.write = Write;
    ops.on_msg = Message;
    ops.on_state = State;
    gc_link_init(iLink, &cfg, &ops, this);
    iConfig.Load();
    iTransport->ConnectL();
    iActive = ETrue;
    iBusy = ETrue;
    iTimer->Start(100000, 100000, TCallBack(Tick, this));
}

void CBoardClient::Finish(TInt aError)
{
    if (!iActive) {
        return;
    }
    if (iFinishing) {
        if (aError != KErrNone && iResult == KErrNone) {
            iResult = aError;
            iTimer->Cancel();
        }
        return;
    }
    iBusy = ETrue;
    iFinishing = ETrue;
    iResult = aError;
    if (aError != KErrNone) {
        iTimer->Cancel();
    }
    iNotifier->Start(TCallBack(Notify, this));
}

TInt CBoardClient::Notify(TAny *aSelf)
{
    CBoardClient *self = static_cast<CBoardClient *>(aSelf);
    if (self->iResult != KErrNone) {
        self->iTransport->Disconnect();
        self->iActive = EFalse;
        self->iRegistered = EFalse;
    }
    self->iBusy = EFalse;
    self->iFinishing = EFalse;
    self->iObserver.BoardFinished(self->iResult, *self->iReply);
    return 0;
}

TInt CBoardClient::Tick(TAny *aSelf)
{
    CBoardClient *self = static_cast<CBoardClient *>(aSelf);
    self->iNow += 100;
    if (self->iBusy && !self->iFinishing && !self->iSelecting && ++self->iIdleTicks > 250) {
        self->Finish(KErrTimedOut);
    }
    if (self->iActive && !self->iFinishing) {
        gc_link_poll(self->iLink, self->iNow);
        if (self->iBusy && !self->iFinishing && self->iAwaitRegister && self->iRegisterRetryTicks > 0 &&
            --self->iRegisterRetryTicks == 0) {
            self->Register();
        }
    }
    return 1;
}

void CBoardClient::Send(const gc_msg &aMessage)
{
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t n = gc_msg_encode(&aMessage, buf, sizeof(buf));
    if (!n || gc_link_send(iLink, buf, n, iNow) != GC_LINK_OK) {
        Finish(KErrOverflow);
    }
}

void CBoardClient::Register()
{
    gc_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GC_MT_REGISTER;
    m.u.reg.api = GC_API_V1;
    m.u.reg.variant = 1;
    memcpy(m.u.reg.name, iName.Ptr(), iName.Length());
    iRegistered = EFalse;
    iAwaitRegister = ETrue;
    iRegisterRetryTicks = 0;
    Send(m);
}

void CBoardClient::SendRequest()
{
    gc_msg m;
    if (!iBusy || iRequestSent) {
        return;
    }
    iRequestSent = ETrue;
    memset(&m, 0, sizeof(m));
    m.type = iPost ? GC_MT_BOARD_POST : GC_MT_BOARD_READ;
    m.u.board.index = iIndex;
    if (iPost) {
        memcpy(m.u.board.text, iText.Ptr(), iText.Length());
    }
    Send(m);
}

void CBoardClient::Write(void *aSelf, const gc_u8 *aData, size_t aLength)
{
    CBoardClient *self = static_cast<CBoardClient *>(aSelf);
    if (self->iTransport->Write(TPtrC8(aData, aLength)) != KErrNone) {
        self->Finish(KErrOverflow);
    }
}

void CBoardClient::State(void *aSelf, int aEvent, int /*aReason*/)
{
    CBoardClient *self = static_cast<CBoardClient *>(aSelf);
    if (!self->iActive) {
        return;
    }
    if ((aEvent == GC_LE_UP_FRESH || aEvent == GC_LE_UP_RESUMED) && !self->iFinishing) {
        self->Register();
    } else if (aEvent == GC_LE_DOWN || aEvent == GC_LE_CLOSED) {
        self->Finish(KErrDisconnected);
    }
}

void CBoardClient::Message(void *aSelf, const gc_u8 *aData, size_t aLength)
{
    CBoardClient *self = static_cast<CBoardClient *>(aSelf);
    if (!self->iActive || self->iFinishing) {
        return;
    }
    gc_msg m;
    if (!gc_msg_decode(aData, aLength, &m)) {
        self->Finish(KErrCorrupt);
        return;
    }
    if (m.type == GC_MT_REGISTERED) {
        if (!self->iAwaitRegister) {
            return;
        }
        if (m.u.registered.result == GC_R_UNAVAILABLE && self->iBusy) {
            self->iRegisterRetryTicks = 20;
            return;
        }
        if (m.u.registered.result != GC_R_OK) {
            self->Finish(m.u.registered.result == GC_R_REJECTED ? KErrAccessDenied : KErrCouldNotConnect);
            return;
        }
        self->iAwaitRegister = EFalse;
        self->iRegisterRetryTicks = 0;
        self->iRegistered = ETrue;
        self->iRegisteredName.Copy(self->iName);
        self->SendRequest();
    } else if (m.type == GC_MT_ERROR) {
        *self->iReply = m;
        self->Finish(KErrGeneral);
    } else if (self->iBusy && self->iRequestSent &&
               ((self->iPost && m.type == GC_MT_BOARD_POSTED) || (!self->iPost && m.type == GC_MT_BOARD_ENTRY))) {
        *self->iReply = m;
        self->Finish(KErrNone);
    }
}

void CBoardClient::TransportUp()
{
    TTime t;
    t.UniversalTime();
    TUint32 id = t.Int64().Low() ^ (TUint32)this ^ 0x5A5A0000;
    gc_link_start(iLink, id ? id : 1, iNow);
}

void CBoardClient::TransportData(const TDesC8 &aData)
{
    if (iActive) {
        gc_link_input(iLink, aData.Ptr(), aData.Length(), iNow);
    }
}

void CBoardClient::TransportDown(TInt aError)
{
    Finish(aError != KErrNone ? aError : KErrDisconnected);
}

void CBoardClient::TransportSelecting(TBool aShowing)
{
    iSelecting = aShowing;
    iIdleTicks = 0;
}

void CBoardClient::TransportProgress(const TDesC8 & /*aStep*/, TInt /*aValue*/) {}
