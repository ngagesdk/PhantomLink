/** @file GameInstaller.cpp
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "GameInstaller.h"
#include <bautils.h>
#include <f32file.h>

_LIT(KDefaultTargetDir, "C:\\System\\Libs\\");
_LIT(KTargetName, "gamecomms.dll");
_LIT(KTempName, "gamecomms.pl$");
_LIT8(KPatchMagic, "GCP1");

/* patch format: see dll/variants/gcpatch.py */
const TInt KPatchHeaderSize = 28;
const TInt KPatchRunHeaderSize = 8;
const TInt KMaxDllSize = 512 * 1024;
/* free space that must stay on C: after the copy, so the phone can boot */
const TInt KMinFreeAfterInstall = 128 * 1024;

static TUint32 Get32(const TUint8 *aPtr)
{
    return (TUint32)aPtr[0] | ((TUint32)aPtr[1] << 8) | ((TUint32)aPtr[2] << 16) | ((TUint32)aPtr[3] << 24);
}

static TUint32 Fnv1a(const TDesC8 &aData)
{
    TUint32 hash = 0x811c9dc5u;
    const TUint8 *p = aData.Ptr();
    for (TInt i = 0; i < aData.Length(); i++) {
        hash = (hash ^ p[i]) * 0x01000193u;
    }
    return hash;
}

static void VariantName(TDes &aName, const TDesC &aDllDir, TInt aVariant, const TDesC &aExt)
{
    aName.Copy(aDllDir);
    aName.Append(_L("gcv"));
    aName.AppendNum(aVariant);
    aName.Append(aExt);
}

static HBufC8 *ReadFileLC(RFs &aFs, const TDesC &aName)
{
    RFile file;
    User::LeaveIfError(file.Open(aFs, aName, EFileRead | EFileShareReadersOnly));
    CleanupClosePushL(file);
    TInt size = 0;
    User::LeaveIfError(file.Size(size));
    if (size <= 0 || size > KMaxDllSize) {
        User::Leave(KErrCorrupt);
    }
    HBufC8 *buf = HBufC8::NewLC(size);
    TPtr8 ptr = buf->Des();
    User::LeaveIfError(file.Read(ptr, size));
    if (ptr.Length() != size) {
        User::Leave(KErrCorrupt);
    }
    CleanupStack::Pop(buf);
    CleanupStack::PopAndDestroy(); /* file */
    CleanupStack::PushL(buf);
    return buf;
}

/* Returns the DLL image of the variant: dlls\gcvN.dll if it exists,
 * otherwise the base DLL named in dlls\gcvN.pat with the patch applied. */
static HBufC8 *BuildVariantLC(RFs &aFs, TInt aVariant, const TDesC &aDllDir)
{
    TFileName name;
    VariantName(name, aDllDir, aVariant, _L(".dll"));
    if (BaflUtils::FileExists(aFs, name)) {
        return ReadFileLC(aFs, name);
    }

    VariantName(name, aDllDir, aVariant, _L(".pat"));
    HBufC8 *patch = ReadFileLC(aFs, name);
    const TUint8 *p = patch->Ptr();
    const TInt len = patch->Length();
    if (len < KPatchHeaderSize || patch->Left(4) != KPatchMagic) {
        User::Leave(KErrCorrupt);
    }
    const TUint32 baseVariant = Get32(p + 4);
    const TUint32 baseSize = Get32(p + 8);
    const TUint32 baseHash = Get32(p + 12);
    const TUint32 targetSize = Get32(p + 16);
    const TUint32 targetHash = Get32(p + 20);
    const TUint32 runs = Get32(p + 24);
    if (baseVariant < 1 || baseVariant > 11 || (TInt)baseVariant == aVariant || targetSize == 0 ||
        targetSize > (TUint32)KMaxDllSize) {
        User::Leave(KErrCorrupt);
    }

    VariantName(name, aDllDir, (TInt)baseVariant, _L(".dll"));
    HBufC8 *base = ReadFileLC(aFs, name);
    if ((TUint32)base->Length() != baseSize || Fnv1a(*base) != baseHash) {
        User::Leave(KErrCorrupt);
    }

    HBufC8 *image = HBufC8::NewLC((TInt)targetSize);
    TPtr8 out = image->Des();
    out.Copy(base->Left(Min(base->Length(), (TInt)targetSize)));
    if (out.Length() < (TInt)targetSize) {
        out.AppendFill(0, (TInt)targetSize - out.Length());
    }
    TUint8 *dst = (TUint8 *)out.Ptr();

    TInt pos = KPatchHeaderSize;
    for (TUint32 i = 0; i < runs; i++) {
        if (len - pos < KPatchRunHeaderSize) {
            User::Leave(KErrCorrupt);
        }
        const TUint32 off = Get32(p + pos);
        const TUint32 n = Get32(p + pos + 4);
        pos += KPatchRunHeaderSize;
        if (n > (TUint32)(len - pos) || off > targetSize || n > targetSize - off) {
            User::Leave(KErrCorrupt);
        }
        if (n > 0) {
            Mem::Copy(dst + off, p + pos, (TInt)n);
        }
        pos += (TInt)n;
    }
    if (pos != len || Fnv1a(out) != targetHash) {
        User::Leave(KErrCorrupt);
    }

    CleanupStack::Pop(image);
    CleanupStack::PopAndDestroy(2); /* base, patch */
    CleanupStack::PushL(image);
    return image;
}

