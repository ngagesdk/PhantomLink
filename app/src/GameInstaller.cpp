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

static TInt DoInstallL(RFs &aFs, TInt aVariant, const TDesC &aAppFullName)
{
    TFileName targetDir;
    targetDir.Copy(KDefaultTargetDir);
    TFileName targetFile(targetDir);
    targetFile.Append(KTargetName);

    TParsePtrC app(aAppFullName);
    TFileName src;
    src.Copy(app.DriveAndPath());
    src.Append(_L("dlls\\gcv"));
    src.AppendNum(aVariant);
    src.Append(_L(".dll"));

    TEntry srcEntry;
    TInt err = aFs.Entry(src, srcEntry);
    if (err != KErrNone) {
        return err;
    }

    BaflUtils::EnsurePathExistsL(aFs, targetDir);

    CFileMan *fileMan = CFileMan::NewL(aFs);
    CleanupStack::PushL(fileMan);
    err = fileMan->Copy(src, targetFile, CFileMan::EOverWrite);
    CleanupStack::PopAndDestroy(); /* fileMan */
    if (err != KErrNone) {
        return err;
    }

    /* the copy must be complete, a half written DLL would crash the game */
    TEntry dstEntry;
    err = aFs.Entry(targetFile, dstEntry);
    if (err == KErrNone && dstEntry.iSize != srcEntry.iSize) {
        err = KErrCorrupt;
    }
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
