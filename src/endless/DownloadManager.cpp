#include "stdafx.h"
#include "DownloadManager.h"
extern "C" {
    #include "rufus.h"
}

#define DOWNLOAD_JOB_PREFIX _T("Endless")

volatile ULONG DownloadManager::m_refCount = 0;

DownloadManager::DownloadManager()
{
    m_bcManager = NULL;
    m_PendingJobModificationCount = 0;
}

DownloadManager::~DownloadManager()
{
}

bool DownloadManager::Init(HWND window, DWORD statusMessageId)
{
    CComPtr<IEnumBackgroundCopyJobs> enumJobs;
    CComPtr<IBackgroundCopyJob> job;
    ULONG jobCount = 0;
    HRESULT hr = S_OK;

    m_dispatchWindow = window;
    m_statusMsgId = statusMessageId;

    IFFALSE_GOTOERROR(m_bcManager == NULL, "Manager already initialized.");

    // Specify the appropriate COM threading model for your application.
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error on calling CoInitializeEx");

    hr = CoCreateInstance(__uuidof(BackgroundCopyManager), NULL, CLSCTX_LOCAL_SERVER, __uuidof(IBackgroundCopyManager), (void**)&m_bcManager);
    IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error creating instance of BackgroundCopyManager.");

    ClearExtraDownloadJobs();    

    return true;
error:
    uprintf("DownloadManager::Init failed with hr = 0x%x, last error = %s", hr, WindowsErrorString());
    return false;
}

bool DownloadManager::Uninit()
{
    IFFALSE_GOTOERROR(m_bcManager != NULL,  "Manager or job is NULL. Nothing to uninit.");
    
    m_dispatchWindow = NULL;
    ClearExtraDownloadJobs();
    m_bcManager = NULL;

    //RADU: what else ?
    return S_OK;

error:
    return false;
}

bool DownloadManager::AddDownload(DownloadType_t type, LPCTSTR url, LPCTSTR file, bool startDownload, bool *appendFile, LPCTSTR jobSuffix)
{
    LPWSTR local = NULL, remote = NULL;
    CComPtr<IBackgroundCopyJob> currentJob;
    GUID jobId;
    HRESULT hr = S_OK;
    ULONG count = 0;
    CString jobName;
    bool jobExisted = false;

    IFFALSE_GOTOERROR(m_bcManager != NULL, "Manager is NULL. Was Init called?");

    uprintf("DownloadManager::AddDownload (%ls, %ls)", url, file);

    jobName = DownloadManager::GetJobName(type);
    if (jobSuffix != NULL) jobName += jobSuffix;

    if(type != DownloadType_t::DownloadTypeReleseJson && jobSuffix == NULL) {
        uprintf("Error: AddDownload NO SUFFIX ADDED");
    }

    hr = GetExistingJob(jobName, currentJob);
    if (SUCCEEDED(hr) && type == DownloadType_t::DownloadTypeReleseJson) {
        hr = currentJob->Cancel();
        currentJob = NULL;
    }

    if (FAILED(hr) || currentJob == NULL) {
        hr = m_bcManager->CreateJob(jobName, BG_JOB_TYPE_DOWNLOAD, &jobId, &currentJob);
        IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error creating instance of IBackgroundCopyJob.");

        hr = currentJob->AddFile(url, file);
        IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error adding file to download job");
        
    } else if(appendFile != NULL && *appendFile) {
        hr = currentJob->AddFile(url, file);
        IFFALSE_GOTOERROR(SUCCEEDED(hr) || hr == CO_E_OBJNOTCONNECTED, "Error adding file to download job");
    } else {
        jobExisted = true;
    }

    // RADU: Check if file is already added and/or if it's the same file, maybe json changed? 
    if (appendFile == NULL || !(*appendFile)) {
        hr = currentJob->SetNotifyInterface(this);
        IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error calling SetNotifyInterface.");

        hr = currentJob->SetNotifyFlags(BG_NOTIFY_JOB_TRANSFERRED | BG_NOTIFY_JOB_ERROR | BG_NOTIFY_JOB_MODIFICATION | BG_NOTIFY_FILE_TRANSFERRED);
        IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error calling SetNotifyFlags.");
    }

    if (appendFile != NULL) {
        *appendFile = jobExisted;
    }

    if (startDownload || jobExisted) {
        hr = currentJob->Resume();
        IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error resuming download.");
    }

    return true;

error:
    uprintf("DownloadManager::AddDownload (%ls, %ls) failed with hr = 0x%x, last error = %s", url, file, hr, WindowsErrorString());
    return false;
}

