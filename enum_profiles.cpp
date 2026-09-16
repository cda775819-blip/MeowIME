#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <cstdio>

static const CLSID CLSID_TF_InputProcessorProfiles =
{ 0x33c53a50, 0xf456, 0x4884, { 0xb0, 0x49, 0x85, 0xfd, 0x64, 0x3e, 0xcf, 0xed } };

static const CLSID MY_CLSID = { 0x1F8A3C21, 0x5B7E, 0x4A2D, { 0x9E, 0x3F, 0x4C, 0x8B, 0x1D, 0x2E, 0x7A, 0x5F } };
static const GUID  MY_PROFILE = { 0x3F8A3C21, 0x5B7E, 0x4A2D, { 0x9E, 0x3F, 0x4C, 0x8B, 0x1D, 0x2E, 0x7A, 0x5F } };

int wmain()
{
    CoInitialize(NULL);
    ITfInputProcessorProfiles* p = NULL;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
        __uuidof(ITfInputProcessorProfiles), (void**)&p);
    if (FAILED(hr)) { wprintf(L"CoCreateInstance failed 0x%08X\n", hr); return 1; }

    BOOL enabled = FALSE;
    hr = p->IsEnabledLanguageProfile(MY_CLSID, 0x0804, MY_PROFILE, &enabled);
    wprintf(L"IsEnabledLanguageProfile: hr=0x%08X enabled=%d\n", hr, enabled);

    wprintf(L"--- EnumLanguageProfiles(0x0804) ---\n");
    IEnumTfLanguageProfiles* pEnum = NULL;
    hr = p->EnumLanguageProfiles(0x0804, &pEnum);
    if (SUCCEEDED(hr))
    {
        TF_LANGUAGEPROFILE prof;
        ULONG n = 0; int i = 0;
        while (pEnum->Next(1, &prof, &n) == S_OK && n == 1)
        {
            LPOLESTR sClsid = NULL, sProf = NULL;
            StringFromCLSID(prof.clsid, &sClsid);
            StringFromCLSID(prof.guidProfile, &sProf);
            wprintf(L"[%d] clsid=%s active=%d profile=%s\n", i,
                sClsid ? sClsid : L"?", prof.fActive, sProf ? sProf : L"?");
            CoTaskMemFree(sClsid); CoTaskMemFree(sProf);
            i++;
        }
        wprintf(L"total=%d\n", i);
        pEnum->Release();
    }
    else wprintf(L"EnumLanguageProfiles(0x0804) failed 0x%08X\n", hr);

    p->Release();
    return 0;
}
