/** @file GcConfig.cpp
 *
 *  Minimal INI reader/writer (no estlib dependency).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "GcConfig.h"
#include <f32file.h>

/* The only settings file.  Never on E:: retail game cards are read only,
 * and two files would make the settings depend on which one exists. */
_LIT(KIniPhone, "C:\\System\\Data\\GameComms.ini");
const TInt KMaxIniSize = 2048;

TGcConfig::TGcConfig()
    : iAutoJoin(EFalse), iLog(EFalse), iReleaseBridge(ETrue), iFollowBtName(ETrue), iHasBridge(EFalse), iChannel(0)
{
}

static TInt HexVal(TUint8 c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static TBool ParseAddr(const TDesC8 &aText, TBTDevAddr &aAddr)
{
    TUint8 b[6];
    TInt n = 0, hi = -1, i;
    for (i = 0; i < aText.Length() && n < 6; i++) {
        TInt v = HexVal(aText[i]);
        if (v < 0) {
            continue; /* allow ':' and '-' separators */
        }
        if (hi < 0) {
            hi = v;
        } else {
            b[n++] = (TUint8)((hi << 4) | v);
            hi = -1;
        }
    }
    if (n != 6) {
        return EFalse;
    }
    aAddr = TBTDevAddr(TPtrC8(b, 6));
    /* 000000000000 is no address (left by a failed selection): ask again */
    return aAddr != TBTDevAddr();
}

static TInt ToInt(const TDesC8 &aText)
{
    TLex8 lex(aText);
    TInt v = 0;
    lex.Val(v);
    return v;
}

static void Trim(TPtrC8 &aText)
{
    TInt s = 0, e = aText.Length();
    while (s < e && (aText[s] == ' ' || aText[s] == '\t' || aText[s] == '"')) {
        s++;
    }
    while (e > s && (aText[e - 1] == ' ' || aText[e - 1] == '\t' || aText[e - 1] == '\r' || aText[e - 1] == '"')) {
        e--;
    }
    aText.Set(aText.Mid(s, e - s));
}

void TGcConfig::Load()
{
    RFs fs;
    RFile file;
    if (fs.Connect() != KErrNone) {
        return;
    }
    iPath = KIniPhone;
    if (file.Open(fs, iPath, EFileRead | EFileShareReadersOnly) != KErrNone) {
        iPath.Zero();
        fs.Close();
        return;
    }
    HBufC8 *heap = HBufC8::New(KMaxIniSize); /* keep the game's stack small */
    if (!heap) {
        file.Close();
        fs.Close();
        return;
    }
    TPtr8 buf = heap->Des();
    file.Read(buf);
    file.Close();
    fs.Close();

    TPtrC8 rest(buf);
    while (rest.Length() > 0) {
        TInt nl = rest.Locate('\n');
        TPtrC8 line = nl < 0 ? rest : rest.Left(nl);
        rest.Set(nl < 0 ? TPtrC8() : rest.Mid(nl + 1));
        Trim(line);
        if (line.Length() == 0 || line[0] == ';' || line[0] == '#' || line[0] == '[') {
            continue;
        }
        TInt eq = line.Locate('=');
        if (eq < 0) {
            continue;
        }
        TPtrC8 key = line.Left(eq);
        TPtrC8 val = line.Mid(eq + 1);
        Trim(key);
        Trim(val);
        if (key.CompareF(_L8("DeviceName")) == 0) {
            iDeviceName = val.Left(32);
        } else if (key.CompareF(_L8("AutoJoin")) == 0) {
            iAutoJoin = ToInt(val) != 0;
        } else if (key.CompareF(_L8("Log")) == 0) {
            iLog = ToInt(val) != 0;
        } else if (key.CompareF(_L8("ReleaseBridge")) == 0) {
            iReleaseBridge = ToInt(val) != 0;
        } else if (key.CompareF(_L8("FollowBtName")) == 0) {
            iFollowBtName = ToInt(val) != 0;
        } else if (key.CompareF(_L8("Address")) == 0) {
            iHasBridge = ParseAddr(val, iBridge);
        } else if (key.CompareF(_L8("Channel")) == 0) {
            iChannel = ToInt(val);
        } else if (key.CompareF(_L8("ArenaServer")) == 0) {
            iArenaServer = val.Left(iArenaServer.MaxLength());
        }
    }
    delete heap;
}

void TGcConfig::SaveBridgeAddress(const TBTDevAddr &aAddr)
{
    TBuf8<12> hex;
    TInt i;
    for (i = 0; i < 6; i++) {
        hex.AppendNumFixedWidth(aAddr[i], EHex, 2);
    }
    SetValue(_L8("[Bridge]"), _L8("Address"), hex);
    iBridge = aAddr; /* used for this session even if C: is full */
    iHasBridge = ETrue;
}

const TDesC &TGcConfig::WritePath()
{
    return KIniPhone;
}

/* writes the whole settings file to C:, creating the folder if needed */
static TInt WriteIni(RFs &aFs, const TDesC8 &aContent)
{
    RFile file;
    TParsePtrC parse(KIniPhone);
    TInt err = aFs.MkDirAll(parse.DriveAndPath());
    if (err == KErrAlreadyExists) {
        err = KErrNone;
    }
    if (err == KErrNone) {
        err = file.Replace(aFs, KIniPhone, EFileWrite);
    }
    if (err == KErrNone) {
        err = file.Write(aContent);
        file.Close();
    }
    return err;
}

static TBool KeyIs(const TPtrC8 &aLine, const TDesC8 &aKey)
{
    TInt eq = aLine.Locate('=');
    if (eq < 0) {
        return EFalse;
    }
    TPtrC8 key = aLine.Left(eq);
    Trim(key);
    return key.CompareF(aKey) == 0;
}

TInt TGcConfig::SetValue(const TDesC8 &aSection, const TDesC8 &aKey, const TDesC8 &aValue)
{
    /* Rewrites the file, keeping every other line, comments included.
     * An empty value removes the key. */
    RFs fs;
    RFile file;
    TInt err = fs.Connect();
    if (err != KErrNone) {
        return err;
    }
    HBufC8 *inBuf = HBufC8::New(KMaxIniSize);
    HBufC8 *outBuf = HBufC8::New(KMaxIniSize + 256);
    if (!inBuf || !outBuf) {
        delete inBuf;
        delete outBuf;
        fs.Close();
        return KErrNoMemory;
    }
    TPtr8 in = inBuf->Des();
    TPtr8 out = outBuf->Des();
    if (file.Open(fs, KIniPhone, EFileRead | EFileShareReadersOnly) == KErrNone) {
        file.Read(in);
        file.Close();
    }

    TBuf8<80> newLine;
    if (aValue.Length()) {
        newLine.Append(aKey.Left(32));
        newLine.Append('=');
        newLine.Append(aValue.Left(40));
        newLine.Append(_L8("\r\n"));
    }
    TPtrC8 rest(in);
    TBool inSection = EFalse, written = aValue.Length() == 0;
    while (rest.Length() > 0) {
        TInt nl = rest.Locate('\n');
        TPtrC8 line = nl < 0 ? rest : rest.Left(nl + 1);
        rest.Set(nl < 0 ? TPtrC8() : rest.Mid(nl + 1));
        TPtrC8 t = line;
        Trim(t);
        if (t.Length() && t[0] == '[') {
            if (inSection && !written) {
                out.Append(newLine);
                written = ETrue;
            }
            inSection = t.CompareF(aSection) == 0;
        } else if (inSection && t.Length() && t[0] != ';' && t[0] != '#' && KeyIs(t, aKey)) {
            continue; /* replaced */
        }
        /* room for the line, its line break and the new section + key */
        if (out.Length() + line.Length() + 2 + newLine.Length() + aSection.Length() + 4 <= out.MaxLength()) {
            out.Append(line);
            if (nl < 0) {
                out.Append(_L8("\r\n")); /* last line had no line break */
            }
        }
    }
    if (!written) {
        if (!inSection) {
            out.Append(_L8("\r\n"));
            out.Append(aSection);
            out.Append(_L8("\r\n"));
        }
        out.Append(newLine);
    }

    err = WriteIni(fs, out);
    if (err == KErrNone) {
        iPath = KIniPhone;
    }
    fs.Close();
    delete inBuf;
    delete outBuf;
    return err;
}
