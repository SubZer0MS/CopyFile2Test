#include <iostream>
#include <Windows.h>

HANDLE g_CancelEvent;

typedef struct _FILECOPYINFO
{
    int progress = 0;
    bool bOffloaded = false;
    bool bCancelled = false;
    LONGLONG llBytesTransferred = 0;
    LONGLONG llBytesTotal = 0;
    CRITICAL_SECTION csLock;
    HANDLE hCopyThread = INVALID_HANDLE_VALUE;
    PCWSTR szSourchPath;
    PCWSTR szDestinationPath;
} FILECOPYINFO, *PFILECOPYINFO;

bool WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT ||
        signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT
        )
    {
        SetEvent(g_CancelEvent);
    }

    return true;
}

COPYFILE2_MESSAGE_ACTION CALLBACK FileCopyProgressRoutine(
    _In_ const COPYFILE2_MESSAGE* pMessage,
    _In_opt_ PVOID pvCallbackContext)
{
    PFILECOPYINFO pFileCopyInfo = reinterpret_cast<PFILECOPYINFO>(pvCallbackContext);

    if (pMessage->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED)
    {
        LONGLONG bytesTransferred = (LONGLONG)pMessage->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        LONGLONG bytesTotal = (LONGLONG)pMessage->Info.ChunkFinished.uliTotalFileSize.QuadPart;

        EnterCriticalSection(&pFileCopyInfo->csLock);
        pFileCopyInfo->progress = (int)(bytesTotal > 0 ? ((LONGLONG)99) * bytesTransferred / bytesTotal : 99);
        pFileCopyInfo->llBytesTransferred = bytesTransferred <= bytesTotal ? bytesTransferred : bytesTotal;
        pFileCopyInfo->llBytesTotal = bytesTotal;
        pFileCopyInfo->bOffloaded &= (pMessage->Info.ChunkFinished.dwFlags & COPYFILE2_MESSAGE_COPY_OFFLOAD);
        LeaveCriticalSection(&pFileCopyInfo->csLock);

        std::wcout << L"Copy progress is " << pFileCopyInfo->progress << L"% and Offloading is " << (pFileCopyInfo->bOffloaded ? L"TRUE" : L"FALSE") << std::endl;
    }

    return pFileCopyInfo->bCancelled ? COPYFILE2_PROGRESS_CANCEL : COPYFILE2_PROGRESS_CONTINUE;
}

DWORD WINAPI FileCopyThread(
    __inout LPVOID lpThreadParameter)
{
    PFILECOPYINFO pFileCopyInfo = reinterpret_cast<PFILECOPYINFO>(lpThreadParameter);

    COPYFILE2_EXTENDED_PARAMETERS exParams;
    exParams.dwSize = sizeof(exParams);
    exParams.dwCopyFlags = 0UL;
    exParams.pProgressRoutine = FileCopyProgressRoutine;
    exParams.pfCancel = (LPBOOL)pFileCopyInfo->bCancelled;
    exParams.pvCallbackContext = (LPVOID)pFileCopyInfo;

    HRESULT status = CopyFile2(pFileCopyInfo->szSourchPath, pFileCopyInfo->szDestinationPath, &exParams);

    if (!SUCCEEDED(status))
    {
        std::wcerr << L"ERROR: CopFile2 failed." << std::endl;

        return E_FAIL;
    }

    pFileCopyInfo->progress = 100;
    std::wcout << L"Copy progress is " << pFileCopyInfo->progress << L"% and Offloading is " << (pFileCopyInfo->bOffloaded ? L"TRUE" : L"FALSE") << std::endl;

    return ERROR_SUCCESS;
}

DWORD wmain(int argc, PWCHAR argv[])
{
    DWORD status = ERROR_SUCCESS;

    if (argc != 3)
    {
        std::wcerr << L"ERROR: Invalid number of arguments passed. Needs Source and Destination as arguments." << std::endl;
        return ERROR_INVALID_PARAMETER;
    }

    std::wstring sourcePath = argv[1];
    std::wstring destinationPath = argv[2];

    FILECOPYINFO fileCopyInfo;
    fileCopyInfo.szSourchPath = sourcePath.c_str();
    fileCopyInfo.szDestinationPath = destinationPath.c_str();

    InitializeCriticalSection(&fileCopyInfo.csLock);

    fileCopyInfo.hCopyThread = CreateThread(NULL, NULL, FileCopyThread, (LPVOID)&fileCopyInfo, 0, nullptr);

    if (fileCopyInfo.hCopyThread == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"ERROR: Failed to create the Copy Thread." << std::endl;
        status = GetLastError();
    }
    else
    {
        g_CancelEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (g_CancelEvent == NULL)
        {
            std::wcerr << L"ERROR: Failed to create Cancel Event." << std::endl;
            status = GetLastError();
        }

        if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE))
        {
            std::wcerr << L"ERROR: Failed to set Console Handler." << std::endl;
            status = GetLastError();
        }

        HANDLE waitObjects[2];
        waitObjects[0] = fileCopyInfo.hCopyThread;
        waitObjects[1] = g_CancelEvent;

        status = WaitForMultipleObjects(2, waitObjects, FALSE, INFINITE);

        if (status == WAIT_OBJECT_0)
        {
            std::wcout << L"Copy thread finished." << std::endl;
        }
        else if (status == WAIT_OBJECT_0 + 1)
        {
            fileCopyInfo.bCancelled = true;
            WaitForSingleObject(fileCopyInfo.hCopyThread, INFINITE);
            std::wcout << L"Copy operation has been cancelled by the user." << std::endl;
        }

        GetExitCodeThread(fileCopyInfo.hCopyThread, &status);
        
        if (status == COPYFILE2_PROGRESS_CANCEL)
        {            
            if (!DeleteFile(fileCopyInfo.szDestinationPath))
            {
                std::wcerr << L"ERROR: Failed to delete the destination file when operation has been cancelled." << std::endl;
                status = GetLastError();
            }
        }

        CloseHandle(fileCopyInfo.hCopyThread);
    }

    DeleteCriticalSection(&fileCopyInfo.csLock);

    if (g_CancelEvent)
    {
        CloseHandle(g_CancelEvent);
    }
    
    return status;
}