STDMETHODIMP DownloadManager::JobTransferred(IBackgroundCopyJob *pJob)
{
    HRESULT hr;

    //Add logic that will not block the callback thread. If you need to perform
    //extensive logic at this time, consider creating a separate thread to perform
    //the work.

    hr = pJob->Complete();
    if (FAILED(hr))
    {
        //Handle error. BITS probably was unable to rename one or more of the 
        //temporary files. See the Remarks section of the IBackgroundCopyJob::Complete 
        //method for more details.
    }

    //If you do not return S_OK, BITS continues to call this callback.
    return S_OK;
}

#define TWO_GB 2147483648    // 2GB

STDMETHODIMP DownloadManager::JobError(IBackgroundCopyJob *pJob, IBackgroundCopyError *pError)
{
    HRESULT hr;
    BG_FILE_PROGRESS Progress;
    BG_ERROR_CONTEXT Context;
    IBackgroundCopyFile *pFile;
    HRESULT ErrorCode = S_OK;
    WCHAR* pszJobName = NULL;
    WCHAR* pszErrorDescription = NULL;
    BOOL IsError = TRUE;

    //Use pJob and pError to retrieve information of interest. For example,
    //if the job is an upload reply, call the IBackgroundCopyError::GetError method 
    //to determine the context in which the job failed. If the context is 
    //BG_JOB_CONTEXT_REMOTE_APPLICATION, the server application that received the 
    //upload file failed.

    hr = pError->GetError(&Context, &ErrorCode);

    //If the proxy or server does not support the Content-Range header or if
    //antivirus software removes the range requests, BITS returns BG_E_INSUFFICIENT_RANGE_SUPPORT.
    //This implementation tries to switch the job to foreground priority, so
    //the content has a better chance of being successfully downloaded.
    if (BG_E_INSUFFICIENT_RANGE_SUPPORT == ErrorCode)
    {
        hr = pError->GetFile(&pFile);
        hr = pFile->GetProgress(&Progress);
        if (BG_SIZE_UNKNOWN == Progress.BytesTotal)
        {
            //The content is dynamic, do not change priority. Handle as an error.
        }
        else if (Progress.BytesTotal > TWO_GB)
        {
            //BITS does not use range requests if the content is less than 2 GB. 
            //However, if the content is greater than 2 GB, BITS
            //uses 2 GB ranges to download the file, so switching to foreground 
            //priority will not help.
        }
        else
        {
            hr = pJob->SetPriority(BG_JOB_PRIORITY_FOREGROUND);
            hr = pJob->Resume();
            IsError = FALSE;
        }

        pFile->Release();
    }

    if (TRUE == IsError)
    {
        hr = pJob->GetDisplayName(&pszJobName);
        hr = pError->GetErrorDescription(LANGIDFROMLCID(GetThreadLocale()), &pszErrorDescription);

        if (pszJobName && pszErrorDescription)
        {
            //Do something with the job name and description. 
        }

        CoTaskMemFree(pszJobName);
        CoTaskMemFree(pszErrorDescription);
    }

    //If you do not return S_OK, BITS continues to call this callback.
    return S_OK;
}

