/** @file PhantomLinkAppUi.cpp
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "PhantomLinkAppUi.h"
#include "GameInstaller.h"
#include "GameList.h"
#include "PhantomLink.hrh"
#include "PhantomLinkView.h"
#include <PhantomLink.rsg>
#include <aknnotewrappers.h>
#include <aknquerydialog.h>
#include <avkon.hrh>
#include <badesca.h>
#include <eikmenup.h>
#include <utf.h>

#include <apparc.h>
#include <eikapp.h>

/* TRAP_IGNORE does not exist in this SDK */
#define PL_IGNORE(aStmt)        \
    do {                        \
        TRAPD(ignored_, aStmt); \
    } while (0)

const TInt KStatusLine = 0;

CPhantomLinkAppUi::CPhantomLinkAppUi() : iMode(EGames), iOperation(ENone)
{
}

void CPhantomLinkAppUi::ConstructL()
{
    BaseConstructL();
    iView = CPhantomLinkView::NewL(ClientRect());
    AddToStackL(iView);
    iClient = CBridgeClient::NewL(*this);
    iBoardClient = CBoardClient::NewL(*this);
    iBoardName = _L("N-Gage");
    Mem::FillZ(&iBoard, sizeof(iBoard));
    iStatus = _L("not read");
    ShowGamesL();
}

CPhantomLinkAppUi::~CPhantomLinkAppUi()
{
    delete iBoardClient;
    delete iClient;
    if (iView) {
        RemoveFromStack(iView);
        delete iView;
    }
}

/* ------------------------------------------------------------------ */
/* lists                                                               */
/* ------------------------------------------------------------------ */

void CPhantomLinkAppUi::ShowGamesL()
{
    iMode = EGames;
    CDesCArrayFlat *items = new (ELeave) CDesCArrayFlat(GameCount() + 1);
    CleanupStack::PushL(items);
    TBuf<80> line;
    TBuf<72> name;
    for (TInt i = 0; i < GameCount(); i++) {
        name.Copy(TPtrC8((const TUint8 *)GameAt(i).iName).Left(70));
        line.Copy(_L("\t"));
        line.Append(name);
        items->AppendL(line);
    }
    items->AppendL(_L("\tRestore original behavior"));
    CleanupStack::Pop();
    iView->SetListL(items, 0, ETrue);
}

void CPhantomLinkAppUi::ShowBridgeL()
{
    iMode = EBridge;
    CDesCArrayFlat *items = new (ELeave) CDesCArrayFlat(TBridgeSettings::EFieldCount + 1);
    CleanupStack::PushL(items);
    TBuf<120> line;
    line.Copy(_L("\tStatus: "));
    line.Append(iStatus);
    items->AppendL(line);
    for (TInt f = 0; f < TBridgeSettings::EFieldCount; f++) {
        iSettings.ListText(f, line);
        items->AppendL(line);
    }
    CleanupStack::Pop();
    iView->SetListL(items, 0, EFalse);
}

void CPhantomLinkAppUi::ShowBoardL()
{
    iMode = EBoard;
    CDesCArrayFlat *items = new (ELeave) CDesCArrayFlat(3);
    CleanupStack::PushL(items);
    TBuf<200> line;
    if (!iBoard.u.board.count) {
        items->AppendL(_L("\tNo messages"));
    } else {
        line.Copy(_L("\tMessage "));
        line.AppendNum(iBoard.u.board.index + 1);
        line.Append(_L(" of "));
        line.AppendNum(iBoard.u.board.count);
        items->AppendL(line);
        TBuf<GC_MAX_NAME> name;
        TBuf<GC_BOARD_TIME_MAX> time;
        CnvUtfConverter::ConvertToUnicodeFromUtf8(name, TPtrC8((const TUint8 *)iBoard.u.board.name));
        CnvUtfConverter::ConvertToUnicodeFromUtf8(time, TPtrC8((const TUint8 *)iBoard.u.board.time));
        line.Copy(_L("\t"));
        line.Append(name);
        line.Append(_L(" - "));
        line.Append(time);
        items->AppendL(line);
        TBuf<GC_BOARD_TEXT_MAX> text;
        CnvUtfConverter::ConvertToUnicodeFromUtf8(text, TPtrC8((const TUint8 *)iBoard.u.board.text));
        line.Copy(_L("\t"));
        line.Append(text);
        items->AppendL(line);
    }
    CleanupStack::Pop(items);
    iView->SetListL(items, 0, EFalse);
}

