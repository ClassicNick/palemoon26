/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */ 
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"
#include "mozilla/Util.h"

#include "nsCOMPtr.h"
#include "nsAutoPtr.h"
#include "nsMemory.h"

#include "nsLocalFile.h"
#include "nsIDirectoryEnumerator.h"
#include "nsNativeCharsetUtils.h"

#include "nsISimpleEnumerator.h"
#include "nsIComponentManager.h"
#include "prio.h"
#include "private/pprio.h"  // To get PR_ImportFile
#include "prprf.h"
#include "prmem.h"
#include "nsHashKeys.h"

#include "nsXPIDLString.h"
#include "nsReadableUtils.h"

#include <direct.h>
#include <windows.h>
#include <shlwapi.h>
#include <aclapi.h>

#include "shellapi.h"
#include "shlguid.h"

#include  <io.h>
#include  <stdio.h>
#include  <stdlib.h>
#include  <mbstring.h>

#include "nsXPIDLString.h"
#include "prproces.h"

#include "mozilla/Mutex.h"
#include "SpecialSystemDirectory.h"

#include "nsTraceRefcntImpl.h"
#include "nsXPCOMCIDInternal.h"
#include "nsThreadUtils.h"

using namespace mozilla;

#define CHECK_mWorkingPath()                    \
    PR_BEGIN_MACRO                              \
        if (mWorkingPath.IsEmpty())             \
            return NS_ERROR_NOT_INITIALIZED;    \
    PR_END_MACRO

// CopyFileEx only supports unbuffered I/O in Windows Vista and above
#ifndef COPY_FILE_NO_BUFFERING
#define COPY_FILE_NO_BUFFERING 0x00001000
#endif

#ifndef FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED  0x00002000
#endif

#ifndef DRIVE_REMOTE
#define DRIVE_REMOTE 4
#endif

/**
 * A runnable to dispatch back to the main thread when 
 * AsyncLocalFileWinOperation completes.
*/
class AsyncLocalFileWinDone : public nsRunnable
{
public:
    AsyncLocalFileWinDone() :
        mWorkerThread(do_GetCurrentThread())
    {
        // Objects of this type must only be created on worker threads
        MOZ_ASSERT(!NS_IsMainThread()); 
    }

    NS_IMETHOD Run() {
        // This event shuts down the worker thread and so must be main thread.
        MOZ_ASSERT(NS_IsMainThread());

        // If we don't destroy the thread when we're done with it, it will hang
        // around forever... and that is bad!
        mWorkerThread->Shutdown();
        return NS_OK;
    }

private:
    nsCOMPtr<nsIThread> mWorkerThread;
};

/**
 * A runnable to dispatch from the main thread when an async operation should
 * be performed. 
*/
class AsyncLocalFileWinOperation : public nsRunnable
{
public:
    enum FileOp { RevealOp, LaunchOp };

    AsyncLocalFileWinOperation(AsyncLocalFileWinOperation::FileOp aOperation,
                               const nsAString &aResolvedPath) : 
        mOperation(aOperation),
        mResolvedPath(aResolvedPath)
    {
      if(!sILCreateFromPathW && !sSHOpenFolderAndSelectItems) {
        // shell32.dll should be loaded already, so we are not actually
        // loading the library here.
        HMODULE hLibShell = GetModuleHandleW(L"shell32.dll");
        if (hLibShell) {
          // ILCreateFromPathW is available in XP and up.
          sILCreateFromPathW = (ILCreateFromPathWPtr)
                                GetProcAddress(hLibShell,
                                               "ILCreateFromPathW");

          // SHOpenFolderAndSelectItems is available in XP and up.
          sSHOpenFolderAndSelectItems = (SHOpenFolderAndSelectItemsPtr)
                                         GetProcAddress(hLibShell,
                                                        "SHOpenFolderAndSelectItems");
        }
      }
    }

    NS_IMETHOD Run() {
        MOZ_ASSERT(!NS_IsMainThread(),
            "AsyncLocalFileWinOperation should not be run on the main thread!");

        CoInitialize(NULL);
        switch(mOperation) {
        case RevealOp: {
            Reveal();
        }
        break;
        case LaunchOp: {
            Launch();
        }
        break;
        }
        CoUninitialize();

        // Send the result back to the main thread so that it can shutdown
        nsCOMPtr<nsIRunnable> resultrunnable = new AsyncLocalFileWinDone();
        NS_DispatchToMainThread(resultrunnable);
        return NS_OK;
    }

private:
    static ILCreateFromPathWPtr sILCreateFromPathW;
    static SHOpenFolderAndSelectItemsPtr sSHOpenFolderAndSelectItems;
    // Reveals the path in explorer.
    nsresult RevealUsingShell()
    {
      // All of these shell32.dll related pointers should be non NULL
      // on XP and later.
      if (!sILCreateFromPathW || !sSHOpenFolderAndSelectItems) {
        return NS_ERROR_FAILURE;
      }

        DWORD attributes = GetFileAttributesW(mResolvedPath.get());
        if (INVALID_FILE_ATTRIBUTES == attributes) {
            return NS_ERROR_FILE_INVALID_PATH;
        }

        HRESULT hr;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            // We have a directory so we should open the directory itself.
            ITEMIDLIST *dir = sILCreateFromPathW(mResolvedPath.get());
            if (!dir) {
              return NS_ERROR_FAILURE;
            }

            const ITEMIDLIST* selection[] = { dir };
            UINT count = ArrayLength(selection);

            //Perform the open of the directory.
            hr = sSHOpenFolderAndSelectItems(dir, count, selection, 0);
            CoTaskMemFree(dir);
        } else {
            int32_t len = mResolvedPath.Length();
            // We don't currently handle UNC long paths of the form \\?\ anywhere so
            // this should be fine.
            if (len > MAX_PATH) {
                return NS_ERROR_FILE_INVALID_PATH;
            }
            WCHAR parentDirectoryPath[MAX_PATH + 1] = { 0 };
            wcsncpy(parentDirectoryPath, mResolvedPath.get(), MAX_PATH);
            PathRemoveFileSpecW(parentDirectoryPath);

            // We have a file so we should open the parent directory.
            ITEMIDLIST *dir = sILCreateFromPathW(parentDirectoryPath);
            if (!dir) {
                return NS_ERROR_FAILURE;
            }

            // Set the item in the directory to select to the file we want to reveal.
            ITEMIDLIST *item = sILCreateFromPathW(mResolvedPath.get());
            if (!item) {
                CoTaskMemFree(dir);
                return NS_ERROR_FAILURE;
            }
            
            const ITEMIDLIST* selection[] = { item };
            UINT count = ArrayLength(selection);

            //Perform the selection of the file.
            hr = sSHOpenFolderAndSelectItems(dir, count, selection, 0);

            CoTaskMemFree(dir);
            CoTaskMemFree(item);
        }
        
        return SUCCEEDED(hr) ? NS_OK : NS_ERROR_FAILURE;
    }

    nsresult RevealClassic()
    {
      // use the full path to explorer for security
      nsCOMPtr<nsIFile> winDir;
      nsresult rv = GetSpecialSystemDirectory(Win_WindowsDirectory, getter_AddRefs(winDir));
      NS_ENSURE_SUCCESS(rv, rv);
      nsAutoString explorerPath;
      rv = winDir->GetPath(explorerPath);
      NS_ENSURE_SUCCESS(rv, rv);
      explorerPath.AppendLiteral("\\explorer.exe");

      DWORD attributes = GetFileAttributesW(mResolvedPath.get());
      if (INVALID_FILE_ATTRIBUTES == attributes) {
          return NS_ERROR_FILE_INVALID_PATH;
      }
      // Always open a new window for files because Win2K doesn't appear to select
      // the file if a window showing that folder was already open. If the resolved
      // path is a directory then instead of opening the parent and selecting it,
      // we open the directory itself.
      nsAutoString explorerParams;
      if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) // valid because we ResolveAndStat above
        explorerParams.AppendLiteral("/n,/select,");
      explorerParams.Append(L'\"');
      explorerParams.Append(mResolvedPath);
      explorerParams.Append(L'\"');

      if (::ShellExecuteW(NULL, L"open", explorerPath.get(), explorerParams.get(),
        NULL, SW_SHOWNORMAL) <= (HINSTANCE) 32)
        return NS_ERROR_FAILURE;

      return NS_OK;
    }

    nsresult Reveal()
    {
        // make sure mResolvedPath is set
        nsresult rv;/* = ResolveAndStat();
        if (NS_FAILED(rv) && rv != NS_ERROR_FILE_NOT_FOUND)
            return rv;*/

        // First try revealing with the shell, and if that fails fall back
        // to the classic way using explorer.exe command line parameters
        rv = RevealUsingShell();
        if (NS_FAILED(rv)) {
          rv = RevealClassic();
        }

        return rv;
    }
    
    // Launches the default shell operation for the file path
    nsresult Launch()
    {
        // use the app registry name to launch a shell execute....
        SHELLEXECUTEINFOW seinfo;
        memset(&seinfo, 0, sizeof(seinfo));
        seinfo.cbSize = sizeof(SHELLEXECUTEINFOW);
        seinfo.fMask  = 0;
        seinfo.hwnd   = NULL;
        seinfo.lpVerb = NULL;
        seinfo.lpFile = mResolvedPath.get();
        seinfo.lpParameters =  NULL;
        seinfo.lpDirectory  = NULL;
        seinfo.nShow  = SW_SHOWNORMAL;

        // Use the directory of the file we're launching as the working
        // directory.  That way if we have a self extracting EXE it won't
        // suggest to extract to the install directory.
        WCHAR workingDirectory[MAX_PATH + 1] = { L'\0' };
        wcsncpy(workingDirectory,  mResolvedPath.get(), MAX_PATH);
        if (PathRemoveFileSpecW(workingDirectory)) {
            seinfo.lpDirectory = workingDirectory;
        } else {
            NS_WARNING("Could not set working directory for launched file.");
        }
        
        if (ShellExecuteExW(&seinfo)) {
            return NS_OK;
        }
        DWORD r = GetLastError();
        // if the file has no association, we launch windows' 
        // "what do you want to do" dialog
        if (r == SE_ERR_NOASSOC) {
            nsAutoString shellArg;
            shellArg.Assign(NS_LITERAL_STRING("shell32.dll,OpenAs_RunDLL ") + 
                            mResolvedPath);
            seinfo.lpFile = L"RUNDLL32.EXE";
            seinfo.lpParameters = shellArg.get();
            if (ShellExecuteExW(&seinfo))
                return NS_OK;
            r = GetLastError();
        }
        if (r < 32) {
            switch (r) {
              case 0:
              case SE_ERR_OOM:
                  return NS_ERROR_OUT_OF_MEMORY;
              case ERROR_FILE_NOT_FOUND:
                  return NS_ERROR_FILE_NOT_FOUND;
              case ERROR_PATH_NOT_FOUND:
                  return NS_ERROR_FILE_UNRECOGNIZED_PATH;
              case ERROR_BAD_FORMAT:
                  return NS_ERROR_FILE_CORRUPTED;
              case SE_ERR_ACCESSDENIED:
                  return NS_ERROR_FILE_ACCESS_DENIED;
              case SE_ERR_ASSOCINCOMPLETE:
              case SE_ERR_NOASSOC:
                  return NS_ERROR_UNEXPECTED;
              case SE_ERR_DDEBUSY:
              case SE_ERR_DDEFAIL:
              case SE_ERR_DDETIMEOUT:
                  return NS_ERROR_NOT_AVAILABLE;
              case SE_ERR_DLLNOTFOUND:
                  return NS_ERROR_FAILURE;
              case SE_ERR_SHARE:
                  return NS_ERROR_FILE_IS_LOCKED;
              default:
                  return NS_ERROR_FILE_EXECUTION_FAILED;
            }
        }
        return NS_OK;
    }

    // Stores the operation that will be performed on the thread
    AsyncLocalFileWinOperation::FileOp mOperation;

    // Stores the path to perform the operation on
    nsString mResolvedPath;
};

ILCreateFromPathWPtr AsyncLocalFileWinOperation::sILCreateFromPathW = NULL;
SHOpenFolderAndSelectItemsPtr AsyncLocalFileWinOperation::sSHOpenFolderAndSelectItems = NULL;

class nsDriveEnumerator : public nsISimpleEnumerator
{
public:
    nsDriveEnumerator();
    virtual ~nsDriveEnumerator();
    NS_DECL_ISUPPORTS
    NS_DECL_NSISIMPLEENUMERATOR
    nsresult Init();
private:
    /* mDrives stores the null-separated drive names.
     * Init sets them.
     * HasMoreElements checks mStartOfCurrentDrive.
     * GetNext advances mStartOfCurrentDrive.
     */
    nsString mDrives;
    nsAString::const_iterator mStartOfCurrentDrive;
    nsAString::const_iterator mEndOfDrivesString;
};

//----------------------------------------------------------------------------
// short cut resolver
//----------------------------------------------------------------------------
class ShortcutResolver
{
public:
    ShortcutResolver();
    // nonvirtual since we're not subclassed
    ~ShortcutResolver();