STDMETHODIMP DownloadManager::JobModification(IBackgroundCopyJob *JobModification, DWORD dwReserved)
{
    HRESULT hr;
    WCHAR* pszJobName = NULL;
    BG_JOB_PROGRESS Progress;
    BG_JOB_STATE State;

    hr = JobModification->GetDisplayName(&pszJobName);
    if (SUCCEEDED(hr))
    {
        hr = JobModification->GetProgress(&Progress);
        if (SUCCEEDED(hr))
        {
            hr = JobModification->GetState(&State);
            if (SUCCEEDED(hr))
            {
                DownloadStatus_t *downloadStatus = NULL;
                switch (State) {
                case BG_JOB_STATE_ACKNOWLEDGED:
                case BG_JOB_STATE_TRANSFERRED:
                case BG_JOB_STATE_TRANSFERRING:
                case BG_JOB_STATE_ERROR:
                case BG_JOB_STATE_TRANSIENT_ERROR:
                case BG_JOB_STATE_CANCELLED:
                    downloadStatus = new DownloadStatus_t;
                    *downloadStatus = DownloadStatusNull;
                    if (SUCCEEDED(hr)) downloadStatus->jobName = pszJobName;
                    hr = JobModification->GetProgress(&downloadStatus->progress);
                    break;
                }

                //Do something with the progress and state information.
                //BITS generates a high volume of modification
                //callbacks. Use this callback with discretion. Consider creating a timer and 
                //polling for state and progress information.
                if (BG_JOB_STATE_TRANSFERRED == State) {
                    //Call pJob->Complete(); to acknowledge that the transfer is complete
                    //and make the file available to the client.                    
                    uprintf("DONE\n");
                    hr = JobModification->Complete();
                    if (FAILED(hr)) JobModification->Cancel();
                    downloadStatus->done = true;
                }
                else if (BG_JOB_STATE_ERROR == State || BG_JOB_STATE_TRANSIENT_ERROR == State) {
                    //Call pJob->GetError(&pError); to retrieve an IBackgroundCopyError interface 
                    //pointer which you use to determine the cause of the error.
                    CComPtr<IBackgroundCopyError> error;
                    JobModification->GetError(&error);
                    LPWSTR str[1024];
                    memset(str, 0, 1024);
                    error->GetErrorDescription(GetUserDefaultLangID(), str);
                    uprintf("ERROR!%ls\n", str);
                    if (BG_JOB_STATE_ERROR == State) {
                        downloadStatus->error = true;
                    }
                }
                else if (BG_JOB_STATE_TRANSFERRING == State) {
                    //Call pJob->GetProgress(&Progress); to determine the number of bytes 
                    //and files transferred.                                        
                    downloadStatus->done = false;
                    
                }
                else if (BG_JOB_STATE_QUEUED == State) {
                    uprintf("QUEUED\n");
                }
                else if (BG_JOB_STATE_CONNECTING == State) {
                    uprintf("CONNECTING\n");
                }
                else if (BG_JOB_STATE_SUSPENDED == State) {
                    uprintf("SUSPENDED\n");
                    //m_bcJob->Cancel();
                }
                else if (BG_JOB_STATE_TRANSIENT_ERROR == State) {
                    uprintf("TRANSIENT ERROR\n");
                }
                else if (BG_JOB_STATE_ACKNOWLEDGED == State) {                    
                    uprintf("ACKNOWLEDGED\n");
                    downloadStatus->done = true;
                }
                else if (BG_JOB_STATE_CANCELLED == State) {
                    uprintf("CANCELLED\n");
                    downloadStatus->error = true;
                } else {
                    uprintf("Unknown download state %d\n", State);
                }

                if(downloadStatus != NULL && m_dispatchWindow != NULL) ::PostMessage(m_dispatchWindow, m_statusMsgId, (WPARAM)downloadStatus, 0);
            }
        }
        CoTaskMemFree(pszJobName);
    }

    return S_OK;
}