void CPhantomLinkAppUi::ReadBoardL(TUint32 aIndex)
{
    TBuf8<GC_MAX_NAME> name;
    if (CnvUtfConverter::ConvertFromUnicodeToUtf8(name, iBoardName) != 0) {
        ErrorL(_L("Name is too long in UTF-8"));
        return;
    }
    iBoardClient->StartL(name, KNullDesC8, aIndex, EFalse);
}

void CPhantomLinkAppUi::PostBoardL()
{
    TBuf<GC_MAX_NAME> name(iBoardName);
    CAknTextQueryDialog *nameDialog = CAknTextQueryDialog::NewL(name);
    nameDialog->SetMaxLength(GC_MAX_NAME);
    if (!nameDialog->ExecuteLD(R_PL_TEXT_QUERY, _L("Your name"))) {
        return;
    }
    if (!name.Length()) {
        ErrorL(_L("Enter a name"));
        return;
    }
    TBuf<GC_BOARD_TEXT_MAX> text(iBoardDraft);
    CAknTextQueryDialog *dialog = CAknTextQueryDialog::NewL(text);
    dialog->SetMaxLength(GC_BOARD_TEXT_MAX);
    if (!dialog->ExecuteLD(R_PL_TEXT_QUERY, _L("Message"))) {
        return;
    }
    if (!text.Length()) {
        ErrorL(_L("Enter a message"));
        return;
    }
    iBoardName.Copy(name);
    iBoardDraft.Copy(text);
    TBuf8<GC_MAX_NAME> name8;
    TBuf8<GC_BOARD_TEXT_MAX> text8;
    if (CnvUtfConverter::ConvertFromUnicodeToUtf8(name8, name) != 0 ||
        CnvUtfConverter::ConvertFromUnicodeToUtf8(text8, text) != 0) {
        ErrorL(_L("Name or message is too long in UTF-8"));
        return;
    }
    iBoardClient->StartL(name8, text8, 0, ETrue);
}

void CPhantomLinkAppUi::RefreshBridgeL()
{
    if (iMode != EBridge) {
        return;
    }
    TBuf<120> line;
    for (TInt f = 0; f < TBridgeSettings::EFieldCount; f++) {
        iSettings.ListText(f, line);
        iView->ReplaceItemL(f + 1, line);
    }
}

void CPhantomLinkAppUi::SetStatusL(const TDesC &aText)
{
    iStatus.Copy(aText.Left(iStatus.MaxLength()));
    if (iMode == EBridge) {
        TBuf<120> line;
        line.Copy(_L("\tStatus: "));
        line.Append(iStatus);
        iView->ReplaceItemL(KStatusLine, line);
    }
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

void CPhantomLinkAppUi::DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenuPane)
{
    if (aResourceId != R_PL_MENU) {
        return;
    }
    TBool games = iMode == EGames;
    TBool bridge = iMode == EBridge;
    TBool board = iMode == EBoard;
    aMenuPane->SetItemDimmed(EPlCmdSelect, EFalse);
    aMenuPane->SetItemDimmed(EPlCmdGames, games);
    aMenuPane->SetItemDimmed(EPlCmdBridge, bridge);
    aMenuPane->SetItemDimmed(EPlCmdRead, !bridge);
    aMenuPane->SetItemDimmed(EPlCmdWrite, !bridge);
    aMenuPane->SetItemDimmed(EPlCmdReboot, !bridge);
    aMenuPane->SetItemDimmed(EPlCmdSetup, !bridge);
    aMenuPane->SetItemDimmed(EPlCmdFactory, !bridge);
    aMenuPane->SetItemDimmed(EPlCmdChoose, !bridge);
    aMenuPane->SetItemDimmed(EPlCmdBoard, board);
    aMenuPane->SetItemDimmed(EPlCmdBoardRefresh, !board);
    aMenuPane->SetItemDimmed(EPlCmdBoardPost, !board);
    aMenuPane->SetItemDimmed(EPlCmdBoardOlder, !board || iBoard.u.board.index + 1 >= iBoard.u.board.count);
    aMenuPane->SetItemDimmed(EPlCmdBoardNewer, !board || iBoard.u.board.index == 0);
}