    nsresult Init();
    nsresult Resolve(const WCHAR* in, WCHAR* out);
    nsresult SetShortcut(bool updateExisting,
                         const WCHAR* shortcutPath,
                         const WCHAR* targetPath,
                         const WCHAR* workingDir,
                         const WCHAR* args,
                         const WCHAR* description,
                         const WCHAR* iconFile,
                         int32_t iconIndex);

private:
    Mutex                  mLock;
    nsRefPtr<IPersistFile> mPersistFile;
    nsRefPtr<IShellLinkW>  mShellLink;
};

ShortcutResolver::ShortcutResolver() :
    mLock("ShortcutResolver.mLock")
{
    CoInitialize(NULL);
}

ShortcutResolver::~ShortcutResolver()
{
    CoUninitialize();
}

nsresult
ShortcutResolver::Init()
{
    // Get a pointer to the IPersistFile interface.
    if (FAILED(CoCreateInstance(CLSID_ShellLink,
                                NULL,
                                CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW,
                                getter_AddRefs(mShellLink))) ||
        FAILED(mShellLink->QueryInterface(IID_IPersistFile,
                                          getter_AddRefs(mPersistFile)))) {
        mShellLink = nullptr;
        return NS_ERROR_FAILURE;
    }
    return NS_OK;
}

// |out| must be an allocated buffer of size MAX_PATH
nsresult
ShortcutResolver::Resolve(const WCHAR* in, WCHAR* out)
{
    if (!mShellLink)
        return NS_ERROR_FAILURE;

    MutexAutoLock lock(mLock);

    if (FAILED(mPersistFile->Load(in, STGM_READ)) ||
        FAILED(mShellLink->Resolve(nullptr, SLR_NO_UI)) ||
        FAILED(mShellLink->GetPath(out, MAX_PATH, NULL, SLGP_UNCPRIORITY)))
        return NS_ERROR_FAILURE;
    return NS_OK;
}

nsresult
ShortcutResolver::SetShortcut(bool updateExisting,
                              const WCHAR* shortcutPath,
                              const WCHAR* targetPath,
                              const WCHAR* workingDir,
                              const WCHAR* args,
                              const WCHAR* description,
                              const WCHAR* iconPath,
                              int32_t iconIndex)
{
    if (!mShellLink) {
      return NS_ERROR_FAILURE;
    }

    if (!shortcutPath) {
      return NS_ERROR_FAILURE;
    }

    MutexAutoLock lock(mLock);

    if (updateExisting) {
      if (FAILED(mPersistFile->Load(shortcutPath, STGM_READWRITE))) {
        return NS_ERROR_FAILURE;
      }
    } else {
      if (!targetPath) {
        return NS_ERROR_FILE_TARGET_DOES_NOT_EXIST;
      }

      // Since we reuse our IPersistFile, we have to clear out any values that
      // may be left over from previous calls to SetShortcut.
      if (FAILED(mShellLink->SetWorkingDirectory(L""))
       || FAILED(mShellLink->SetArguments(L""))
       || FAILED(mShellLink->SetDescription(L""))
       || FAILED(mShellLink->SetIconLocation(L"", 0))) {
        return NS_ERROR_FAILURE;
      }
    }

    if (targetPath && FAILED(mShellLink->SetPath(targetPath))) {
      return NS_ERROR_FAILURE;
    }

    if (workingDir && FAILED(mShellLink->SetWorkingDirectory(workingDir))) {
      return NS_ERROR_FAILURE;
    }

    if (args && FAILED(mShellLink->SetArguments(args))) {
      return NS_ERROR_FAILURE;
    }

    if (description && FAILED(mShellLink->SetDescription(description))) {
      return NS_ERROR_FAILURE;
    }

    if (iconPath && FAILED(mShellLink->SetIconLocation(iconPath, iconIndex))) {
      return NS_ERROR_FAILURE;
    }

    if (FAILED(mPersistFile->Save(shortcutPath,
                                  TRUE))) {
      // Second argument indicates whether the file path specified in the
      // first argument should become the "current working file" for this
      // IPersistFile
      return NS_ERROR_FAILURE;
    }

    return NS_OK;
}

static ShortcutResolver * gResolver = nullptr;

static nsresult NS_CreateShortcutResolver()
{
    gResolver = new ShortcutResolver();
    if (!gResolver)
        return NS_ERROR_OUT_OF_MEMORY;

    return gResolver->Init();
}

static void NS_DestroyShortcutResolver()
{
    delete gResolver;
    gResolver = nullptr;
}


//-----------------------------------------------------------------------------
// static helper functions
//-----------------------------------------------------------------------------

// certainly not all the error that can be
// encountered, but many of them common ones
static nsresult ConvertWinError(DWORD winErr)
{
    nsresult rv;

    switch (winErr)
    {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            rv = NS_ERROR_FILE_NOT_FOUND;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_NOT_SAME_DEVICE:
            rv = NS_ERROR_FILE_ACCESS_DENIED;
            break;
        case ERROR_SHARING_VIOLATION: // CreateFile without sharing flags
        case ERROR_LOCK_VIOLATION: // LockFile, LockFileEx
            rv = NS_ERROR_FILE_IS_LOCKED;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_INVALID_BLOCK:
        case ERROR_INVALID_HANDLE:
        case ERROR_ARENA_TRASHED:
            rv = NS_ERROR_OUT_OF_MEMORY;
            break;
        case ERROR_CURRENT_DIRECTORY:
            rv = NS_ERROR_FILE_DIR_NOT_EMPTY;
            break;
        case ERROR_WRITE_PROTECT:
            rv = NS_ERROR_FILE_READ_ONLY;
            break;
        case ERROR_HANDLE_DISK_FULL:
            rv = NS_ERROR_FILE_TOO_BIG;
            break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
        case ERROR_CANNOT_MAKE:
            rv = NS_ERROR_FILE_ALREADY_EXISTS;
            break;
        case ERROR_FILENAME_EXCED_RANGE:
            rv = NS_ERROR_FILE_NAME_TOO_LONG;
            break;
        case ERROR_DIRECTORY:
            rv = NS_ERROR_FILE_NOT_DIRECTORY;
            break;
        case 0:
            rv = NS_OK;
            break;
        default:
            rv = NS_ERROR_FAILURE;
            break;
    }
    return rv;
}

// as suggested in the MSDN documentation on SetFilePointer
static __int64 
MyFileSeek64(HANDLE aHandle, __int64 aDistance, DWORD aMoveMethod)
{
    LARGE_INTEGER li;

    li.QuadPart = aDistance;
    li.LowPart = SetFilePointer(aHandle, li.LowPart, &li.HighPart, aMoveMethod);
    if (li.LowPart == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
    {
        li.QuadPart = -1;
    }

    return li.QuadPart;
}

static bool
IsShortcutPath(const nsAString &path)
{
    // Under Windows, the shortcuts are just files with a ".lnk" extension. 
    // Note also that we don't resolve links in the middle of paths.
    // i.e. "c:\foo.lnk\bar.txt" is invalid.
    NS_ABORT_IF_FALSE(!path.IsEmpty(), "don't pass an empty string");
    int32_t len = path.Length();
    return len >= 4 && (StringTail(path, 4).LowerCaseEqualsASCII(".lnk"));
}

//-----------------------------------------------------------------------------
// We need the following three definitions to make |OpenFile| convert a file 
// handle to an NSPR file descriptor correctly when |O_APPEND| flag is
// specified. It is defined in a private header of NSPR (primpl.h) we can't
// include. As a temporary workaround until we decide how to extend
// |PR_ImportFile|, we define it here. Currently, |_PR_HAVE_PEEK_BUFFER|
// and |PR_STRICT_ADDR_LEN| are not defined for the 'w95'-dependent portion
// of NSPR so that fields of |PRFilePrivate| #ifdef'd by them are not copied.
// Similarly, |_MDFileDesc| is taken from nsprpub/pr/include/md/_win95.h.
// In an unlikely case we switch to 'NT'-dependent NSPR AND this temporary 
// workaround last beyond the switch, |PRFilePrivate| and |_MDFileDesc| 
// need to be changed to match the definitions for WinNT.
//-----------------------------------------------------------------------------
typedef enum {
    _PR_TRI_TRUE = 1,
    _PR_TRI_FALSE = 0,
    _PR_TRI_UNKNOWN = -1
} _PRTriStateBool;

struct _MDFileDesc {
    PROsfd osfd;
};

struct PRFilePrivate {
    int32_t state;
    bool nonblocking;
    _PRTriStateBool inheritable;
    PRFileDesc *next;
    int lockCount;      /*   0: not locked
                         *  -1: a native lockfile call is in progress
                         * > 0: # times the file is locked */
    bool    appendMode; 
    _MDFileDesc md;
};

//-----------------------------------------------------------------------------
// Six static methods defined below (OpenFile,  FileTimeToPRTime, GetFileInfo,
// OpenDir, CloseDir, ReadDir) should go away once the corresponding 
// UTF-16 APIs are implemented on all the supported platforms (or at least 
// Windows 9x/ME) in NSPR. Currently, they're only implemented on 
// Windows NT4 or later. (bug 330665)
//-----------------------------------------------------------------------------

// copied from nsprpub/pr/src/{io/prfile.c | md/windows/w95io.c} : 
// PR_Open and _PR_MD_OPEN
static nsresult
OpenFile(const nsAFlatString &name, int osflags, int mode,
         PRFileDesc **fd)
{
    int32_t access = 0;

    int32_t disposition = 0;
    int32_t attributes = 0;

    if (osflags & PR_SYNC) 
        attributes = FILE_FLAG_WRITE_THROUGH;
    if (osflags & PR_RDONLY || osflags & PR_RDWR)
        access |= GENERIC_READ;
    if (osflags & PR_WRONLY || osflags & PR_RDWR)
        access |= GENERIC_WRITE;

    if ( osflags & PR_CREATE_FILE && osflags & PR_EXCL )
        disposition = CREATE_NEW;
    else if (osflags & PR_CREATE_FILE) {
        if (osflags & PR_TRUNCATE)
            disposition = CREATE_ALWAYS;
        else
            disposition = OPEN_ALWAYS;
    } else {
        if (osflags & PR_TRUNCATE)
            disposition = TRUNCATE_EXISTING;
        else
            disposition = OPEN_EXISTING;
    }

    if (osflags & nsIFile::DELETE_ON_CLOSE) {
        attributes |= FILE_FLAG_DELETE_ON_CLOSE;
    }

    if (osflags & nsIFile::OS_READAHEAD) {
        attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    }

    // If no write permissions are requested, and if we are possibly creating
    // the file, then set the new file as read only.
    // The flag has no effect if we happen to open the file.
    if (!(mode & (PR_IWUSR | PR_IWGRP | PR_IWOTH)) &&
        disposition != OPEN_EXISTING) {
        attributes |= FILE_ATTRIBUTE_READONLY;
    }

    HANDLE file = ::CreateFileW(name.get(), access,
                                FILE_SHARE_READ|FILE_SHARE_WRITE,
                                NULL, disposition, attributes, NULL);

    if (file == INVALID_HANDLE_VALUE) { 
        *fd = nullptr;
        return ConvertWinError(GetLastError());
    }

    *fd = PR_ImportFile((PROsfd) file); 
    if (*fd) {
        // On Windows, _PR_HAVE_O_APPEND is not defined so that we have to
        // add it manually. (see |PR_Open| in nsprpub/pr/src/io/prfile.c)
        (*fd)->secret->appendMode = (PR_APPEND & osflags) ? true : false;
        return NS_OK;
    }

    nsresult rv = NS_ErrorAccordingToNSPR();

    CloseHandle(file);

    return rv;
}

// copied from nsprpub/pr/src/{io/prfile.c | md/windows/w95io.c} :
// PR_FileTimeToPRTime and _PR_FileTimeToPRTime
static
void FileTimeToPRTime(const FILETIME *filetime, PRTime *prtm)
{
#ifdef __GNUC__
    const PRTime _pr_filetime_offset = 116444736000000000LL;
#else
    const PRTime _pr_filetime_offset = 116444736000000000i64;
#endif

    PR_ASSERT(sizeof(FILETIME) == sizeof(PRTime));
    ::CopyMemory(prtm, filetime, sizeof(PRTime));
#ifdef __GNUC__
    *prtm = (*prtm - _pr_filetime_offset) / 10LL;
#else
    *prtm = (*prtm - _pr_filetime_offset) / 10i64;
#endif
}

// copied from nsprpub/pr/src/{io/prfile.c | md/windows/w95io.c} with some
// changes : PR_GetFileInfo64, _PR_MD_GETFILEINFO64
static nsresult
GetFileInfo(const nsAFlatString &name, PRFileInfo64 *info)
{
    WIN32_FILE_ATTRIBUTE_DATA fileData;

    if (name.IsEmpty() || name.FindCharInSet(L"?*") != kNotFound)
        return NS_ERROR_INVALID_ARG;

    if (!::GetFileAttributesExW(name.get(), GetFileExInfoStandard, &fileData))
        return ConvertWinError(GetLastError());

    if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        info->type = PR_FILE_DIRECTORY;
    } else {
        info->type = PR_FILE_FILE;
    }

    info->size = fileData.nFileSizeHigh;
    info->size = (info->size << 32) + fileData.nFileSizeLow;

    FileTimeToPRTime(&fileData.ftLastWriteTime, &info->modifyTime);

    if (0 == fileData.ftCreationTime.dwLowDateTime &&
            0 == fileData.ftCreationTime.dwHighDateTime) {
        info->creationTime = info->modifyTime;
    } else {
        FileTimeToPRTime(&fileData.ftCreationTime, &info->creationTime);
    }

    return NS_OK;
}

