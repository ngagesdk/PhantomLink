/** @file ArenaSetup.cpp
 *
 *  See ArenaSetup.h.  Written from the documented gnubox procedure for
 *  Symbian 6.1 (Nokia 7650 / N-Gage), not from its source.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "ArenaSetup.h"
#include <btregistry.h>
#include <cdbcols.h>
#include <commdb.h>
#include <f32file.h>

_LIT(KApName, "PhantomLink");
_LIT(KBtPort, "BTCOMM::0");
_LIT(KBtCsy, "BTCOMM");
_LIT(KBtTsy, "PHONETSY");
_LIT(KSaveFile, "C:\\System\\Data\\PlModem.txt"); /* old modem settings */
_LIT(KHostsFile, "C:\\System\\Data\\hosts");
_LIT8(KHostsTag, " # PhantomLink Arena");

/* the Arena back ends named in arena.amr (read only on the cartridge) */
static const char *const KArenaHosts[] = {
    "arena.n-gage.com",
    "im.prod.pk.nokia.sf.yav4.com",
    "snap.prod.pk.nokia.sf.yav4.com",
    "ngage-auth",
};
const TInt KArenaHostCount = sizeof(KArenaHosts) / sizeof(KArenaHosts[0]);

static void Report(TDes &aLog, const TDesC &aText, TInt aErr)
{
    if (aLog.Length() + aText.Length() + 16 > aLog.MaxLength()) {
        return;
    }
    aLog.Append(aText);
    if (aErr != KErrNone) {
        aLog.AppendFormat(_L(" (%d)"), aErr);
    }
    aLog.Append('\n');
}

/* Columns that the firmware may lack are written under TRAP. */
static void TryText(CCommsDbTableView &aView, const TText *aCol, const TDesC &aValue, TDes &aLog)
{
    TRAPD(err, aView.WriteTextL(TPtrC(aCol), aValue));
    if (err != KErrNone) {
        Report(aLog, TPtrC(aCol), err);
    }
}

static void TryBool(CCommsDbTableView &aView, const TText *aCol, TBool aValue, TDes &aLog)
{
    TRAPD(err, aView.WriteBoolL(TPtrC(aCol), aValue));
    if (err != KErrNone) {
        Report(aLog, TPtrC(aCol), err);
    }
}

/* Positions aView on the record called aName. */
static TBool FindNamedL(CCommsDbTableView &aView, const TDesC &aName)
{
    TBuf<KCommsDbSvrMaxColumnNameLength> name;
    TInt err = aView.GotoFirstRecord();
    while (err == KErrNone) {
        aView.ReadTextL(TPtrC(COMMDB_NAME), name);
        if (name.CompareF(aName) == 0) {
            return ETrue;
        }
        err = aView.GotoNextRecord();
    }
    return EFalse;
}

/* The data call modem.  On 6.1 every data call access point uses the
 * modem named by the global setting "Modem" (IAP_MODEM in the 6.1
 * cdbcols.h; later releases renamed it ModemForDataAndFax). */
static void OpenDataModemLC(CCommsDatabase &aDb, CCommsDbTableView *&aView, TDes &aLog)
{
    TUint32 id = 0;
    TRAPD(err, aDb.GetGlobalSettingL(TPtrC(IAP_MODEM), id));
    if (err == KErrNone) {
        aView = aDb.OpenViewMatchingUintLC(TPtrC(MODEM), TPtrC(COMMDB_ID), id);
    } else {
        Report(aLog, _L("no global data modem, using the first"), err);
        aView = aDb.OpenTableLC(TPtrC(MODEM));
    }
    User::LeaveIfError(aView->GotoFirstRecord());
}

static void SaveModemL(CCommsDbTableView &aModem)
{
    RFs fs;
    RFile f;
    TBuf<KCommsDbSvrMaxFieldLength> port, csy, tsy;
    TBuf8<3 * KCommsDbSvrMaxFieldLength + 8> line;

    aModem.ReadTextL(TPtrC(MODEM_PORT_NAME), port);
    aModem.ReadTextL(TPtrC(MODEM_CSY_NAME), csy);
    aModem.ReadTextL(TPtrC(MODEM_TSY_NAME), tsy);
    if (csy.CompareF(KBtCsy) == 0) {
        return; /* already ours: keep the original backup */
    }
    line.Copy(port);
    line.Append('\n');
    line.Append(csy);
    line.Append('\n');
    line.Append(tsy);
    line.Append('\n');
    User::LeaveIfError(fs.Connect());
    CleanupClosePushL(fs);
    User::LeaveIfError(f.Replace(fs, KSaveFile, EFileWrite));
    CleanupClosePushL(f);
    User::LeaveIfError(f.Write(line));
    CleanupStack::PopAndDestroy(2);
}