void CPhantomLinkAppUi::HandleCommandL(TInt aCommand)
{
    switch (aCommand) {
    case EAknSoftkeyExit:
    case EAknCmdExit:
    case EEikCmdExit:
        Exit();
        return;
    default:
        break;
    }

    if (iClient->IsBusy() || iBoardClient->IsBusy()) {
        InfoL(_L("Please wait, talking to the bridge"));
        return;
    }

    switch (aCommand) {
    case EPlCmdSelect:
        if (iMode == EGames) {
            InstallSelectedL();
        } else if (iMode == EBoard) {
            if (iBoard.u.board.count) {
                TBuf<GC_BOARD_TEXT_MAX> text;
                CnvUtfConverter::ConvertToUnicodeFromUtf8(text, TPtrC8((const TUint8 *)iBoard.u.board.text));
                InfoL(text);
            }
        } else if (iView->CurrentIndex() == KStatusLine) {
            StartL(ERead, _L8("show\n"), EFalse);
        } else {
            EditSelectedL();
        }
        break;
    case EPlCmdGames:
        iBoardClient->Close();
        ShowGamesL();
        break;
    case EPlCmdBridge:
        iBoardClient->Close();
        ShowBridgeL();
        break;
    case EPlCmdRead:
        StartL(ERead, _L8("show\n"), EFalse);
        break;
    case EPlCmdWrite:
        WriteSettingsL();
        break;
    case EPlCmdReboot:
        StartL(ERestart, _L8("reboot\n"), EFalse);
        break;
    case EPlCmdSetup:
        if (ConfirmL(_L("Restart the bridge into setup mode?"))) {
            StartL(ERestart, _L8("setup\n"), EFalse);
        }
        break;
    case EPlCmdFactory:
        if (ConfirmL(_L("Erase ALL settings of the bridge?"))) {
            StartL(EFactory, _L8("factory\n"), EFalse);
        }
        break;
    case EPlCmdChoose:
        StartL(ERead, _L8("show\n"), ETrue);
        break;
    case EPlCmdBoard:
    case EPlCmdBoardRefresh:
        ReadBoardL(0);
        break;
    case EPlCmdBoardPost:
        PostBoardL();
        break;
    case EPlCmdBoardOlder:
        if (iBoard.u.board.index + 1 < iBoard.u.board.count) {
            ReadBoardL(iBoard.u.board.index + 1);
        }
        break;
    case EPlCmdBoardNewer:
        if (iBoard.u.board.index > 0) {
            ReadBoardL(iBoard.u.board.index - 1);
        }
        break;
    case EPlCmdAbout:
        ShowAboutL();
        break;
    default:
        break;
    }
}

void CPhantomLinkAppUi::InstallSelectedL()
{
    TInt index = iView->CurrentIndex();
    if (index == GameCount()) {
        if (!ConfirmL(_L("Delete gamecomms.dll from the default target directory?"))) {
            return;
        }
        TInt err = GameInstaller::RemoveDefault();
        if (err == KErrNone) {
            InfoL(_L("gamecomms.dll deleted; original behavior restored"));
        } else if (err == KErrNotFound) {
            InfoL(_L("gamecomms.dll is already absent; original behavior is restored"));
        } else {
            TBuf<160> text;
            DescribeError(err, text);
            ErrorL(text);
        }
        return;
    }
    if (index < 0 || index >= GameCount()) {
        return;
    }
    const TGameEntry &game = GameAt(index);
    TInt err = GameInstaller::Install(game.iVariant, ((CApaApplication *)Application())->AppFullName());
    TBuf<160> text;
    if (err == KErrNone) {
        TBuf<64> name;
        name.Copy(TPtrC8((const TUint8 *)game.iName).Left(60));
        text.Copy(_L("gamecomms.dll for "));
        text.Append(name);
        text.Append(_L(" copied"));
        InfoL(text);
    } else {
        DescribeError(err, text);
        ErrorL(text);
    }
}

