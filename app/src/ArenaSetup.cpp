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
#include <es_sock.h>
#include <f32file.h>
#include <in_sock.h>

_LIT(KApName, "PhantomLink");
_LIT(KBtPort, "BTCOMM::0");
_LIT(KBtCsy, "BTCOMM");
_LIT(KBtTsy, "PHONETSY");
_LIT(KSaveFile, "C:\\System\\Data\\PlModem.txt"); /* old modem settings */
_LIT(KHostsFile, "C:\\System\\Data\\hosts");
_LIT8(KHostsTag, " # PhantomLink Arena");

/* the Arena back ends named in the games' arena.amr (read only on the
 * cartridge): Pocket Kingdom, then Rifts: Promise of Power */
static const char *const KArenaHosts[] = {
    "arena.n-gage.com",
    "im.prod.pk.nokia.sf.yav4.com",
    "snap.prod.pk.nokia.sf.yav4.com",
    "im01.rifts.bb.sf.yav4.com",
    "bs01.rifts.bb.sf.yav4.com",
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

/* ---- Arena over GPRS ---- */

/* Strict dotted IPv4 ("a.b.c.d", each 0..255). */
static TBool ParseIp4(const TDesC8 &aText, TUint32 &aAddr)
{
    TUint32 addr = 0;
    TInt part = 0, digits = 0, value = 0, i;
    for (i = 0; i <= aText.Length(); i++) {
        TUint c = i < aText.Length() ? aText[i] : '.';
        if (c >= '0' && c <= '9') {
            value = value * 10 + (TInt)(c - '0');
            if (++digits > 3 || value > 255) {
                return EFalse;
            }
        } else if (c == '.' && digits) {
            addr = (addr << 8) | (TUint32)value;
            part++;
            digits = 0;
            value = 0;
        } else {
            return EFalse;
        }
    }
    if (part != 4) {
        return EFalse;
    }
    aAddr = addr;
    return ETrue;
}

/* Not loopback, private (RFC 1918), carrier NAT (RFC 6598), link local
 * or 0.0.0.0/8. */
static TBool IsPublicIp4(TUint32 a)
{
    return (a >> 24) != 0 && (a >> 24) != 127 && (a >> 24) != 10 && (a >> 20) != 0xac1 && (a >> 16) != 0xc0a8 &&
           (a >> 22) != ((100u << 2) | 1u) && (a >> 16) != 0xa9fe;
}

static void FormatIp4(TUint32 aAddr, TDes8 &aOut)
{
    aOut.Zero();
    aOut.AppendNum((TInt)((aAddr >> 24) & 0xff));
    aOut.Append('.');
    aOut.AppendNum((TInt)((aAddr >> 16) & 0xff));
    aOut.Append('.');
    aOut.AppendNum((TInt)((aAddr >> 8) & 0xff));
    aOut.Append('.');
    aOut.AppendNum((TInt)(aAddr & 0xff));
}

/* Lists the packet data access points (they are the operator's, made by
 * the user or by the operator's settings message). */
static void ReportGprsApsL(TDes &aLog)
{
    CCommsDatabase *db = CCommsDatabase::NewL(EDatabaseTypeIAP);
    CleanupStack::PushL(db);
    CCommsDbTableView *view = db->OpenTableLC(TPtrC(OUTGOING_GPRS));
    TBuf<80> line(_L("GPRS access points: "));
    TBuf<KCommsDbSvrMaxColumnNameLength> name;
    TInt count = 0;
    TInt err = view->GotoFirstRecord();
    while (err == KErrNone) {
        TRAPD(rerr, view->ReadTextL(TPtrC(COMMDB_NAME), name));
        if (rerr == KErrNone) {
            if (count && line.Length() + 2 <= line.MaxLength()) {
                line.Append(_L(", "));
            }
            line.Append(name.Left(line.MaxLength() - line.Length()));
            count++;
        }
        err = view->GotoNextRecord();
    }
    CleanupStack::PopAndDestroy(2); /* view, db */
    if (count) {
        Report(aLog, line, KErrNone);
        Report(aLog, _L("Choose one of them in the game"), KErrNone);
    } else {
        Report(aLog, _L("No GPRS access point: create one (Settings > Connection > Access points, data bearer Packet "
                        "data, your operator's APN)"),
               KErrNone);
    }
}

/* Looks up aName; KErrNone and aAddr on success. */
static TInt ResolveIp4(RSocketServ &aSs, const TDesC &aName, TUint32 &aAddr)
{
    RHostResolver res;
    TNameEntry entry;
    TInt err = res.Open(aSs, KAfInet, KProtocolInetUdp);
    if (err != KErrNone) {
        return err;
    }
    err = res.GetByName(aName, entry);
    res.Close();
    if (err == KErrNone) {
        aAddr = TInetAddr::Cast(entry().iAddr).Address();
    }
    return err;
}

/* True if the hosts file maps every Arena name to aAddr already. */
static TBool HostsInPlaceL(const TDesC8 &aAddr)
{
    RFs fs;
    RFile f;
    TInt size = 0, i;
    TBool all = ETrue;
    User::LeaveIfError(fs.Connect());
    CleanupClosePushL(fs);
    if (f.Open(fs, KHostsFile, EFileRead) != KErrNone) {
        CleanupStack::PopAndDestroy(); /* fs */
        return EFalse;
    }
    CleanupClosePushL(f);
    f.Size(size);
    HBufC8 *buf = HBufC8::NewLC(size);
    TPtr8 p = buf->Des();
    User::LeaveIfError(f.Read(p));
    for (i = 0; i < KArenaHostCount && all; i++) {
        TBuf8<96> line;
        line.Copy(aAddr.Left(15));
        line.Append(' ');
        line.Append(TPtrC8((const TUint8 *)KArenaHosts[i]));
        line.Append(KHostsTag);
        all = p.Find(line) >= 0;
    }
    CleanupStack::PopAndDestroy(3); /* buf, f, fs */
    return all;
}

void ArenaSetup::EnableGprsL(const TDesC8 &aServer, TDes &aLog)
{
    TUint32 ip = 0;
    TBuf8<16> addr;
    TBool resolved = EFalse;
    RSocketServ ss;

    if (aServer.Length() == 0) {
        User::Leave(KErrArgument);
    }
    TRAPD(aerr, ReportGprsApsL(aLog));
    if (aerr != KErrNone) {
        Report(aLog, _L("cannot read the access points"), aerr);
    }

    User::LeaveIfError(ss.Connect());
    CleanupClosePushL(ss);
    if (!ParseIp4(aServer, ip)) {
        /* a host name: look it up now (this opens a data connection) */
        TBuf<64> name;
        name.Copy(aServer.Left(name.MaxLength()));
        TInt err = ResolveIp4(ss, name, ip);
        if (err != KErrNone) {
            Report(aLog, _L("Server name not found"), err);
            User::Leave(err);
        }
        resolved = ETrue;
    }
    if (!IsPublicIp4(ip)) {
        /* unreachable from the operator's network (unless the operator
         * runs the server) */
        Report(aLog, _L("Warning: server address is not public"), KErrNone);
    }
    FormatIp4(ip, addr);
    {
        TBuf<40> line(_L("Server: "));
        TBuf<16> a16;
        a16.Copy(addr);
        line.Append(a16);
        if (resolved) {
            line.Append(_L(" (looked up)"));
        }
        Report(aLog, line, KErrNone);
    }

    TBool inPlace = HostsInPlaceL(addr);
    WriteHostsL(addr, aLog);
    if (!inPlace) {
        /* hosts is read at start-up: nothing to check before a restart */
        Report(aLog, _L("Then choose Arena over GPRS again to check"), KErrNone);
    } else {
        /* the entries were there before: they are in force if the N-Gage
         * was restarted since.  Without them, the game would send the
         * account password to whoever has the old Arena names today. */
        TUint32 got = 0;
        TInt err = ResolveIp4(ss, _L("arena.n-gage.com"), got);
        if (err == KErrNone && got == ip) {
            Report(aLog, _L("Arena names: OK, ready to play"), KErrNone);
        } else {
            TBuf8<16> g8;
            TBuf<80> line(_L("Arena names NOT redirected"));
            if (err == KErrNone) {
                FormatIp4(got, g8);
                TBuf<16> g16;
                g16.Copy(g8);
                line.Append(_L(" ("));
                line.Append(g16);
                line.Append(')');
            }
            Report(aLog, line, err);
            Report(aLog, _L("Do not log in to the Arena; restart the N-Gage first"), KErrNone);
        }
    }
    CleanupStack::PopAndDestroy(); /* ss */
}