void ArenaSetup::EnableL(const TBTDevAddr &aBridge, TDes &aLog)
{
    CCommsDatabase *db = CCommsDatabase::NewL(EDatabaseTypeIAP);
    CleanupStack::PushL(db);
    db->ShowHiddenRecords();
    User::LeaveIfError(db->BeginTransaction());

    /* 1. the ISP record of the access point: direct connection */
    CCommsDbTableView *isp = db->OpenTableLC(TPtrC(DIAL_OUT_ISP));
    if (!FindNamedL(*isp, KApName)) {
        db->RollbackTransaction();
        User::Leave(KErrNotFound);
    }
    User::LeaveIfError(isp->UpdateRecord());
    isp->WriteTextL(TPtrC(ISP_DEFAULT_TEL_NUM), KNullDesC); /* no dialling: PPP at once */
    TryBool(*isp, ISP_USE_LOGIN_SCRIPT, EFalse, aLog);
    TryBool(*isp, ISP_IF_PROMPT_FOR_AUTH, EFalse, aLog);
    TryBool(*isp, ISP_IF_CALLBACK_ENABLED, EFalse, aLog);
    TryBool(*isp, ISP_IP_ADDR_FROM_SERVER, ETrue, aLog);
    TryBool(*isp, ISP_IP_DNS_ADDR_FROM_SERVER, ETrue, aLog);
    TryBool(*isp, ISP_ENABLE_SW_COMP, EFalse, aLog);
    TryBool(*isp, ISP_ENABLE_IP_HEADER_COMP, EFalse, aLog);
    User::LeaveIfError(isp->PutRecordChanges());
    CleanupStack::PopAndDestroy(isp);
    Report(aLog, _L("ISP: direct PPP"), KErrNone);

    /* 2. the data modem: Bluetooth serial port */
    CCommsDbTableView *modem = NULL;
    OpenDataModemLC(*db, modem, aLog);
    SaveModemL(*modem);
    User::LeaveIfError(modem->UpdateRecord());
    modem->WriteTextL(TPtrC(MODEM_PORT_NAME), KBtPort);
    modem->WriteTextL(TPtrC(MODEM_CSY_NAME), KBtCsy);
    TryText(*modem, MODEM_TSY_NAME, KBtTsy, aLog);
    User::LeaveIfError(modem->PutRecordChanges());
    CleanupStack::PopAndDestroy(modem);
    Report(aLog, _L("Modem: BTCOMM::0"), KErrNone);

    User::LeaveIfError(db->CommitTransaction());
    CleanupStack::PopAndDestroy(db);

    /* 3. BTCOMM::0's remote device: the bridge, serial port service. */
    if (aBridge == TBTDevAddr()) {
        Report(aLog, _L("No bridge yet: choose a bridge first"), KErrNotFound);
        return;
    }
    {
        TBTCommPortSettings port(_L("Serial Port"), TUUID(0x1101), 0, KBTDefaultServiceSecurityNone, aBridge);
        CBTRegistry *reg = CBTRegistry::NewLC();
        TInt err = reg->SetDefaultCommPort(port);
        CleanupStack::PopAndDestroy(reg);
        TBuf<40> line(_L("BTCOMM::0 -> "));
        for (TInt i = 0; i < 6; i++) {
            line.AppendNumFixedWidth(aBridge[i], EHex, 2);
        }
        Report(aLog, line, err);
    }
}