void CPhantomLinkAppUi::EditSelectedL()
{
    TInt field = iView->CurrentIndex() - 1;
    if (field < 0 || field >= TBridgeSettings::EFieldCount) {
        return;
    }
    TBuf<TBridgeSettings::EValueLen> value;
    if (!TBridgeSettings::IsSecret(field) || iSettings.iDirty[field]) {
        value.Copy(iSettings.iValue[field]);
    }
    CAknTextQueryDialog *dialog = CAknTextQueryDialog::NewL(value);
    dialog->SetMaxLength(TBridgeSettings::MaxLength(field));
    if (!dialog->ExecuteLD(R_PL_TEXT_QUERY, TBridgeSettings::Label(field))) {
        return;
    }

    TLex lex(value);
    TInt number = 0;
    switch (field) {
    case TBridgeSettings::EPort:
        if (lex.Val(number) != KErrNone || !lex.Eos() || number < 1 || number > 65535) {
            ErrorL(_L("Port must be 1 to 65535"));
            return;
        }
        break;
    case TBridgeSettings::EVerbose:
        if (value.Length() != 1 || (value[0] != '0' && value[0] != '1')) {
            ErrorL(_L("Enter 0 or 1"));
            return;
        }
        break;
    case TBridgeSettings::EBtName:
    case TBridgeSettings::EPin:
        if (value.Length() == 0) {
            ErrorL(_L("This value cannot be empty"));
            return;
        }
        break;
    default:
        break;
    }
    iSettings.Set(field, value);
    RefreshBridgeL();
}

void CPhantomLinkAppUi::WriteSettingsL()
{
    if (!iSettings.HasChanges()) {
        InfoL(_L("No changes to send"));
        return;
    }
    HBufC8 *commands = iSettings.BuildCommandsL(ETrue);
    CleanupStack::PushL(commands);
    if (ConfirmL(_L("Reboot the bridge to apply the settings?"))) {
        commands->Des().Append(_L8("reboot\n")); /* the buffer has room for it */
    }
    StartL(EWrite, *commands, EFalse);
    CleanupStack::PopAndDestroy(); /* commands */
}

void CPhantomLinkAppUi::StartL(TOperation aOperation, const TDesC8 &aCommands, TBool aChooseBridge)
{
    if (iMode != EBridge) {
        ShowBridgeL();
    }
    iOperation = aOperation;
    SetStatusL(_L("connecting..."));
    TRAPD(err, iClient->StartL(aCommands, aChooseBridge));
    if (err != KErrNone) {
        iOperation = ENone;
        TBuf<100> text;
        DescribeError(err, text);
        SetStatusL(text);
        ErrorL(text);
    }
}

/* ------------------------------------------------------------------ */
/* bridge client callbacks                                             */
/* ------------------------------------------------------------------ */

void CPhantomLinkAppUi::BridgeProgress(const TDesC &aText)
{
    PL_IGNORE(SetStatusL(aText));
}

