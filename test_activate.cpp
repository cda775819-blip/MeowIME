#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <cstdio>

static const CLSID MY_CLSID = { 0x1F8A3C21, 0x5B7E, 0x4A2D, { 0x9E, 0x3F, 0x4C, 0x8B, 0x1D, 0x2E, 0x7A, 0x5F } };

typedef HRESULT (WINAPI *PFN_TF_CreateThreadMgr)(ITfThreadMgr**);

int wmain()
{
    CoInitialize(NULL);

    ITfTextInputProcessor* pTip = NULL;
    HRESULT hr = CoCreateInstance(MY_CLSID, NULL, CLSCTX_INPROC_SERVER,
        __uuidof(ITfTextInputProcessor), (void**)&pTip);
    wprintf(L"CoCreateInstance: 0x%08X  tip=%p\n", hr, pTip);
    if (FAILED(hr) || !pTip) return 1;

    HMODULE hMsctf = LoadLibraryW(L"msctf.dll");
    PFN_TF_CreateThreadMgr pfn = (PFN_TF_CreateThreadMgr)GetProcAddress(hMsctf, "TF_CreateThreadMgr");
    wprintf(L"TF_CreateThreadMgr ptr=%p\n", pfn);
    if (!pfn) return 1;

    ITfThreadMgr* pMgr = NULL;
    hr = pfn(&pMgr);
    wprintf(L"TF_CreateThreadMgr: 0x%08X  mgr=%p\n", hr, pMgr);
    if (FAILED(hr) || !pMgr) return 1;

    TfClientId cid = 0;
    hr = pMgr->Activate(&cid);
    wprintf(L"ThreadMgr Activate: 0x%08X  clientId=%u\n", hr, cid);

    hr = pTip->Activate(pMgr, cid);
    wprintf(L"TIP Activate: 0x%08X\n", hr);

    pTip->Deactivate();
    pMgr->Deactivate();
    pTip->Release();
    pMgr->Release();
    return 0;
}