void ArenaSetup::DisableL(TDes &aLog)
{
    RFs fs;
    RFile f;
    TBuf8<3 * KCommsDbSvrMaxFieldLength + 8> data;
    TBuf<KCommsDbSvrMaxFieldLength> field[3];
    TInt i, n = 0, start = 0;

    User::LeaveIfError(fs.Connect());
    CleanupClosePushL(fs);
    TInt err = f.Open(fs, KSaveFile, EFileRead);
    if (err == KErrNone) {
        f.Read(data);
        f.Close();
    }
    if (err != KErrNone) {
        Report(aLog, _L("no saved modem settings"), err);
    } else {
        for (i = 0; i < data.Length() && n < 3; i++) {
            if (data[i] == '\n') {
                field[n++].Copy(data.Mid(start, i - start));
                start = i + 1;
            }
        }
        if (n == 3) {
            CCommsDatabase *db = CCommsDatabase::NewL(EDatabaseTypeIAP);
            CleanupStack::PushL(db);
            db->ShowHiddenRecords();
            CCommsDbTableView *modem = NULL;
            OpenDataModemLC(*db, modem, aLog);
            User::LeaveIfError(modem->UpdateRecord());
            modem->WriteTextL(TPtrC(MODEM_PORT_NAME), field[0]);
            modem->WriteTextL(TPtrC(MODEM_CSY_NAME), field[1]);
            TryText(*modem, MODEM_TSY_NAME, field[2], aLog);
            User::LeaveIfError(modem->PutRecordChanges());
            CleanupStack::PopAndDestroy(2);
            fs.Delete(KSaveFile);
            Report(aLog, _L("Modem restored"), KErrNone);
        }
    }
    CleanupStack::PopAndDestroy(); /* fs */
    RemoveHostsL(aLog);
}

/* Reads the hosts file without our lines into aOut. */
static void ReadForeignHostsL(RFs &aFs, HBufC8 *&aOut)
{
    RFile f;
    TInt size = 0;
    aOut = NULL;
    if (f.Open(aFs, KHostsFile, EFileRead) != KErrNone) {
        aOut = HBufC8::NewL(512);
        return;
    }
    CleanupClosePushL(f);
    f.Size(size);
    HBufC8 *in = HBufC8::NewLC(size);
    TPtr8 ip = in->Des();
    User::LeaveIfError(f.Read(ip));
    aOut = HBufC8::NewL(size + 512);
    TPtr8 out = aOut->Des();
    TPtrC8 rest(ip);
    while (rest.Length()) {
        TInt nl = rest.Locate('\n');
        TPtrC8 line = nl < 0 ? rest : rest.Left(nl + 1);
        if (line.Find(KHostsTag) < 0) {
            out.Append(line);
        }
        rest.Set(nl < 0 ? TPtrC8() : rest.Mid(nl + 1));
    }
    CleanupStack::PopAndDestroy(2); /* in, f */
}

static void WriteFileL(RFs &aFs, const TDesC8 &aData)
{
    RFile f;
    aFs.MkDirAll(KHostsFile);
    User::LeaveIfError(f.Replace(aFs, KHostsFile, EFileWrite));
    CleanupClosePushL(f);
    User::LeaveIfError(f.Write(aData));
    CleanupStack::PopAndDestroy();
}

void ArenaSetup::WriteHostsL(const TDesC8 &aAddr, TDes &aLog)
{
    RFs fs;
    HBufC8 *buf = NULL;
    TInt i;

    User::LeaveIfError(fs.Connect());
    CleanupClosePushL(fs);
    ReadForeignHostsL(fs, buf);
    CleanupStack::PushL(buf);
    TPtr8 p = buf->Des();
    for (i = 0; i < KArenaHostCount; i++) {
        TPtrC8 host((const TUint8 *)KArenaHosts[i]);
        p.Append(aAddr.Left(15));
        p.Append(' ');
        p.Append(host);
        p.Append(KHostsTag);
        p.Append(_L8("\r\n"));
    }
    WriteFileL(fs, p);
    CleanupStack::PopAndDestroy(2);
    Report(aLog, _L("hosts written (restart the N-Gage)"), KErrNone);
}

void ArenaSetup::RemoveHostsL(TDes &aLog)
{
    RFs fs;
    HBufC8 *buf = NULL;
    User::LeaveIfError(fs.Connect());
    CleanupClosePushL(fs);
    ReadForeignHostsL(fs, buf);
    CleanupStack::PushL(buf);
    if (buf->Length()) {
        WriteFileL(fs, *buf);
    } else {
        fs.Delete(KHostsFile);
    }
    CleanupStack::PopAndDestroy(2);
    Report(aLog, _L("hosts entries removed"), KErrNone);
}