struct nsDir
{
    HANDLE   handle; 
    WIN32_FIND_DATAW data;
    bool     firstEntry;
};

static nsresult
OpenDir(const nsAFlatString &name, nsDir * *dir)
{
    NS_ENSURE_ARG_POINTER(dir);

    *dir = nullptr;
    if (name.Length() + 3 >= MAX_PATH)
        return NS_ERROR_FILE_NAME_TOO_LONG;

    nsDir *d  = PR_NEW(nsDir);
    if (!d)
        return NS_ERROR_OUT_OF_MEMORY;

    nsAutoString filename(name);

     //If 'name' ends in a slash or backslash, do not append
     //another backslash.
    if (filename.Last() == L'/' || filename.Last() == L'\\')
        filename.Append('*');
    else 
        filename.AppendLiteral("\\*");

    filename.ReplaceChar(L'/', L'\\');

    // FindFirstFileW Will have a last error of ERROR_DIRECTORY if
    // <file_path>\* is passed in.  If <unknown_path>\* is passed in then
    // ERROR_PATH_NOT_FOUND will be the last error.
    d->handle = ::FindFirstFileW(filename.get(), &(d->data) );

    if (d->handle == INVALID_HANDLE_VALUE) {
        PR_Free(d);
        return ConvertWinError(GetLastError());
    }
    d->firstEntry = true;

    *dir = d;
    return NS_OK;
}

static nsresult
ReadDir(nsDir *dir, PRDirFlags flags, nsString& name)
{
    name.Truncate();
    NS_ENSURE_ARG(dir);

    while (1) {
        BOOL rv;
        if (dir->firstEntry)
        {
            dir->firstEntry = false;
            rv = 1;
        } else
            rv = ::FindNextFileW(dir->handle, &(dir->data));

        if (rv == 0)
            break;

        const PRUnichar *fileName;
        nsString tmp;
        fileName = (dir)->data.cFileName;

        if ((flags & PR_SKIP_DOT) &&
            (fileName[0] == L'.') && (fileName[1] == L'\0'))
            continue;
        if ((flags & PR_SKIP_DOT_DOT) &&
            (fileName[0] == L'.') && (fileName[1] == L'.') &&
            (fileName[2] == L'\0'))
            continue;

        DWORD attrib =  dir->data.dwFileAttributes;
        if ((flags & PR_SKIP_HIDDEN) && (attrib & FILE_ATTRIBUTE_HIDDEN))
            continue;

        if (fileName == tmp.get())
            name = tmp;
        else 
            name = fileName;
        return NS_OK;
    }

    DWORD err = GetLastError();
    return err == ERROR_NO_MORE_FILES ? NS_OK : ConvertWinError(err);
}

static nsresult
CloseDir(nsDir *&d)
{
    NS_ENSURE_ARG(d);

    BOOL isOk = FindClose(d->handle);
    // PR_DELETE also nulls out the passed in pointer.
    PR_DELETE(d);
    return isOk ? NS_OK : ConvertWinError(GetLastError());
}

//-----------------------------------------------------------------------------
// nsDirEnumerator
//-----------------------------------------------------------------------------

class nsDirEnumerator MOZ_FINAL : public nsISimpleEnumerator,
                                  public nsIDirectoryEnumerator
{
    public:

        NS_DECL_ISUPPORTS

        nsDirEnumerator() : mDir(nullptr)
        {
        }

        nsresult Init(nsIFile* parent)
        {
            nsAutoString filepath;
            parent->GetTarget(filepath);

            if (filepath.IsEmpty())
            {
                parent->GetPath(filepath);
            }

            if (filepath.IsEmpty())
            {
                return NS_ERROR_UNEXPECTED;
            }

            // IsDirectory is not needed here because OpenDir will return
            // NS_ERROR_FILE_NOT_DIRECTORY if the passed in path is a file.
            nsresult rv = OpenDir(filepath, &mDir);
            if (NS_FAILED(rv))
                return rv;

            mParent = parent;
            return NS_OK;
        }

        NS_IMETHOD HasMoreElements(bool *result)
        {
            nsresult rv;
            if (mNext == nullptr && mDir)
            {
                nsString name;
                rv = ReadDir(mDir, PR_SKIP_BOTH, name);
                if (NS_FAILED(rv))
                    return rv;
                if (name.IsEmpty()) 
                {
                    // end of dir entries
                    if (NS_FAILED(CloseDir(mDir)))
                        return NS_ERROR_FAILURE;

                    *result = false;
                    return NS_OK;
                }

                nsCOMPtr<nsIFile> file;
                rv = mParent->Clone(getter_AddRefs(file));
                if (NS_FAILED(rv))
                    return rv;

                rv = file->Append(name);
                if (NS_FAILED(rv))
                    return rv;

                mNext = do_QueryInterface(file);
            }
            *result = mNext != nullptr;
            if (!*result) 
                Close();
            return NS_OK;
        }

        NS_IMETHOD GetNext(nsISupports **result)
        {
            nsresult rv;
            bool hasMore;
            rv = HasMoreElements(&hasMore);
            if (NS_FAILED(rv)) return rv;

            *result = mNext;        // might return nullptr
            NS_IF_ADDREF(*result);

            mNext = nullptr;
            return NS_OK;
        }

        NS_IMETHOD GetNextFile(nsIFile **result)
        {
            *result = nullptr;
            bool hasMore = false;
            nsresult rv = HasMoreElements(&hasMore);
            if (NS_FAILED(rv) || !hasMore)
                return rv;
            *result = mNext;
            NS_IF_ADDREF(*result);
            mNext = nullptr;
            return NS_OK;
        }

        NS_IMETHOD Close()
        {
            if (mDir)
            {
                nsresult rv = CloseDir(mDir);
                NS_ASSERTION(NS_SUCCEEDED(rv), "close failed");
                if (NS_FAILED(rv))
                    return NS_ERROR_FAILURE;
            }
            return NS_OK;
        }

        // dtor can be non-virtual since there are no subclasses, but must be
        // public to use the class on the stack.
        ~nsDirEnumerator()
        {
            Close();
        }

    protected:
        nsDir*             mDir;
        nsCOMPtr<nsIFile>  mParent;
        nsCOMPtr<nsIFile>  mNext;
};

NS_IMPL_ISUPPORTS2(nsDirEnumerator, nsISimpleEnumerator, nsIDirectoryEnumerator)


//-----------------------------------------------------------------------------
// nsLocalFile <public>
//-----------------------------------------------------------------------------

nsLocalFile::nsLocalFile()
  : mDirty(true)
  , mResolveDirty(true)
  , mFollowSymlinks(false)
{
}

nsresult
nsLocalFile::nsLocalFileConstructor(nsISupports* outer, const nsIID& aIID, void* *aInstancePtr)
{
    NS_ENSURE_ARG_POINTER(aInstancePtr);
    NS_ENSURE_NO_AGGREGATION(outer);

    nsLocalFile* inst = new nsLocalFile();
    if (inst == NULL)
        return NS_ERROR_OUT_OF_MEMORY;

    nsresult rv = inst->QueryInterface(aIID, aInstancePtr);
    if (NS_FAILED(rv))
    {
        delete inst;
        return rv;
    }
    return NS_OK;
}


//-----------------------------------------------------------------------------
// nsLocalFile::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_THREADSAFE_ISUPPORTS4(nsLocalFile,
                              nsILocalFile,
                              nsIFile,
                              nsILocalFileWin,
                              nsIHashable)


//-----------------------------------------------------------------------------
// nsLocalFile <private>
//-----------------------------------------------------------------------------

nsLocalFile::nsLocalFile(const nsLocalFile& other)
  : mDirty(true)
  , mResolveDirty(true)
  , mFollowSymlinks(other.mFollowSymlinks)
  , mWorkingPath(other.mWorkingPath)
{
}

// Resolve the shortcut file from mWorkingPath and write the path 
// it points to into mResolvedPath.
nsresult
nsLocalFile::ResolveShortcut()
{
    // we can't do anything without the resolver
    if (!gResolver)
        return NS_ERROR_FAILURE;

    mResolvedPath.SetLength(MAX_PATH);
    if (mResolvedPath.Length() != MAX_PATH)
        return NS_ERROR_OUT_OF_MEMORY;

    PRUnichar *resolvedPath = mResolvedPath.BeginWriting();

    // resolve this shortcut
    nsresult rv = gResolver->Resolve(mWorkingPath.get(), resolvedPath);

    size_t len = NS_FAILED(rv) ? 0 : wcslen(resolvedPath);
    mResolvedPath.SetLength(len);

    return rv;
}

// Resolve any shortcuts and stat the resolved path. After a successful return
// the path is guaranteed valid and the members of mFileInfo64 can be used.
nsresult
nsLocalFile::ResolveAndStat()
{
    // if we aren't dirty then we are already done
    if (!mDirty)
        return NS_OK;

    // we can't resolve/stat anything that isn't a valid NSPR addressable path
    if (mWorkingPath.IsEmpty())
        return NS_ERROR_FILE_INVALID_PATH;

    // this is usually correct
    mResolvedPath.Assign(mWorkingPath);

    // slutty hack designed to work around bug 134796 until it is fixed
    nsAutoString nsprPath(mWorkingPath.get());
    if (mWorkingPath.Length() == 2 && mWorkingPath.CharAt(1) == L':') 
        nsprPath.Append('\\');

    // first we will see if the working path exists. If it doesn't then
    // there is nothing more that can be done
    nsresult rv = GetFileInfo(nsprPath, &mFileInfo64);
    if (NS_FAILED(rv))
        return rv;

    // if this isn't a shortcut file or we aren't following symlinks then we're done 
    if (!mFollowSymlinks 
        || mFileInfo64.type != PR_FILE_FILE 
        || !IsShortcutPath(mWorkingPath))
    {
        mDirty = false;
        mResolveDirty = false;
        return NS_OK;
    }

    // we need to resolve this shortcut to what it points to, this will
    // set mResolvedPath. Even if it fails we need to have the resolved
    // path equal to working path for those functions that always use
    // the resolved path.
    rv = ResolveShortcut();
    if (NS_FAILED(rv))
    {
        mResolvedPath.Assign(mWorkingPath);
        return rv;
    }
    mResolveDirty = false;

    // get the details of the resolved path
    rv = GetFileInfo(mResolvedPath, &mFileInfo64);
    if (NS_FAILED(rv))
        return rv;

    mDirty = false;
    return NS_OK;
}

/**
 * Fills the mResolvedPath member variable with the file or symlink target
 * if follow symlinks is on.  This is a copy of the Resolve parts from
 * ResolveAndStat. ResolveAndStat is much slower though because of the stat.
 *
 * @return NS_OK on success.
*/
nsresult
nsLocalFile::Resolve()
{
  // if we aren't dirty then we are already done
  if (!mResolveDirty) {
    return NS_OK;
  }

  // we can't resolve/stat anything that isn't a valid NSPR addressable path
  if (mWorkingPath.IsEmpty()) {
    return NS_ERROR_FILE_INVALID_PATH;
  }
  
  // this is usually correct
  mResolvedPath.Assign(mWorkingPath);

  // if this isn't a shortcut file or we aren't following symlinks then
  // we're done.
  if (!mFollowSymlinks || 
      !IsShortcutPath(mWorkingPath)) {
    mResolveDirty = false;
    return NS_OK;
  }

  // we need to resolve this shortcut to what it points to, this will
  // set mResolvedPath. Even if it fails we need to have the resolved
  // path equal to working path for those functions that always use
  // the resolved path.
  nsresult rv = ResolveShortcut();
  if (NS_FAILED(rv)) {
    mResolvedPath.Assign(mWorkingPath);
    return rv;
  }

  mResolveDirty = false;
  return NS_OK;
}

//-----------------------------------------------------------------------------
// nsLocalFile::nsIFile,nsILocalFile
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsLocalFile::Clone(nsIFile **file)
{
    // Just copy-construct ourselves
    *file = new nsLocalFile(*this);
    if (!*file)
      return NS_ERROR_OUT_OF_MEMORY;

    NS_ADDREF(*file);
    
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::InitWithFile(nsIFile *aFile)
{
    NS_ENSURE_ARG(aFile);
    
    nsAutoString path;
    aFile->GetPath(path);
    if (path.IsEmpty())
        return NS_ERROR_INVALID_ARG;
    return InitWithPath(path); 
}

NS_IMETHODIMP
nsLocalFile::InitWithPath(const nsAString &filePath)
{
    MakeDirty();

    nsAString::const_iterator begin, end;
    filePath.BeginReading(begin);
    filePath.EndReading(end);

    // input string must not be empty
    if (begin == end)
        return NS_ERROR_FAILURE;

    PRUnichar firstChar = *begin;
    PRUnichar secondChar = *(++begin);

    // just do a sanity check.  if it has any forward slashes, it is not a Native path
    // on windows.  Also, it must have a colon at after the first char.
    if (FindCharInReadable(L'/', begin, end))
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    if (secondChar != L':' && (secondChar != L'\\' || firstChar != L'\\'))
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    if (secondChar == L':') {
        // Make sure we have a valid drive, later code assumes the drive letter
        // is a single char a-z or A-Z.
        if (PathGetDriveNumberW(filePath.Data()) == -1) {
            return NS_ERROR_FILE_UNRECOGNIZED_PATH;
        }
    }

    mWorkingPath = filePath;
    // kill any trailing '\'
    if (mWorkingPath.Last() == L'\\')
        mWorkingPath.Truncate(mWorkingPath.Length() - 1);

    return NS_OK;

}

NS_IMETHODIMP
nsLocalFile::OpenNSPRFileDesc(int32_t flags, int32_t mode, PRFileDesc **_retval)
{
    nsresult rv = Resolve();
    if (NS_FAILED(rv))
        return rv;

    return OpenFile(mResolvedPath, flags, mode, _retval);
}


NS_IMETHODIMP
nsLocalFile::OpenANSIFileDesc(const char *mode, FILE * *_retval)
{
    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv) && rv != NS_ERROR_FILE_NOT_FOUND)
        return rv;

    *_retval = _wfopen(mResolvedPath.get(), NS_ConvertASCIItoUTF16(mode).get());
    if (*_retval)
        return NS_OK;

    return NS_ERROR_FAILURE;
}



