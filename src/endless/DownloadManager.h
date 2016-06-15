#pragma once

#include <windows.h>
#include <bits.h>
#include <initializer_list>

#include "GeneralCode.h"

/// IUnknown methods declaration macro
#define DECLARE_IUNKNOWN \
    STDMETHOD_(ULONG, AddRef)(void); \
    STDMETHOD_(ULONG, Release)(void); \
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppvObj);

typedef enum DownloadType {
    DownloadTypeReleseJson,
    DownloadTypeLiveImage,
    DownloadTypeInstallerImage,
    DownloadTypeMax
} DownloadType_t;

static LPCTSTR DownloadTypeToString(DownloadType_t type)
{
    switch (type)
    {
        TOSTR(DownloadTypeReleseJson);
        TOSTR(DownloadTypeLiveImage);
        TOSTR(DownloadTypeInstallerImage);
        default: return _T("UNKNOWN_DOWNLOAD_TYPE");
    }
}

typedef struct DownloadStatus {
    BG_JOB_PROGRESS progress;    
    bool done;
    bool error;
    CString jobName;
    BG_ERROR_CONTEXT errorContext;
    HRESULT errorCode;
} DownloadStatus_t;

typedef std::initializer_list<LPCTSTR> ListOfStrings;

static const DownloadStatus_t DownloadStatusNull = { {0, 0, 0, 0}, false, false, L"" };

class DownloadManager : public IBackgroundCopyCallback {
public:

    DownloadManager();
    ~DownloadManager();

    bool Init(HWND window, DWORD statusMessageId);
    bool Uninit();

    bool AddDownload(DownloadType_t type, ListOfStrings urls, ListOfStrings files, bool resumeExisting, LPCTSTR jobSuffix = NULL);

    static CString GetJobName(DownloadType_t type);

    static bool GetDownloadProgress(CComPtr<IBackgroundCopyJob> &currentJob, DownloadStatus_t *downloadStatus, const CString &jobName);
    static HRESULT GetExistingJob(CComPtr<IBackgroundCopyManager> &bcManager, LPCWSTR jobName, CComPtr<IBackgroundCopyJob> &existingJob);

    void ClearExtraDownloadJobs();

    // IUnknown implementation
    DECLARE_IUNKNOWN;

    // IBackgroundCopyCallback implementation
    STDMETHOD(JobTransferred)(IBackgroundCopyJob *pJob);
    STDMETHOD(JobError)(IBackgroundCopyJob *pJob, IBackgroundCopyError *pError);
    STDMETHOD(JobModification)(IBackgroundCopyJob *JobModification, DWORD dwReserved);

    void SetLatestEosVersion(CString latestEosVersion)
    {
        m_latestEosVersion = latestEosVersion;
    }

private:
    LONG m_PendingJobModificationCount;
    static volatile ULONG m_refCount;
    HWND m_dispatchWindow;
    DWORD m_statusMsgId;
    CComPtr<IBackgroundCopyManager> m_bcManager;
    CComPtr<IBackgroundCopyJob> m_bcReleaseJson;
    CString m_latestEosVersion;
};