// IUnknown methods implementation
ULONG DownloadManager::AddRef(void) { return InterlockedIncrement(&m_refCount); }
ULONG DownloadManager::Release(void) { return InterlockedDecrement(&m_refCount); }
HRESULT DownloadManager::QueryInterface(REFIID riid, LPVOID *ppvObj)
{
    *ppvObj = NULL;

    if (__uuidof(IBackgroundCopyCallback) == riid) {
        *ppvObj = (IBackgroundCopyCallback*)(this);        
    } else if (__uuidof(IUnknown) == riid) {
        *ppvObj = (IUnknown*)(this);
    }

    if (*ppvObj != NULL) {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
} 

// Helper methods

HRESULT DownloadManager::GetExistingJob(LPCWSTR jobName, CComPtr<IBackgroundCopyJob> &existingJob)
{
    CComPtr<IEnumBackgroundCopyJobs> enumJobs;
    CComPtr<IBackgroundCopyJob> job;
    HRESULT hr;
    ULONG index = 0;
    ULONG jobCount = 0;
    LPWSTR displayName = NULL;
    BG_JOB_STATE state;

    hr = m_bcManager->EnumJobs(0, &enumJobs);
    IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error calling EnumJobs.");
    
    hr = enumJobs->GetCount(&jobCount);
    IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error calling IEnumBackgroundCopyJobs::GetCount.");

    uprintf("Found a total of %d download jobs.", jobCount);

    for (index = 0; index < jobCount; index++) {
        hr = enumJobs->Next(1, &job, NULL);
        IFFALSE_GOTOERROR(SUCCEEDED(hr) && job != NULL, "Error querying for next job.");
        
        hr = job->GetDisplayName(&displayName);
        IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error querying for display name.");
        uprintf("Found job %d name %ls", index, displayName);
        if (0 == _tcscmp(jobName, displayName)) {            
            hr = job->GetState(&state);
            IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error querying for job state.");
            if (state != BG_JOB_STATE_CANCELLED && state != BG_JOB_STATE_ERROR && state != BG_JOB_STATE_TRANSIENT_ERROR) {
                existingJob = job;
                return S_OK;
            } else {
                job->Cancel();
            }
        }

        CoTaskMemFree(displayName);
        job = NULL;
    }

error:
    return E_FAIL;
}

void DownloadManager::ClearExtraDownloadJobs()
{
    CComPtr<IEnumBackgroundCopyJobs> enumJobs;
    CComPtr<IBackgroundCopyJob> job;
    HRESULT hr;
    ULONG index = 0;
    ULONG jobCount = 0;
    LPWSTR displayName = NULL;
    CList<CString> foundJobs;

    IFFALSE_GOTOERROR(m_bcManager != NULL, "Manager or job is NULL. Nothing to uninit.");

    hr = m_bcManager->EnumJobs(0, &enumJobs);
    IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error calling EnumJobs.");

    hr = enumJobs->GetCount(&jobCount);
    IFFALSE_GOTOERROR(SUCCEEDED(hr), "Error calling IEnumBackgroundCopyJobs::GetCount.");

    uprintf("Found a total of %d download jobs.", jobCount);

    for (index = 0; index < jobCount; index++) {
        hr = enumJobs->Next(1, &job, NULL);
        IFFALSE_GOTOERROR(SUCCEEDED(hr) && job != NULL, "Error querying for next job.");

        hr = job->GetDisplayName(&displayName);
        IFFALSE_CONTINUE(SUCCEEDED(hr), "Error querying for display name.");
        if (displayName == _tcsstr(displayName, DOWNLOAD_JOB_PREFIX)) {
            if (NULL == foundJobs.Find(displayName)) {
                foundJobs.AddTail(displayName);
                job->Suspend();
                uprintf("Suspending job %d name %ls", index, displayName);
            } else {
                job->Cancel();
                uprintf("Cancelling job %d name %ls", index, displayName);
            }
        }
        else {
            uprintf("Found job %d name %ls", index, displayName);
        }

        CoTaskMemFree(displayName);
        job = NULL;
    }

error:
    return;
}

CString DownloadManager::GetJobName(DownloadType_t type)
{
    return CString(DOWNLOAD_JOB_PREFIX) + DownloadTypeToString(type);
}