NS_IMETHODIMP
nsLocalFile::Create(uint32_t type, uint32_t attributes)
{
    if (type != NORMAL_FILE_TYPE && type != DIRECTORY_TYPE)
        return NS_ERROR_FILE_UNKNOWN_TYPE;

    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv) && rv != NS_ERROR_FILE_NOT_FOUND)
        return rv;

    // create directories to target
    //
    // A given local file can be either one of these forms:
    //
    //   - normal:    X:\some\path\on\this\drive
    //                       ^--- start here
    //
    //   - UNC path:  \\machine\volume\some\path\on\this\drive
    //                                     ^--- start here
    //
    // Skip the first 'X:\' for the first form, and skip the first full
    // '\\machine\volume\' segment for the second form.

    PRUnichar* path = mResolvedPath.BeginWriting();

    if (path[0] == L'\\' && path[1] == L'\\')
    {
        // dealing with a UNC path here; skip past '\\machine\'
        path = wcschr(path + 2, L'\\');
        if (!path)
            return NS_ERROR_FILE_INVALID_PATH;
        ++path;
    }

    // search for first slash after the drive (or volume) name
    PRUnichar* slash = wcschr(path, L'\\');

    nsresult directoryCreateError = NS_OK;
    if (slash)
    {
        // skip the first '\\'
        ++slash;
        slash = wcschr(slash, L'\\');

        while (slash)
        {
            *slash = L'\0';

            if (!::CreateDirectoryW(mResolvedPath.get(), NULL)) {
                rv = ConvertWinError(GetLastError());
                if (NS_ERROR_FILE_NOT_FOUND == rv &&
                    NS_ERROR_FILE_ACCESS_DENIED == directoryCreateError) {
                    // If a previous CreateDirectory failed due to access, return that.
                    return NS_ERROR_FILE_ACCESS_DENIED;
                }
                // perhaps the base path already exists, or perhaps we don't have
                // permissions to create the directory.  NOTE: access denied could
                // occur on a parent directory even though it exists.
                else if (NS_ERROR_FILE_ALREADY_EXISTS != rv &&
                         NS_ERROR_FILE_ACCESS_DENIED != rv) {
                    return rv;
                }

                directoryCreateError = rv;
            }
            *slash = L'\\';
            ++slash;
            slash = wcschr(slash, L'\\');
        }
    }

    if (type == NORMAL_FILE_TYPE)
    {
        PRFileDesc* file;
        rv = OpenFile(mResolvedPath,
                      PR_RDONLY | PR_CREATE_FILE | PR_APPEND | PR_EXCL, attributes,
                      &file);
        if (file)
            PR_Close(file);

        if (rv == NS_ERROR_FILE_ACCESS_DENIED)
        {
            // need to return already-exists for directories (bug 452217)
            bool isdir;
            if (NS_SUCCEEDED(IsDirectory(&isdir)) && isdir)
                rv = NS_ERROR_FILE_ALREADY_EXISTS;
        } else if (NS_ERROR_FILE_NOT_FOUND == rv && 
                   NS_ERROR_FILE_ACCESS_DENIED == directoryCreateError) {
            // If a previous CreateDirectory failed due to access, return that.
            return NS_ERROR_FILE_ACCESS_DENIED;
        }
        return rv;
    }

    if (type == DIRECTORY_TYPE)
    {
        if (!::CreateDirectoryW(mResolvedPath.get(), NULL)) {
          rv = ConvertWinError(GetLastError());
          if (NS_ERROR_FILE_NOT_FOUND == rv && 
              NS_ERROR_FILE_ACCESS_DENIED == directoryCreateError) {
              // If a previous CreateDirectory failed due to access, return that.
              return NS_ERROR_FILE_ACCESS_DENIED;
          } else {
              return rv;
          }
        }
        else
            return NS_OK;
    }

    return NS_ERROR_FILE_UNKNOWN_TYPE;
}


NS_IMETHODIMP
nsLocalFile::Append(const nsAString &node)
{
    // append this path, multiple components are not permitted
    return AppendInternal(PromiseFlatString(node), false);
}

NS_IMETHODIMP
nsLocalFile::AppendRelativePath(const nsAString &node)
{
    // append this path, multiple components are permitted
    return AppendInternal(PromiseFlatString(node), true);
}


nsresult
nsLocalFile::AppendInternal(const nsAFlatString &node, bool multipleComponents)
{
    if (node.IsEmpty())
        return NS_OK;

    // check the relative path for validity
    if (node.First() == L'\\'                                   // can't start with an '\'
        || node.FindChar(L'/') != kNotFound                     // can't contain /
        || node.EqualsASCII(".."))                              // can't be ..
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    if (multipleComponents)
    {
        // can't contain .. as a path component. Ensure that the valid components
        // "foo..foo", "..foo", and "foo.." are not falsely detected,
        // but the invalid paths "..\", "foo\..", "foo\..\foo", 
        // "..\foo", etc are.
        NS_NAMED_LITERAL_STRING(doubleDot, "\\.."); 
        nsAString::const_iterator start, end, offset;
        node.BeginReading(start);
        node.EndReading(end);
        offset = end; 
        while (FindInReadable(doubleDot, start, offset))
        {
            if (offset == end || *offset == L'\\')
                return NS_ERROR_FILE_UNRECOGNIZED_PATH;
            start = offset;
            offset = end;
        }
        
        // catches the remaining cases of prefixes 
        if (StringBeginsWith(node, NS_LITERAL_STRING("..\\")))
            return NS_ERROR_FILE_UNRECOGNIZED_PATH;
    }
    // single components can't contain '\'
    else if (node.FindChar(L'\\') != kNotFound)
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    MakeDirty();
    
    mWorkingPath.Append(NS_LITERAL_STRING("\\") + node);
    
    return NS_OK;
}

#define TOUPPER(u) (((u) >= L'a' && (u) <= L'z') ? \
                    (u) - (L'a' - L'A') : (u))

