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
    wprintf(L"CoCreateInstance: 0x%08X\n", hr);
    if (FAILED(hr)) return 1;

    hr = p->ActivateLanguageProfile(MY_CLSID, 0x0804, MY_PROFILE);
    wprintf(L"ActivateLanguageProfile: 0x%08X\n", hr);

    LANGID lang = 0; GUID prof = {0};
    hr = p->GetActiveLanguageProfile(MY_CLSID, &lang, &prof);
    wprintf(L"GetActiveLanguageProfile: 0x%08X lang=0x%04X\n", hr, lang);

    p->Release();
    return 0;
}
