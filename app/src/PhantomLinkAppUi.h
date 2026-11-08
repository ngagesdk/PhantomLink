/** @file PhantomLinkAppUi.h
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef PHANTOMLINKAPPUI_H
#define PHANTOMLINKAPPUI_H

#include "BoardClient.h"
#include "BridgeClient.h"
#include "GcConfig.h"
#include <aknappui.h>

class CPhantomLinkView;

class CPhantomLinkAppUi : public CAknAppUi, public MBridgeClientObserver, public MBoardObserver
{
  public:
    void ConstructL();
    CPhantomLinkAppUi();
    ~CPhantomLinkAppUi();

  public: // from CEikAppUi
    void HandleCommandL(TInt aCommand);
    void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane *aMenuPane);

  public: // from MBridgeClientObserver
    void BridgeFinished(TInt aError);
    void BridgeProgress(const TDesC &aText);
    void BoardFinished(TInt aError, const gc_msg &aReply);

  private:
    enum TMode
    {
        EGames,
        EBridge,
        EBoard,
        EComms
    };
    /* rows of the GameComms settings page (GameComms.ini) */
    enum TCommsRow
    {
        ECommsFile,
        ECommsName,
        ECommsAutoJoin,
        ECommsLog,
        ECommsRelease,
        ECommsFollow,
        ECommsForgetBridge, /* one click removes the [Bridge] section */
        ECommsRowCount
    };
    enum TOperation
    {
        ENone,
        ERead,
        EWrite,
        ERestart,
        EFactory
    };

    void ShowGamesL();
    void ShowBridgeL();
    void ShowBoardL();
    void ShowCommsL(TInt aCurrent);
    void CommsText(TInt aRow, TDes &aText) const;
    void EditCommsL();
    void ResetCommsL();
    void ForgetBridgeL();
    void ReadBoardL(TUint32 aIndex);
    void PostBoardL();
    void RefreshBridgeL();
    void SetStatusL(const TDesC &aText);
    void InstallSelectedL();
    void EditSelectedL();
    void StartL(TOperation aOperation, const TDesC8 &aCommands, TBool aChooseBridge);
    void WriteSettingsL();
    void ShowAboutL();
    TBool ConfirmL(const TDesC &aPrompt);
    void InfoL(const TDesC &aText);
    void ErrorL(const TDesC &aText);
    void DescribeError(TInt aError, TDes &aText);

    CPhantomLinkView *iView;
    CBridgeClient *iClient;
    CBoardClient *iBoardClient;
    gc_msg iBoard;
    TBuf<GC_MAX_NAME> iBoardName;
    TBuf<GC_BOARD_TEXT_MAX> iBoardDraft;
    TBridgeSettings iSettings;
    TGcConfig iComms; /* GameComms.ini as last read */
    TMode iMode;
    TOperation iOperation;
    TBuf<80> iStatus;
};

#endif /* PHANTOMLINKAPPUI_H */