NS_IMETHODIMP
nsLocalFile::Normalize()
{
    // XXX See bug 187957 comment 18 for possible problems with this implementation.
    
    if (mWorkingPath.IsEmpty())
        return NS_OK;

    nsAutoString path(mWorkingPath);

    // find the index of the root backslash for the path. Everything before 
    // this is considered fully normalized and cannot be ascended beyond 
    // using ".."  For a local drive this is the first slash (e.g. "c:\").
    // For a UNC path it is the slash following the share name 
    // (e.g. "\\server\share\").
    int32_t rootIdx = 2;        // default to local drive
    if (path.First() == L'\\')   // if a share then calculate the rootIdx
    {
        rootIdx = path.FindChar(L'\\', 2);   // skip \\ in front of the server
        if (rootIdx == kNotFound)
            return NS_OK;                   // already normalized
        rootIdx = path.FindChar(L'\\', rootIdx+1);
        if (rootIdx == kNotFound)
            return NS_OK;                   // already normalized
    }
    else if (path.CharAt(rootIdx) != L'\\')
    {
        // The path has been specified relative to the current working directory 
        // for that drive. To normalize it, the current working directory for 
        // that drive needs to be inserted before the supplied relative path
        // which will provide an absolute path (and the rootIdx will still be 2).
        WCHAR cwd[MAX_PATH];
        WCHAR * pcwd = cwd;
        int drive = TOUPPER(path.First()) - 'A' + 1;
        /* We need to worry about IPH, for details read bug 419326.
         * _getdrives - http://msdn2.microsoft.com/en-us/library/xdhk0xd2.aspx 
         * uses a bitmask, bit 0 is 'a:'
         * _chdrive - http://msdn2.microsoft.com/en-us/library/0d1409hb.aspx
         * _getdcwd - http://msdn2.microsoft.com/en-us/library/7t2zk3s4.aspx
         * take an int, 1 is 'a:'.
         *
         * Because of this, we need to do some math. Subtract 1 to convert from
         * _chdrive/_getdcwd format to _getdrives drive numbering.
         * Shift left x bits to convert from integer indexing to bitfield indexing.
         * And of course, we need to find out if the drive is in the bitmask.
         *
         * If we're really unlucky, we can still lose, but only if the user
         * manages to eject the drive between our call to _getdrives() and
         * our *calls* to _wgetdcwd.
         */
        if (!((1 << (drive - 1)) & _getdrives()))
            return NS_ERROR_FILE_INVALID_PATH;
        if (!_wgetdcwd(drive, pcwd, MAX_PATH))
            pcwd = _wgetdcwd(drive, 0, 0);
        if (!pcwd)
            return NS_ERROR_OUT_OF_MEMORY;
        nsAutoString currentDir(pcwd);
        if (pcwd != cwd)
            free(pcwd);

        if (currentDir.Last() == '\\')
            path.Replace(0, 2, currentDir);
        else
            path.Replace(0, 2, currentDir + NS_LITERAL_STRING("\\"));
    }
    NS_POSTCONDITION(0 < rootIdx && rootIdx < (int32_t)path.Length(), "rootIdx is invalid");
    NS_POSTCONDITION(path.CharAt(rootIdx) == '\\', "rootIdx is invalid");

    // if there is nothing following the root path then it is already normalized
    if (rootIdx + 1 == (int32_t)path.Length())
        return NS_OK;

    // assign the root
    const PRUnichar * pathBuffer = path.get();  // simplify access to the buffer
    mWorkingPath.SetCapacity(path.Length()); // it won't ever grow longer
    mWorkingPath.Assign(pathBuffer, rootIdx);

    // Normalize the path components. The actions taken are:
    //
    //  "\\"    condense to single backslash
    //  "."     remove from path
    //  ".."    up a directory
    //  "..."   remove from path (any number of dots > 2)
    //
    // The last form is something that Windows 95 and 98 supported and 
    // is a shortcut for changing up multiple directories. Windows XP
    // and ilk ignore it in a path, as is done here.
    int32_t len, begin, end = rootIdx;
    while (end < (int32_t)path.Length())
    {
        // find the current segment (text between the backslashes) to 
        // be examined, this will set the following variables:
        //  begin == index of first char in segment
        //  end   == index 1 char after last char in segment
        //  len   == length of segment 
        begin = end + 1;
        end = path.FindChar('\\', begin);
        if (end == kNotFound)
            end = path.Length();
        len = end - begin;
        
        // ignore double backslashes
        if (len == 0)
            continue;
        
        // len != 0, and interesting paths always begin with a dot
        if (pathBuffer[begin] == '.')
        {
            // ignore single dots
            if (len == 1)
                continue;   

            // handle multiple dots
            if (len >= 2 && pathBuffer[begin+1] == L'.')
            {
                // back up a path component on double dot
                if (len == 2)
                {
                    int32_t prev = mWorkingPath.RFindChar('\\');
                    if (prev >= rootIdx)
                        mWorkingPath.Truncate(prev);
                    continue;
                }

                // length is > 2 and the first two characters are dots. 
                // if the rest of the string is dots, then ignore it.
                int idx = len - 1;
                for (; idx >= 2; --idx) 
                {
                    if (pathBuffer[begin+idx] != L'.')
                        break;
                }

                // this is true if the loop above didn't break
                // and all characters in this segment are dots.
                if (idx < 2) 
                    continue;
            }
        }

        // add the current component to the path, including the preceding backslash
        mWorkingPath.Append(pathBuffer + begin - 1, len + 1);
    }

    // kill trailing dots and spaces.
    int32_t filePathLen = mWorkingPath.Length() - 1;
    while(filePathLen > 0 && (mWorkingPath[filePathLen] == L' ' ||
          mWorkingPath[filePathLen] == L'.'))
    {
        mWorkingPath.Truncate(filePathLen--);
    } 

    MakeDirty();
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetLeafName(nsAString &aLeafName)
{
    aLeafName.Truncate();

    if (mWorkingPath.IsEmpty())
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    int32_t offset = mWorkingPath.RFindChar(L'\\');

    // if the working path is just a node without any lashes.
    if (offset == kNotFound)
        aLeafName = mWorkingPath;
    else
        aLeafName = Substring(mWorkingPath, offset + 1);

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetLeafName(const nsAString &aLeafName)
{
    MakeDirty();

    if (mWorkingPath.IsEmpty())
        return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    // cannot use nsCString::RFindChar() due to 0x5c problem
    int32_t offset = mWorkingPath.RFindChar(L'\\');
    if (offset)
    {
        mWorkingPath.Truncate(offset+1);
    }
    mWorkingPath.Append(aLeafName);

    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::GetPath(nsAString &_retval)
{
    _retval = mWorkingPath;
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetCanonicalPath(nsAString &aResult)
{
    EnsureShortPath();
    aResult.Assign(mShortWorkingPath);
    return NS_OK;
}

typedef struct {
    WORD wLanguage;
    WORD wCodePage;
} LANGANDCODEPAGE;

NS_IMETHODIMP
nsLocalFile::GetVersionInfoField(const char* aField, nsAString& _retval)
{
    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    rv = NS_ERROR_FAILURE;

    // Cast away const-ness here because WinAPI functions don't understand it, 
    // the path is used for [in] parameters only however so it's safe. 
    WCHAR *path = const_cast<WCHAR*>(mFollowSymlinks ? mResolvedPath.get() 
                                                        : mWorkingPath.get());

    DWORD dummy;
    DWORD size = ::GetFileVersionInfoSizeW(path, &dummy);
    if (!size)
        return rv;

    void* ver = calloc(size, 1);
    if (!ver)
        return NS_ERROR_OUT_OF_MEMORY;

    if (::GetFileVersionInfoW(path, 0, size, ver)) 
    {
        LANGANDCODEPAGE* translate = nullptr;
        UINT pageCount;
        BOOL queryResult = ::VerQueryValueW(ver, L"\\VarFileInfo\\Translation", 
                                            (void**)&translate, &pageCount);
        if (queryResult && translate) 
        {
            for (int32_t i = 0; i < 2; ++i) 
            { 
                PRUnichar subBlock[MAX_PATH];
                _snwprintf(subBlock, MAX_PATH,
                           L"\\StringFileInfo\\%04x%04x\\%s", 
                           (i == 0 ? translate[0].wLanguage 
                                   : ::GetUserDefaultLangID()),
                           translate[0].wCodePage,
                           NS_ConvertASCIItoUTF16(
                               nsDependentCString(aField)).get());
                subBlock[MAX_PATH - 1] = 0;
                LPVOID value = nullptr;
                UINT size;
                queryResult = ::VerQueryValueW(ver, subBlock, &value, &size);
                if (queryResult && value)
                {
                    _retval.Assign(static_cast<PRUnichar*>(value));
                    if (!_retval.IsEmpty()) 
                    {
                        rv = NS_OK;
                        break;
                    }
                }
            }
        }
    }
    free(ver);
    
    return rv;
}

NS_IMETHODIMP
nsLocalFile::SetShortcut(nsIFile* targetFile,
                         nsIFile* workingDir,
                         const PRUnichar* args,
                         const PRUnichar* description,
                         nsIFile* iconFile,
                         int32_t iconIndex)
{
    bool exists;
    nsresult rv = this->Exists(&exists);
    if (NS_FAILED(rv)) {
      return rv;
    }

    const WCHAR* targetFilePath = NULL;
    const WCHAR* workingDirPath = NULL;
    const WCHAR* iconFilePath = NULL;

    nsAutoString targetFilePathAuto;
    if (targetFile) {
        rv = targetFile->GetPath(targetFilePathAuto);
        if (NS_FAILED(rv)) {
          return rv;
        }
        targetFilePath = targetFilePathAuto.get();
    }

    nsAutoString workingDirPathAuto;
    if (workingDir) {
        rv = workingDir->GetPath(workingDirPathAuto);
        if (NS_FAILED(rv)) {
          return rv;
        }
        workingDirPath = workingDirPathAuto.get();
    }

    nsAutoString iconPathAuto;
    if (iconFile) {
        rv = iconFile->GetPath(iconPathAuto);
        if (NS_FAILED(rv)) {
          return rv;
        }
        iconFilePath = iconPathAuto.get();
    }

    rv = gResolver->SetShortcut(exists,
                                mWorkingPath.get(),
                                targetFilePath,
                                workingDirPath,
                                args,
                                description,
                                iconFilePath,
                                iconFilePath? iconIndex : 0);
    if (targetFilePath && NS_SUCCEEDED(rv)) {
      MakeDirty();
    }

    return rv;
}

/** 
 * Determines if the drive type for the specified file is rmeote or local.
 * 
 * @param path   The path of the file to check
 * @param remote Out parameter, on function success holds true if the specified
 *               file path is remote, or false if the file path is local.
 * @return true  on success. The return value implies absolutely nothing about
 *               wether the file is local or remote.
*/
static bool
IsRemoteFilePath(LPCWSTR path, bool &remote)
{
  // Obtain the parent directory path and make sure it ends with
  // a trailing backslash.
  WCHAR dirPath[MAX_PATH + 1] = { 0 };
  wcsncpy(dirPath, path, MAX_PATH);
  if (!PathRemoveFileSpecW(dirPath)) {
    return false;
  }
  size_t len = wcslen(dirPath);
  // In case the dirPath holds exaclty MAX_PATH and remains unchanged, we
  // recheck the required length here since we need to terminate it with
  // a backslash.
  if (len >= MAX_PATH) {
    return false;
  }

  dirPath[len] = L'\\';
  dirPath[len + 1] = L'\0';
  UINT driveType = GetDriveTypeW(dirPath);
  remote = driveType == DRIVE_REMOTE;
  return true;
}

nsresult
nsLocalFile::CopySingleFile(nsIFile *sourceFile, nsIFile *destParent,
                            const nsAString &newName, 
                            bool followSymlinks, bool move,
                            bool skipNtfsAclReset)
{
    nsresult rv;
    nsAutoString filePath;

    // get the path that we are going to copy to.
    // Since windows does not know how to auto
    // resolve shortcuts, we must work with the
    // target.
    nsAutoString destPath;
    destParent->GetTarget(destPath);

    destPath.Append('\\');

    if (newName.IsEmpty())
    {
        nsAutoString aFileName;
        sourceFile->GetLeafName(aFileName);
        destPath.Append(aFileName);
    }
    else
    {
        destPath.Append(newName);
    }


    if (followSymlinks)
    {
        rv = sourceFile->GetTarget(filePath);
        if (filePath.IsEmpty())
            rv = sourceFile->GetPath(filePath);
    }
    else
    {
        rv = sourceFile->GetPath(filePath);
    }

    if (NS_FAILED(rv))
        return rv;

    // Pass the flag COPY_FILE_NO_BUFFERING to CopyFileEx as we may be copying
    // to a SMBV2 remote drive. Without this parameter subsequent append mode
    // file writes can cause the resultant file to become corrupt. We only need to do 
    // this if the major version of Windows is > 5(Only Windows Vista and above 
    // can support SMBV2).  With a 7200RPM hard drive:
    // Copying a 1KB file with COPY_FILE_NO_BUFFERING takes about 30-60ms.
    // Copying a 1KB file without COPY_FILE_NO_BUFFERING takes < 1ms.
    // So we only use COPY_FILE_NO_BUFFERING when we have a remote drive.
    int copyOK;
    DWORD dwVersion = GetVersion();
    DWORD dwMajorVersion = (DWORD)(LOBYTE(LOWORD(dwVersion)));
    DWORD dwCopyFlags = 0;
    if (dwMajorVersion > 5) {
        bool path1Remote, path2Remote;
        if (!IsRemoteFilePath(filePath.get(), path1Remote) || 
            !IsRemoteFilePath(destPath.get(), path2Remote) ||
            path1Remote || path2Remote) {
            dwCopyFlags |= COPY_FILE_NO_BUFFERING;
        }
    }
    
    if (!move)
    {
        copyOK = ::CopyFileExW(filePath.get(), destPath.get(), NULL, NULL, NULL, dwCopyFlags);
    }
    else {
        DWORD status;
        if (FileEncryptionStatusW(filePath.get(), &status)
            && status == FILE_IS_ENCRYPTED)
        {
            dwCopyFlags |= COPY_FILE_ALLOW_DECRYPTED_DESTINATION;
            copyOK = CopyFileExW(filePath.get(), destPath.get(), NULL, NULL, NULL, dwCopyFlags);

            if (copyOK)
                DeleteFileW(filePath.get());
        }
        else
        {
            copyOK = ::MoveFileExW(filePath.get(), destPath.get(),
                                   MOVEFILE_REPLACE_EXISTING |
                                   MOVEFILE_WRITE_THROUGH);
            
            // Check if copying the source file to a different volume,
            // as this could be an SMBV2 mapped drive.
            if (!copyOK && GetLastError() == ERROR_NOT_SAME_DEVICE)
            {
                copyOK = CopyFileExW(filePath.get(), destPath.get(), NULL, NULL, NULL, dwCopyFlags);
            
                if (copyOK)
                    DeleteFile(filePath.get());
            }
        }
    }

    if (!copyOK)  // CopyFileEx and MoveFileEx return zero at failure.
        rv = ConvertWinError(GetLastError());
    else if (move && !skipNtfsAclReset)
    {
        // Set security permissions to inherit from parent.
        // Note: propagates to all children: slow for big file trees
        PACL pOldDACL = NULL;
        PSECURITY_DESCRIPTOR pSD = NULL;
        ::GetNamedSecurityInfoW((LPWSTR)destPath.get(), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                NULL, NULL, &pOldDACL, NULL, &pSD);
        if (pOldDACL)
            ::SetNamedSecurityInfoW((LPWSTR)destPath.get(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION |
                                    UNPROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, pOldDACL, NULL);
        if (pSD)
            LocalFree((HLOCAL)pSD);
    }

    return rv;
}

nsresult
nsLocalFile::CopyMove(nsIFile *aParentDir, const nsAString &newName, bool followSymlinks, bool move)
{
    nsCOMPtr<nsIFile> newParentDir = aParentDir;
    // check to see if this exists, otherwise return an error.
    // we will check this by resolving.  If the user wants us
    // to follow links, then we are talking about the target,
    // hence we can use the |followSymlinks| parameter.
    nsresult rv  = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    if (!newParentDir)
    {
        // no parent was specified.  We must rename.
        if (newName.IsEmpty())
            return NS_ERROR_INVALID_ARG;

        rv = GetParent(getter_AddRefs(newParentDir));
        if (NS_FAILED(rv))
            return rv;
    }

    if (!newParentDir)
        return NS_ERROR_FILE_DESTINATION_NOT_DIR;

    // make sure it exists and is a directory.  Create it if not there.
    bool exists;
    newParentDir->Exists(&exists);
    if (!exists)
    {
        rv = newParentDir->Create(DIRECTORY_TYPE, 0644);  // TODO, what permissions should we use
        if (NS_FAILED(rv))
            return rv;
    }
    else
    {
        bool isDir;
        newParentDir->IsDirectory(&isDir);
        if (!isDir)
        {
            if (followSymlinks)
            {
                bool isLink;
                newParentDir->IsSymlink(&isLink);
                if (isLink)
                {
                    nsAutoString target;
                    newParentDir->GetTarget(target);

                    nsCOMPtr<nsIFile> realDest = new nsLocalFile();
                    if (realDest == nullptr)
                        return NS_ERROR_OUT_OF_MEMORY;

                    rv = realDest->InitWithPath(target);

                    if (NS_FAILED(rv))
                        return rv;

                    return CopyMove(realDest, newName, followSymlinks, move);
                }
            }
            else
            {
                return NS_ERROR_FILE_DESTINATION_NOT_DIR;
            }
        }
    }

    // Try different ways to move/copy files/directories
    bool done = false;
    bool isDir;
    IsDirectory(&isDir);
    bool isSymlink;
    IsSymlink(&isSymlink);

    // Try to move the file or directory, or try to copy a single file (or non-followed symlink)
    if (move || !isDir || (isSymlink && !followSymlinks))
    {
        // Copy/Move single file, or move a directory
        rv = CopySingleFile(this, newParentDir, newName, followSymlinks, move,
                            !aParentDir);
        done = NS_SUCCEEDED(rv);
        // If we are moving a directory and that fails, fallback on directory
        // enumeration.  See bug 231300 for details.
        if (!done && !(move && isDir))
            return rv;  
    }
    
    // Not able to copy or move directly, so enumerate it
    if (!done)
    {
        // create a new target destination in the new parentDir;
        nsCOMPtr<nsIFile> target;
        rv = newParentDir->Clone(getter_AddRefs(target));

        if (NS_FAILED(rv))
            return rv;

        nsAutoString allocatedNewName;
        if (newName.IsEmpty())
        {
            bool isLink;
            IsSymlink(&isLink);
            if (isLink)
            {
                nsAutoString temp;
                GetTarget(temp);
                int32_t offset = temp.RFindChar(L'\\'); 
                if (offset == kNotFound)
                    allocatedNewName = temp;
                else 
                    allocatedNewName = Substring(temp, offset + 1);
            }
            else
            {
                GetLeafName(allocatedNewName);// this should be the leaf name of the
            }
        }
        else
        {
            allocatedNewName = newName;
        }

        rv = target->Append(allocatedNewName);
        if (NS_FAILED(rv))
            return rv;

        allocatedNewName.Truncate();

        // check if the destination directory already exists
        target->Exists(&exists);
        if (!exists)
        {
            // if the destination directory cannot be created, return an error
            rv = target->Create(DIRECTORY_TYPE, 0644);  // TODO, what permissions should we use
            if (NS_FAILED(rv))
                return rv;
        }
        else
        {
            // check if the destination directory is writable and empty
            bool isWritable;

            target->IsWritable(&isWritable);
            if (!isWritable)
                return NS_ERROR_FILE_ACCESS_DENIED;

            nsCOMPtr<nsISimpleEnumerator> targetIterator;
            rv = target->GetDirectoryEntries(getter_AddRefs(targetIterator));
            if (NS_FAILED(rv))
                return rv;

            bool more;
            targetIterator->HasMoreElements(&more);
            // return error if target directory is not empty
            if (more)
                return NS_ERROR_FILE_DIR_NOT_EMPTY;
        }

        nsDirEnumerator dirEnum;

        rv = dirEnum.Init(this);
        if (NS_FAILED(rv)) {
            NS_WARNING("dirEnum initialization failed");
            return rv;
        }

        bool more = false;
        while (NS_SUCCEEDED(dirEnum.HasMoreElements(&more)) && more)
        {
            nsCOMPtr<nsISupports> item;
            nsCOMPtr<nsIFile> file;
            dirEnum.GetNext(getter_AddRefs(item));
            file = do_QueryInterface(item);
            if (file)
            {
                bool isDir, isLink;

                file->IsDirectory(&isDir);
                file->IsSymlink(&isLink);

                if (move)
                {
                    if (followSymlinks)
                        return NS_ERROR_FAILURE;

                    rv = file->MoveTo(target, EmptyString());
                    NS_ENSURE_SUCCESS(rv,rv);
                }
                else
                {
                    if (followSymlinks)
                        rv = file->CopyToFollowingLinks(target, EmptyString());
                    else
                        rv = file->CopyTo(target, EmptyString());
                    NS_ENSURE_SUCCESS(rv,rv);
                }
            }
        }
        // we've finished moving all the children of this directory
        // in the new directory.  so now delete the directory
        // note, we don't need to do a recursive delete.
        // MoveTo() is recursive.  At this point,
        // we've already moved the children of the current folder
        // to the new location.  nothing should be left in the folder.
        if (move)
        {
          rv = Remove(false /* recursive */);
          NS_ENSURE_SUCCESS(rv,rv);
        }
    }


    // If we moved, we want to adjust this.
    if (move)
    {
        MakeDirty();

        nsAutoString newParentPath;
        newParentDir->GetPath(newParentPath);

        if (newParentPath.IsEmpty())
            return NS_ERROR_FAILURE;

        if (newName.IsEmpty())
        {
            nsAutoString aFileName;
            GetLeafName(aFileName);

            InitWithPath(newParentPath);
            Append(aFileName);
        }
        else
        {
            InitWithPath(newParentPath);
            Append(newName);
        }
    }

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::CopyTo(nsIFile *newParentDir, const nsAString &newName)
{
    return CopyMove(newParentDir, newName, false, false);
}

NS_IMETHODIMP
nsLocalFile::CopyToFollowingLinks(nsIFile *newParentDir, const nsAString &newName)
{
    return CopyMove(newParentDir, newName, true, false);
}

NS_IMETHODIMP
nsLocalFile::MoveTo(nsIFile *newParentDir, const nsAString &newName)
{
    return CopyMove(newParentDir, newName, false, true);
}


NS_IMETHODIMP
nsLocalFile::Load(PRLibrary * *_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    bool isFile;
    nsresult rv = IsFile(&isFile);

    if (NS_FAILED(rv))
        return rv;

    if (! isFile)
        return NS_ERROR_FILE_IS_DIRECTORY;

#ifdef NS_BUILD_REFCNT_LOGGING
    nsTraceRefcntImpl::SetActivityIsLegal(false);
#endif

    PRLibSpec libSpec;
    libSpec.value.pathname_u = mResolvedPath.get();
    libSpec.type = PR_LibSpec_PathnameU;
    *_retval =  PR_LoadLibraryWithFlags(libSpec, 0);

#ifdef NS_BUILD_REFCNT_LOGGING
    nsTraceRefcntImpl::SetActivityIsLegal(true);
#endif

    if (*_retval)
        return NS_OK;
    return NS_ERROR_NULL_POINTER;
}

NS_IMETHODIMP
nsLocalFile::Remove(bool recursive)
{
    // NOTE:
    //
    // if the working path points to a shortcut, then we will only
    // delete the shortcut itself.  even if the shortcut points to
    // a directory, we will not recurse into that directory or 
    // delete that directory itself.  likewise, if the shortcut
    // points to a normal file, we will not delete the real file.
    // this is done to be consistent with the other platforms that
    // behave this way.  we do this even if the followLinks attribute
    // is set to true.  this helps protect against misuse that could
    // lead to security bugs (e.g., bug 210588).
    //
    // Since shortcut files are no longer permitted to be used as unix-like
    // symlinks interspersed in the path (e.g. "c:/file.lnk/foo/bar.txt")
    // this processing is a lot simpler. Even if the shortcut file is 
    // pointing to a directory, only the mWorkingPath value is used and so
    // only the shortcut file will be deleted.

    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    bool isDir, isLink;
    nsresult rv;
    
    isDir = false;
    rv = IsSymlink(&isLink);
    if (NS_FAILED(rv))
        return rv;

    // only check to see if we have a directory if it isn't a link
    if (!isLink)
    {
        rv = IsDirectory(&isDir);
        if (NS_FAILED(rv))
            return rv;
    }

    if (isDir)
    {
        if (recursive)
        {
            nsDirEnumerator dirEnum;

            rv = dirEnum.Init(this);
            if (NS_FAILED(rv))
                return rv;

            bool more = false;
            while (NS_SUCCEEDED(dirEnum.HasMoreElements(&more)) && more)
            {
                nsCOMPtr<nsISupports> item;
                dirEnum.GetNext(getter_AddRefs(item));
                nsCOMPtr<nsIFile> file = do_QueryInterface(item);
                if (file)
                    file->Remove(recursive);
            }
        }
        if (RemoveDirectoryW(mWorkingPath.get()) == 0)
            return ConvertWinError(GetLastError());
    }
    else
    {
        if (DeleteFileW(mWorkingPath.get()) == 0)
            return ConvertWinError(GetLastError());
    }

    MakeDirty();
    return rv;
}

NS_IMETHODIMP
nsLocalFile::GetLastModifiedTime(PRTime *aLastModifiedTime)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(aLastModifiedTime);
 
    // get the modified time of the target as determined by mFollowSymlinks
    // If true, then this will be for the target of the shortcut file, 
    // otherwise it will be for the shortcut file itself (i.e. the same 
    // results as GetLastModifiedTimeOfLink)

    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    // microseconds -> milliseconds
    *aLastModifiedTime = mFileInfo64.modifyTime / PR_USEC_PER_MSEC;
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::GetLastModifiedTimeOfLink(PRTime *aLastModifiedTime)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(aLastModifiedTime);
 
    // The caller is assumed to have already called IsSymlink 
    // and to have found that this file is a link. 

    PRFileInfo64 info;
    nsresult rv = GetFileInfo(mWorkingPath, &info);
    if (NS_FAILED(rv)) 
        return rv;

    // microseconds -> milliseconds
    *aLastModifiedTime = info.modifyTime / PR_USEC_PER_MSEC;
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::SetLastModifiedTime(PRTime aLastModifiedTime)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    // set the modified time of the target as determined by mFollowSymlinks
    // If true, then this will be for the target of the shortcut file, 
    // otherwise it will be for the shortcut file itself (i.e. the same 
    // results as SetLastModifiedTimeOfLink)

    rv = SetModDate(aLastModifiedTime, mResolvedPath.get());
    if (NS_SUCCEEDED(rv))
        MakeDirty();

    return rv;
}


NS_IMETHODIMP
nsLocalFile::SetLastModifiedTimeOfLink(PRTime aLastModifiedTime)
{
    // The caller is assumed to have already called IsSymlink 
    // and to have found that this file is a link. 

    nsresult rv = SetModDate(aLastModifiedTime, mWorkingPath.get());
    if (NS_SUCCEEDED(rv))
        MakeDirty();

    return rv;
}

nsresult
nsLocalFile::SetModDate(PRTime aLastModifiedTime, const PRUnichar *filePath)
{
    // The FILE_FLAG_BACKUP_SEMANTICS is required in order to change the
    // modification time for directories.
    HANDLE file = ::CreateFileW(filePath,          // pointer to name of the file
                                GENERIC_WRITE,     // access (write) mode
                                0,                 // share mode
                                NULL,              // pointer to security attributes
                                OPEN_EXISTING,     // how to create
                                FILE_FLAG_BACKUP_SEMANTICS,  // file attributes
                                NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        return ConvertWinError(GetLastError());
    }

    FILETIME ft;
    SYSTEMTIME st;
    PRExplodedTime pret;

    // PR_ExplodeTime expects usecs...
    PR_ExplodeTime(aLastModifiedTime * PR_USEC_PER_MSEC, PR_GMTParameters, &pret);
    st.wYear            = pret.tm_year;
    st.wMonth           = pret.tm_month + 1; // Convert start offset -- Win32: Jan=1; NSPR: Jan=0
    st.wDayOfWeek       = pret.tm_wday;
    st.wDay             = pret.tm_mday;
    st.wHour            = pret.tm_hour;
    st.wMinute          = pret.tm_min;
    st.wSecond          = pret.tm_sec;
    st.wMilliseconds    = pret.tm_usec/1000;

    nsresult rv = NS_OK;
    // if at least one of these fails...
    if (!(SystemTimeToFileTime(&st, &ft) != 0 &&
          SetFileTime(file, NULL, &ft, &ft) != 0))
    {
      rv = ConvertWinError(GetLastError());
    }

    CloseHandle(file);
    return rv;
}

NS_IMETHODIMP
nsLocalFile::GetPermissions(uint32_t *aPermissions)
{
    NS_ENSURE_ARG(aPermissions);

    // get the permissions of the target as determined by mFollowSymlinks
    // If true, then this will be for the target of the shortcut file, 
    // otherwise it will be for the shortcut file itself (i.e. the same 
    // results as GetPermissionsOfLink)
    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    bool isWritable, isExecutable;
    IsWritable(&isWritable);
    IsExecutable(&isExecutable);

    *aPermissions = PR_IRUSR|PR_IRGRP|PR_IROTH;         // all read
    if (isWritable)
        *aPermissions |= PR_IWUSR|PR_IWGRP|PR_IWOTH;    // all write
    if (isExecutable)
        *aPermissions |= PR_IXUSR|PR_IXGRP|PR_IXOTH;    // all execute

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetPermissionsOfLink(uint32_t *aPermissions)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(aPermissions);

    // The caller is assumed to have already called IsSymlink 
    // and to have found that this file is a link. It is not 
    // possible for a link file to be executable.

    DWORD word = ::GetFileAttributesW(mWorkingPath.get());
    if (word == INVALID_FILE_ATTRIBUTES)
        return NS_ERROR_FILE_INVALID_PATH;

    bool isWritable = !(word & FILE_ATTRIBUTE_READONLY);
    *aPermissions = PR_IRUSR|PR_IRGRP|PR_IROTH;         // all read
    if (isWritable)
        *aPermissions |= PR_IWUSR|PR_IWGRP|PR_IWOTH;    // all write

    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::SetPermissions(uint32_t aPermissions)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    // set the permissions of the target as determined by mFollowSymlinks
    // If true, then this will be for the target of the shortcut file, 
    // otherwise it will be for the shortcut file itself (i.e. the same 
    // results as SetPermissionsOfLink)
    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    // windows only knows about the following permissions
    int mode = 0;
    if (aPermissions & (PR_IRUSR|PR_IRGRP|PR_IROTH))    // any read
        mode |= _S_IREAD;
    if (aPermissions & (PR_IWUSR|PR_IWGRP|PR_IWOTH))    // any write
        mode |= _S_IWRITE;

    if (_wchmod(mResolvedPath.get(), mode) == -1)
        return NS_ERROR_FAILURE;

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetPermissionsOfLink(uint32_t aPermissions)
{
    // The caller is assumed to have already called IsSymlink 
    // and to have found that this file is a link. 

    // windows only knows about the following permissions
    int mode = 0;
    if (aPermissions & (PR_IRUSR|PR_IRGRP|PR_IROTH))    // any read
        mode |= _S_IREAD;
    if (aPermissions & (PR_IWUSR|PR_IWGRP|PR_IWOTH))    // any write
        mode |= _S_IWRITE;

    if (_wchmod(mWorkingPath.get(), mode) == -1)
        return NS_ERROR_FAILURE;

    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::GetFileSize(int64_t *aFileSize)
{
    NS_ENSURE_ARG(aFileSize);

    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    *aFileSize = mFileInfo64.size;
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::GetFileSizeOfLink(int64_t *aFileSize)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(aFileSize);

    // The caller is assumed to have already called IsSymlink 
    // and to have found that this file is a link. 

    PRFileInfo64 info;
    if (NS_FAILED(GetFileInfo(mWorkingPath, &info)))
        return NS_ERROR_FILE_INVALID_PATH;

    *aFileSize = info.size;
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::SetFileSize(int64_t aFileSize)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    HANDLE hFile = ::CreateFileW(mResolvedPath.get(),// pointer to name of the file
                                 GENERIC_WRITE,      // access (write) mode
                                 FILE_SHARE_READ,    // share mode
                                 NULL,               // pointer to security attributes
                                 OPEN_EXISTING,          // how to create
                                 FILE_ATTRIBUTE_NORMAL,  // file attributes
                                 NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return ConvertWinError(GetLastError());
    }

    // seek the file pointer to the new, desired end of file
    // and then truncate the file at that position
    rv = NS_ERROR_FAILURE;
    aFileSize = MyFileSeek64(hFile, aFileSize, FILE_BEGIN);
    if (aFileSize != -1 && SetEndOfFile(hFile))
    {
        MakeDirty();
        rv = NS_OK;
    }

    CloseHandle(hFile);
    return rv;
}

NS_IMETHODIMP
nsLocalFile::GetDiskSpaceAvailable(int64_t *aDiskSpaceAvailable)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(aDiskSpaceAvailable);

    ResolveAndStat();

    if (mFileInfo64.type == PR_FILE_FILE) {
      // Since GetDiskFreeSpaceExW works only on directories, use the parent.
      nsCOMPtr<nsIFile> parent;
      if (NS_SUCCEEDED(GetParent(getter_AddRefs(parent))) && parent) {
        return parent->GetDiskSpaceAvailable(aDiskSpaceAvailable);
      }
    }

    ULARGE_INTEGER liFreeBytesAvailableToCaller, liTotalNumberOfBytes;
    if (::GetDiskFreeSpaceExW(mResolvedPath.get(), &liFreeBytesAvailableToCaller, 
                              &liTotalNumberOfBytes, NULL))
    {
        *aDiskSpaceAvailable = liFreeBytesAvailableToCaller.QuadPart;
        return NS_OK;
    }
    *aDiskSpaceAvailable = 0;
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetParent(nsIFile * *aParent)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG_POINTER(aParent);

    // A two-character path must be a drive such as C:, so it has no parent
    if (mWorkingPath.Length() == 2) {
        *aParent = nullptr;
        return NS_OK;
    }

    int32_t offset = mWorkingPath.RFindChar(PRUnichar('\\'));
    // adding this offset check that was removed in bug 241708 fixes mail
    // directories that aren't relative to/underneath the profile dir.
    // e.g., on a different drive. Before you remove them, please make
    // sure local mail directories that aren't underneath the profile dir work.
    if (offset == kNotFound)
      return NS_ERROR_FILE_UNRECOGNIZED_PATH;

    // A path of the form \\NAME is a top-level path and has no parent
    if (offset == 1 && mWorkingPath[0] == L'\\') {
        *aParent = nullptr;
        return NS_OK;
    }

    nsAutoString parentPath(mWorkingPath);

    if (offset > 0)
        parentPath.Truncate(offset);
    else
        parentPath.AssignLiteral("\\\\.");

    nsCOMPtr<nsIFile> localFile;
    nsresult rv = NS_NewLocalFile(parentPath, mFollowSymlinks, getter_AddRefs(localFile));

    if (NS_SUCCEEDED(rv) && localFile) {
        return CallQueryInterface(localFile, aParent);
    }
    return rv;
}

NS_IMETHODIMP
nsLocalFile::Exists(bool *_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(_retval);
    *_retval = false;

    MakeDirty();
    nsresult rv = ResolveAndStat();
    *_retval = NS_SUCCEEDED(rv) || rv == NS_ERROR_FILE_IS_LOCKED;

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsWritable(bool *aIsWritable)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    // The read-only attribute on a FAT directory only means that it can't 
    // be deleted. It is still possible to modify the contents of the directory.
    nsresult rv = IsDirectory(aIsWritable);
    if (rv == NS_ERROR_FILE_ACCESS_DENIED) {
      *aIsWritable = true;
      return NS_OK;
    } else if (rv == NS_ERROR_FILE_IS_LOCKED) {
      // If the file is normally allowed write access
      // we should still return that the file is writable.
    } else if (NS_FAILED(rv)) {
        return rv;
    }
    if (*aIsWritable)
        return NS_OK;

    // writable if the file doesn't have the readonly attribute
    rv = HasFileAttribute(FILE_ATTRIBUTE_READONLY, aIsWritable);
    if (rv == NS_ERROR_FILE_ACCESS_DENIED) {
        *aIsWritable = false;
        return NS_OK;
    } else if (rv == NS_ERROR_FILE_IS_LOCKED) {
      // If the file is normally allowed write access
      // we should still return that the file is writable.
    } else if (NS_FAILED(rv)) {
        return rv;
    }
    *aIsWritable = !*aIsWritable;

    // If the read only attribute is not set, check to make sure
    // we can open the file with write access.
    if (*aIsWritable) {
        PRFileDesc* file;
        rv = OpenFile(mResolvedPath, PR_WRONLY, 0, &file);
        if (NS_SUCCEEDED(rv)) {
            PR_Close(file);
        } else if (rv == NS_ERROR_FILE_ACCESS_DENIED) {
          *aIsWritable = false;
        } else if (rv == NS_ERROR_FILE_IS_LOCKED) {
            // If it is locked and read only we would have 
            // gotten access denied
            *aIsWritable = true; 
        } else {
            return rv;
        }
    }
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsReadable(bool *_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(_retval);
    *_retval = false;

    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv))
        return rv;

    *_retval = true;
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::IsExecutable(bool *_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(_retval);
    *_retval = false;
    
    nsresult rv;

    // only files can be executables
    bool isFile;
    rv = IsFile(&isFile);
    if (NS_FAILED(rv))
        return rv;
    if (!isFile)
        return NS_OK;

    //TODO: shouldn't we be checking mFollowSymlinks here?
    bool symLink;
    rv = IsSymlink(&symLink);
    if (NS_FAILED(rv))
        return rv;

    nsAutoString path;
    if (symLink)
        GetTarget(path);
    else
        GetPath(path);

    // kill trailing dots and spaces.
    int32_t filePathLen = path.Length() - 1;
    while(filePathLen > 0 && (path[filePathLen] == L' ' || path[filePathLen] == L'.'))
    {
        path.Truncate(filePathLen--);
    } 

    // Get extension.
    int32_t dotIdx = path.RFindChar(PRUnichar('.'));
    if ( dotIdx != kNotFound ) {
        // Convert extension to lower case.
        PRUnichar *p = path.BeginWriting();
        for( p+= dotIdx + 1; *p; p++ )
            *p +=  (*p >= L'A' && *p <= L'Z') ? 'a' - 'A' : 0; 
        
        // Search for any of the set of executable extensions.
        static const char * const executableExts[] = {
            "ad",
            "ade",         // access project extension
            "adp",
            "air",         // Adobe AIR installer
            "app",         // executable application
            "application", // from bug 348763
            "asp",
            "bas",
            "bat",
            "chm",
            "cmd",
            "com",
            "cpl",
            "crt",
            "exe",
            "fxp",         // FoxPro compiled app
            "hlp",
            "hta",
            "inf",
            "ins",
            "isp",
            "jar",         // java application bundle
            "js",
            "jse",
            "lnk",
            "mad",         // Access Module Shortcut
            "maf",         // Access
            "mag",         // Access Diagram Shortcut
            "mam",         // Access Macro Shortcut
            "maq",         // Access Query Shortcut
            "mar",         // Access Report Shortcut
            "mas",         // Access Stored Procedure
            "mat",         // Access Table Shortcut
            "mau",         // Media Attachment Unit
            "mav",         // Access View Shortcut
            "maw",         // Access Data Access Page
            "mda",         // Access Add-in, MDA Access 2 Workgroup
            "mdb",
            "mde",
            "mdt",         // Access Add-in Data
            "mdw",         // Access Workgroup Information
            "mdz",         // Access Wizard Template
            "msc",
            "msh",         // Microsoft Shell
            "mshxml",      // Microsoft Shell
            "msi",
            "msp",
            "mst",
            "ops",         // Office Profile Settings
            "pcd",
            "pif",
            "plg",         // Developer Studio Build Log
            "prf",         // windows system file
            "prg",
            "pst",
            "reg",
            "scf",         // Windows explorer command
            "scr",
            "sct",
            "shb",
            "shs",
            "url",
            "vb",
            "vbe",
            "vbs",
            "vsd",
            "vsmacros",    // Visual Studio .NET Binary-based Macro Project
            "vss",
            "vst",
            "vsw",
            "ws",
            "wsc",
            "wsf",
            "wsh"};
        nsDependentSubstring ext = Substring(path, dotIdx + 1);
        for ( size_t i = 0; i < ArrayLength(executableExts); i++ ) {
            if ( ext.EqualsASCII(executableExts[i])) {
                // Found a match.  Set result and quit.
                *_retval = true;
                break;
            }
        }
    }

    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::IsDirectory(bool *_retval)
{
    return HasFileAttribute(FILE_ATTRIBUTE_DIRECTORY, _retval);
}

NS_IMETHODIMP
nsLocalFile::IsFile(bool *_retval)
{
    nsresult rv = HasFileAttribute(FILE_ATTRIBUTE_DIRECTORY, _retval);
    if (NS_SUCCEEDED(rv)) {
        *_retval = !*_retval;
    }
    return rv;
}

NS_IMETHODIMP
nsLocalFile::IsHidden(bool *_retval)
{
    return HasFileAttribute(FILE_ATTRIBUTE_HIDDEN, _retval);
}

nsresult
nsLocalFile::HasFileAttribute(DWORD fileAttrib, bool *_retval)
{
    NS_ENSURE_ARG(_retval);

    nsresult rv = Resolve();
    if (NS_FAILED(rv)) {
        return rv;
    }

    DWORD attributes = GetFileAttributesW(mResolvedPath.get());
    if (INVALID_FILE_ATTRIBUTES == attributes) {
        return ConvertWinError(GetLastError());
    }

    *_retval = ((attributes & fileAttrib) != 0);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsSymlink(bool *_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_ENSURE_ARG(_retval);

    // unless it is a valid shortcut path it's not a symlink
    if (!IsShortcutPath(mWorkingPath)) {
        *_retval = false;
        return NS_OK;
    }

    // we need to know if this is a file or directory
    nsresult rv = ResolveAndStat();
    if (NS_FAILED(rv)) {
        return rv;
    }

    // We should not check mFileInfo64.type here for PR_FILE_FILE because lnk
    // files can point to directories or files.  Important security checks
    // depend on correctly identifying lnk files.  mFileInfo64 now holds info
    // about the target of the lnk file, not the actual lnk file!
    *_retval = true;
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::IsSpecial(bool *_retval)
{
    return HasFileAttribute(FILE_ATTRIBUTE_SYSTEM, _retval);
}

NS_IMETHODIMP
nsLocalFile::Equals(nsIFile *inFile, bool *_retval)
{
    NS_ENSURE_ARG(inFile);
    NS_ENSURE_ARG(_retval);

    EnsureShortPath();

    nsCOMPtr<nsILocalFileWin> lf(do_QueryInterface(inFile));
    if (!lf) {
        *_retval = false;
        return NS_OK;
    }

    nsAutoString inFilePath;
    lf->GetCanonicalPath(inFilePath);

    // Ok : Win9x
    *_retval = _wcsicmp(mShortWorkingPath.get(), inFilePath.get()) == 0; 

    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::Contains(nsIFile *inFile, bool recur, bool *_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    *_retval = false;

    nsAutoString myFilePath;
    if (NS_FAILED(GetTarget(myFilePath)))
        GetPath(myFilePath);

    uint32_t myFilePathLen = myFilePath.Length();

    nsAutoString inFilePath;
    if (NS_FAILED(inFile->GetTarget(inFilePath)))
        inFile->GetPath(inFilePath);

    // make sure that the |inFile|'s path has a trailing separator.
    if (inFilePath.Length() >= myFilePathLen && inFilePath[myFilePathLen] == L'\\')
    {
        if (_wcsnicmp(myFilePath.get(), inFilePath.get(), myFilePathLen) == 0)
        {
            *_retval = true;
        }

    }

    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::GetTarget(nsAString &_retval)
{
    _retval.Truncate();
#if STRICT_FAKE_SYMLINKS
    bool symLink;

    nsresult rv = IsSymlink(&symLink);
    if (NS_FAILED(rv))
        return rv;

    if (!symLink)
    {
        return NS_ERROR_FILE_INVALID_PATH;
    }
#endif
    ResolveAndStat();

    _retval = mResolvedPath;
    return NS_OK;
}


/* attribute bool followLinks; */
NS_IMETHODIMP
nsLocalFile::GetFollowLinks(bool *aFollowLinks)
{
    *aFollowLinks = mFollowSymlinks;
    return NS_OK;
}
NS_IMETHODIMP
nsLocalFile::SetFollowLinks(bool aFollowLinks)
{
    MakeDirty();
    mFollowSymlinks = aFollowLinks;
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::GetDirectoryEntries(nsISimpleEnumerator * *entries)
{
    nsresult rv;

    *entries = nullptr;
    if (mWorkingPath.EqualsLiteral("\\\\.")) {
        nsDriveEnumerator *drives = new nsDriveEnumerator;
        if (!drives)
            return NS_ERROR_OUT_OF_MEMORY;
        NS_ADDREF(drives);
        rv = drives->Init();
        if (NS_FAILED(rv)) {
            NS_RELEASE(drives);
            return rv;
        }
        *entries = drives;
        return NS_OK;
    }

    nsDirEnumerator* dirEnum = new nsDirEnumerator();
    if (dirEnum == nullptr)
        return NS_ERROR_OUT_OF_MEMORY;
    NS_ADDREF(dirEnum);
    rv = dirEnum->Init(this);
    if (NS_FAILED(rv))
    {
        NS_RELEASE(dirEnum);
        return rv;
    }

    *entries = dirEnum;

    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::GetPersistentDescriptor(nsACString &aPersistentDescriptor)
{
    CopyUTF16toUTF8(mWorkingPath, aPersistentDescriptor);
    return NS_OK;
}   
    
NS_IMETHODIMP
nsLocalFile::SetPersistentDescriptor(const nsACString &aPersistentDescriptor)
{
    if (IsUTF8(aPersistentDescriptor))
        return InitWithPath(NS_ConvertUTF8toUTF16(aPersistentDescriptor));
    else
        return InitWithNativePath(aPersistentDescriptor);
}   

/* attrib unsigned long fileAttributesWin; */
static bool IsXPOrGreater()
{
    OSVERSIONINFO osvi;

    ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

    GetVersionEx(&osvi);

    return ((osvi.dwMajorVersion > 5) ||
       ((osvi.dwMajorVersion == 5) && (osvi.dwMinorVersion >= 1)));
}

NS_IMETHODIMP
nsLocalFile::GetFileAttributesWin(uint32_t *aAttribs)
{
    *aAttribs = 0;
    DWORD dwAttrs = GetFileAttributesW(mWorkingPath.get());
    if (dwAttrs == INVALID_FILE_ATTRIBUTES)
      return NS_ERROR_FILE_INVALID_PATH;

    if (!(dwAttrs & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED))
        *aAttribs |= WFA_SEARCH_INDEXED;

    return NS_OK;
}   
    
NS_IMETHODIMP
nsLocalFile::SetFileAttributesWin(uint32_t aAttribs)
{
    DWORD dwAttrs = GetFileAttributesW(mWorkingPath.get());
    if (dwAttrs == INVALID_FILE_ATTRIBUTES)
      return NS_ERROR_FILE_INVALID_PATH;

    if (IsXPOrGreater()) {
      if (aAttribs & WFA_SEARCH_INDEXED) {
          dwAttrs &= ~FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
      } else {
          dwAttrs |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
      }
    }

    if (aAttribs & WFA_READONLY) {
      dwAttrs |= FILE_ATTRIBUTE_READONLY;
    } else if ((aAttribs & WFA_READWRITE) &&
               (dwAttrs & FILE_ATTRIBUTE_READONLY)) {
      dwAttrs &= ~FILE_ATTRIBUTE_READONLY;
    }

    if (SetFileAttributesW(mWorkingPath.get(), dwAttrs) == 0)
      return NS_ERROR_FAILURE;
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::Reveal()
{
    // This API should be main thread only
    MOZ_ASSERT(NS_IsMainThread()); 

    // make sure mResolvedPath is set
    nsresult rv = Resolve();
    if (NS_FAILED(rv) && rv != NS_ERROR_FILE_NOT_FOUND) {
        return rv;
    }

    // To create a new thread, get the thread manager
    nsCOMPtr<nsIThreadManager> tm = do_GetService(NS_THREADMANAGER_CONTRACTID);
    nsCOMPtr<nsIThread> mythread;
    rv = tm->NewThread(0, 0, getter_AddRefs(mythread));
    if (NS_FAILED(rv)) {
        return rv;
    }

    nsCOMPtr<nsIRunnable> runnable = 
        new AsyncLocalFileWinOperation(AsyncLocalFileWinOperation::RevealOp,
                                       mResolvedPath);

    // After the dispatch, the result runnable will shut down the worker
    // thread, so we can let it go.
    mythread->Dispatch(runnable, NS_DISPATCH_NORMAL);
    return NS_OK;
}

NS_IMETHODIMP
nsLocalFile::Launch()
{
    // This API should be main thread only
    MOZ_ASSERT(NS_IsMainThread()); 

    // make sure mResolvedPath is set
    nsresult rv = Resolve();
    if (NS_FAILED(rv))
        return rv;

    // To create a new thread, get the thread manager
    nsCOMPtr<nsIThreadManager> tm = do_GetService(NS_THREADMANAGER_CONTRACTID);
    nsCOMPtr<nsIThread> mythread;
    rv = tm->NewThread(0, 0, getter_AddRefs(mythread));
    if (NS_FAILED(rv)) {
        return rv;
    }

    nsCOMPtr<nsIRunnable> runnable = 
        new AsyncLocalFileWinOperation(AsyncLocalFileWinOperation::LaunchOp,
                                       mResolvedPath);

    // After the dispatch, the result runnable will shut down the worker
    // thread, so we can let it go.
    mythread->Dispatch(runnable, NS_DISPATCH_NORMAL);
    return NS_OK;
}

nsresult
NS_NewLocalFile(const nsAString &path, bool followLinks, nsIFile* *result)
{
    nsLocalFile* file = new nsLocalFile();
    if (file == nullptr)
        return NS_ERROR_OUT_OF_MEMORY;
    NS_ADDREF(file);
    
    file->SetFollowLinks(followLinks);

    if (!path.IsEmpty()) {
        nsresult rv = file->InitWithPath(path);
        if (NS_FAILED(rv)) {
            NS_RELEASE(file);
            return rv;
        }
    }

    *result = file;
    return NS_OK;
}

//-----------------------------------------------------------------------------
// Native (lossy) interface
//-----------------------------------------------------------------------------
  
NS_IMETHODIMP
nsLocalFile::InitWithNativePath(const nsACString &filePath)
{
   nsAutoString tmp;
   nsresult rv = NS_CopyNativeToUnicode(filePath, tmp);
   if (NS_SUCCEEDED(rv))
       return InitWithPath(tmp);

   return rv;
}

NS_IMETHODIMP
nsLocalFile::AppendNative(const nsACString &node)
{
    nsAutoString tmp;
    nsresult rv = NS_CopyNativeToUnicode(node, tmp);
    if (NS_SUCCEEDED(rv))
        return Append(tmp);

    return rv;
}

NS_IMETHODIMP
nsLocalFile::AppendRelativeNativePath(const nsACString &node)
{
    nsAutoString tmp;
    nsresult rv = NS_CopyNativeToUnicode(node, tmp);
    if (NS_SUCCEEDED(rv))
        return AppendRelativePath(tmp);
    return rv;
}


NS_IMETHODIMP
nsLocalFile::GetNativeLeafName(nsACString &aLeafName)
{
    //NS_WARNING("This API is lossy. Use GetLeafName !");
    nsAutoString tmp;
    nsresult rv = GetLeafName(tmp);
    if (NS_SUCCEEDED(rv))
        rv = NS_CopyUnicodeToNative(tmp, aLeafName);

    return rv;
}

NS_IMETHODIMP
nsLocalFile::SetNativeLeafName(const nsACString &aLeafName)
{
    nsAutoString tmp;
    nsresult rv = NS_CopyNativeToUnicode(aLeafName, tmp);
    if (NS_SUCCEEDED(rv))
        return SetLeafName(tmp);

    return rv;
}


NS_IMETHODIMP
nsLocalFile::GetNativePath(nsACString &_retval)
{
    //NS_WARNING("This API is lossy. Use GetPath !");
    nsAutoString tmp;
    nsresult rv = GetPath(tmp);
    if (NS_SUCCEEDED(rv))
        rv = NS_CopyUnicodeToNative(tmp, _retval);

    return rv;
}


NS_IMETHODIMP
nsLocalFile::GetNativeCanonicalPath(nsACString &aResult)
{
    NS_WARNING("This method is lossy. Use GetCanonicalPath !");
    EnsureShortPath();
    NS_CopyUnicodeToNative(mShortWorkingPath, aResult);
    return NS_OK;
}


NS_IMETHODIMP
nsLocalFile::CopyToNative(nsIFile *newParentDir, const nsACString &newName)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    if (newName.IsEmpty())
        return CopyTo(newParentDir, EmptyString());

    nsAutoString tmp;
    nsresult rv = NS_CopyNativeToUnicode(newName, tmp);
    if (NS_SUCCEEDED(rv))
        return CopyTo(newParentDir, tmp);

    return rv;
}

NS_IMETHODIMP
nsLocalFile::CopyToFollowingLinksNative(nsIFile *newParentDir, const nsACString &newName)
{
    if (newName.IsEmpty())
        return CopyToFollowingLinks(newParentDir, EmptyString());

    nsAutoString tmp;
    nsresult rv = NS_CopyNativeToUnicode(newName, tmp);
    if (NS_SUCCEEDED(rv))
        return CopyToFollowingLinks(newParentDir, tmp);

    return rv;
}

NS_IMETHODIMP
nsLocalFile::MoveToNative(nsIFile *newParentDir, const nsACString &newName)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    if (newName.IsEmpty())
        return MoveTo(newParentDir, EmptyString());

    nsAutoString tmp;
    nsresult rv = NS_CopyNativeToUnicode(newName, tmp);
    if (NS_SUCCEEDED(rv))
        return MoveTo(newParentDir, tmp);

    return rv;
}

NS_IMETHODIMP
nsLocalFile::GetNativeTarget(nsACString &_retval)
{
    // Check we are correctly initialized.
    CHECK_mWorkingPath();

    NS_WARNING("This API is lossy. Use GetTarget !");
    nsAutoString tmp;
    nsresult rv = GetTarget(tmp);
    if (NS_SUCCEEDED(rv))
        rv = NS_CopyUnicodeToNative(tmp, _retval);

    return rv;
}

nsresult
NS_NewNativeLocalFile(const nsACString &path, bool followLinks, nsIFile* *result)
{
    nsAutoString buf;
    nsresult rv = NS_CopyNativeToUnicode(path, buf);
    if (NS_FAILED(rv)) {
        *result = nullptr;
        return rv;
    }
    return NS_NewLocalFile(buf, followLinks, result);
}

void
nsLocalFile::EnsureShortPath()
{
    if (!mShortWorkingPath.IsEmpty())
        return;

    WCHAR shortPath[MAX_PATH + 1];
    DWORD lengthNeeded = ::GetShortPathNameW(mWorkingPath.get(), shortPath,
                                             ArrayLength(shortPath));
    // If an error occurred then lengthNeeded is set to 0 or the length of the
    // needed buffer including NULL termination.  If it succeeds the number of
    // wide characters not including NULL termination is returned.
    if (lengthNeeded != 0 && lengthNeeded < ArrayLength(shortPath))
        mShortWorkingPath.Assign(shortPath);
    else
        mShortWorkingPath.Assign(mWorkingPath);
}

// nsIHashable

NS_IMETHODIMP
nsLocalFile::Equals(nsIHashable* aOther, bool *aResult)
{
    nsCOMPtr<nsIFile> otherfile(do_QueryInterface(aOther));
    if (!otherfile) {
        *aResult = false;
        return NS_OK;
    }

    return Equals(otherfile, aResult);
}

NS_IMETHODIMP
nsLocalFile::GetHashCode(uint32_t *aResult)
{
    // In order for short and long path names to hash to the same value we
    // always hash on the short pathname.
    EnsureShortPath();

    *aResult = HashString(mShortWorkingPath);
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsLocalFile <static members>
//-----------------------------------------------------------------------------

void
nsLocalFile::GlobalInit()
{
    DebugOnly<nsresult> rv = NS_CreateShortcutResolver();
    NS_ASSERTION(NS_SUCCEEDED(rv), "Shortcut resolver could not be created");
}

void
nsLocalFile::GlobalShutdown()
{
    NS_DestroyShortcutResolver();
}

NS_IMPL_ISUPPORTS1(nsDriveEnumerator, nsISimpleEnumerator)

nsDriveEnumerator::nsDriveEnumerator()
{
}

nsDriveEnumerator::~nsDriveEnumerator()
{
}

nsresult nsDriveEnumerator::Init()
{
    /* If the length passed to GetLogicalDriveStrings is smaller
     * than the length of the string it would return, it returns
     * the length required for the string. */
    DWORD length = GetLogicalDriveStringsW(0, 0);
    /* The string is null terminated */
    if (!mDrives.SetLength(length+1, fallible_t()))
        return NS_ERROR_OUT_OF_MEMORY;
    if (!GetLogicalDriveStringsW(length, mDrives.BeginWriting()))
        return NS_ERROR_FAILURE;
    mDrives.BeginReading(mStartOfCurrentDrive);
    mDrives.EndReading(mEndOfDrivesString);
    return NS_OK;
}

NS_IMETHODIMP nsDriveEnumerator::HasMoreElements(bool *aHasMore)
{
    *aHasMore = *mStartOfCurrentDrive != L'\0';
    return NS_OK;
}

NS_IMETHODIMP nsDriveEnumerator::GetNext(nsISupports **aNext)
{
    /* GetLogicalDrives stored in mDrives is a concatenation
     * of null terminated strings, followed by a null terminator.
     * mStartOfCurrentDrive is an iterator pointing at the first
     * character of the current drive. */
    if (*mStartOfCurrentDrive == L'\0') {
        *aNext = nullptr;
        return NS_OK;
    }

    nsAString::const_iterator driveEnd = mStartOfCurrentDrive;
    FindCharInReadable(L'\0', driveEnd, mEndOfDrivesString);
    nsString drive(Substring(mStartOfCurrentDrive, driveEnd));
    mStartOfCurrentDrive = ++driveEnd;

    nsIFile *file;
    nsresult rv = NS_NewLocalFile(drive, false, &file);

    *aNext = file;
    return rv;
}