static TInt WriteFile(RFs &aFs, const TDesC &aName, const TDesC8 &aData)
{
    RFile file;
    TInt err = file.Replace(aFs, aName, EFileWrite | EFileShareExclusive);
    if (err != KErrNone) {
        return err;
    }
    err = file.Write(aData);
    if (err == KErrNone) {
        err = file.Flush();
    }
    file.Close();
    return err;
}

static TInt DoInstallL(RFs &aFs, TInt aVariant, const TDesC &aAppFullName)
{
    TParsePtrC app(aAppFullName);
    TFileName dllDir;
    dllDir.Copy(app.DriveAndPath());
    dllDir.Append(_L("dlls\\"));

    HBufC8 *image = BuildVariantLC(aFs, aVariant, dllDir);

    TFileName targetDir;
    targetDir.Copy(KDefaultTargetDir);
    TFileName targetFile(targetDir);
    targetFile.Append(KTargetName);
    TFileName tempFile(targetDir);
    tempFile.Append(KTempName);

    /* the old DLL is still there while the new one is written */
    TVolumeInfo volume;
    TInt err = aFs.Volume(volume, EDriveC);
    if (err == KErrNone && volume.iFree < TInt64(image->Length() + KMinFreeAfterInstall)) {
        err = KErrDiskFull;
    }
    if (err == KErrNone) {
        BaflUtils::EnsurePathExistsL(aFs, targetDir);
        /* write a temporary file first: a half written DLL would crash the game */
        err = WriteFile(aFs, tempFile, *image);
    }
    if (err == KErrNone) {
        TEntry entry;
        err = aFs.Entry(tempFile, entry);
        if (err == KErrNone && entry.iSize != image->Length()) {
            err = KErrCorrupt;
        }
    }
    if (err == KErrNone) {
        err = aFs.Delete(targetFile); /* KErrInUse while the game runs */
        if (err == KErrNotFound) {
            err = KErrNone;
        }
    }
    if (err == KErrNone) {
        err = aFs.Rename(tempFile, targetFile);
    }
    if (err != KErrNone) {
        aFs.Delete(tempFile);
    }

    CleanupStack::PopAndDestroy(image);
    return err;
}

TInt GameInstaller::Install(TInt aVariant, const TDesC &aAppFullName)
{
    if (aVariant < 1 || aVariant > 11) {
        return KErrArgument;
    }
    RFs fs;
    TInt err = fs.Connect();
    if (err != KErrNone) {
        return err;
    }
    TInt result = KErrNone;
    TRAP(err, result = DoInstallL(fs, aVariant, aAppFullName));
    fs.Close();
    return err != KErrNone ? err : result;
}

TInt GameInstaller::RemoveDefault()
{
    RFs fs;
    TInt err = fs.Connect();
    if (err != KErrNone) {
        return err;
    }

    TFileName targetFile;
    targetFile.Copy(KDefaultTargetDir);
    targetFile.Append(KTargetName);
    err = fs.Delete(targetFile);
    fs.Close();
    return err;
}