void CPhantomLinkAppUi::BridgeFinished(TInt aError)
{
    TOperation operation = iOperation;
    iOperation = ENone;
    TBuf<100> text;

    if (aError != KErrNone) {
        DescribeError(aError, text);
        PL_IGNORE(SetStatusL(text));
        PL_IGNORE(ErrorL(text));
        return;
    }

    switch (operation) {
    case ERead:
        iSettings.Parse(iClient->Reply());
        if (!iSettings.iLoaded) {
            PL_IGNORE(SetStatusL(_L("unexpected reply")));
            PL_IGNORE(ErrorL(_L("The bridge sent no settings. Is the firmware up to date?")));
            return;
        }
        PL_IGNORE(SetStatusL(_L("settings read")));
        PL_IGNORE(RefreshBridgeL());
        break;
    case EWrite:
        iSettings.ClearChanges();
        PL_IGNORE(SetStatusL(_L("settings saved")));
        PL_IGNORE(RefreshBridgeL());
        PL_IGNORE(InfoL(_L("Settings saved")));
        break;
    case EFactory:
        iSettings = TBridgeSettings();
        PL_IGNORE(SetStatusL(_L("factory reset done")));
        PL_IGNORE(RefreshBridgeL());
        PL_IGNORE(InfoL(_L("Bridge reset to factory settings and restarting")));
        break;
    case ERestart:
        PL_IGNORE(SetStatusL(_L("bridge restarting")));
        PL_IGNORE(InfoL(_L("Bridge restarting")));
        break;
    default:
        break;
    }
}

void CPhantomLinkAppUi::BoardFinished(TInt aError, const gc_msg &aReply)
{
    if (aError != KErrNone) {
        TBuf<100> text;
        if (aReply.type == GC_MT_ERROR) {
            text.Copy(_L("Board: "));
            TBuf<GC_MAX_NAME> detail;
            CnvUtfConverter::ConvertToUnicodeFromUtf8(detail, TPtrC8((const TUint8 *)aReply.u.error.text));
            text.Append(detail);
        } else {
            DescribeError(aError, text);
        }
        PL_IGNORE(ErrorL(text));
        return;
    }
    if (aReply.type == GC_MT_BOARD_POSTED) {
        iBoardDraft.Zero();
        PL_IGNORE(InfoL(_L("Message posted")));
        TRAPD(err, ReadBoardL(0));
        if (err != KErrNone) {
            TBuf<100> text;
            DescribeError(err, text);
            PL_IGNORE(ErrorL(text));
        }
    } else {
        iBoard = aReply;
        PL_IGNORE(ShowBoardL());
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

void CPhantomLinkAppUi::DescribeError(TInt aError, TDes &aText)
{
    switch (aError) {
    case KErrInUse:
        aText.Copy(_L("File in use. Close the game first."));
        break;
    case KErrNotFound:
    case KErrPathNotFound:
        aText.Copy(_L("Not found (memory card in E:? bridge in range?)"));
        break;
    case KErrTimedOut:
        aText.Copy(_L("The bridge did not answer"));
        break;
    case KErrCancel:
        aText.Copy(_L("Cancelled"));
        break;
    case KErrDisconnected:
    case KErrCommsLineFail:
        aText.Copy(_L("Connection to the bridge lost"));
        break;
    case KErrTooBig:
        aText.Copy(_L("A value is too long"));
        break;
    case KErrAccessDenied:
        aText.Copy(_L("Server rejected the access key"));
        break;
    case KErrCouldNotConnect:
        aText.Copy(_L("Server is unavailable"));
        break;
    default:
        aText.Copy(_L("Error "));
        aText.AppendNum(aError);
        break;
    }
}

TBool CPhantomLinkAppUi::ConfirmL(const TDesC &aPrompt)
{
    CAknQueryDialog *dialog = CAknQueryDialog::NewL();
    return dialog->ExecuteLD(R_PL_CONFIRM_QUERY, aPrompt) != 0;
}

void CPhantomLinkAppUi::InfoL(const TDesC &aText)
{
    CAknInformationNote *note = new (ELeave) CAknInformationNote(ETrue);
    note->ExecuteLD(aText);
}

void CPhantomLinkAppUi::ErrorL(const TDesC &aText)
{
    CAknErrorNote *note = new (ELeave) CAknErrorNote(ETrue);
    note->ExecuteLD(aText);
}

void CPhantomLinkAppUi::ShowAboutL()
{
    InfoL(_L("PhantomLink: pick your game to install its gamecomms.dll, or set up the ESP32 bridge over Bluetooth."));
}
