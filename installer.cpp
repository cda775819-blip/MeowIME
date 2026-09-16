// MeowIME_Setup.exe — 喵喵助手输入法 一键安装器（单文件，资源内嵌）。
// 用法：
//   直接运行        -> 安装
//   运行 /u 或 -u    -> 卸载
// 编译（见 build_installer.ps1）：资源由 rc.exe 生成后与本体一起链接。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>

static std::wstring g_installDir;

// 安装目录：%LOCALAPPDATA%\Programs\MeowIME （用户可写，Rime 用户数据才能正常落盘）
static std::wstring GetInstallDir()
{
    wchar_t base[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        n = GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return std::wstring(base) + L"\\AppData\\Local\\Programs\\MeowIME";
        return L"C:\\MeowIME";
    }
    return std::wstring(base) + L"\\Programs\\MeowIME";
}

static bool EnsureDir(const std::wstring& dir)
{
    int r = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    return (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS);
}

// 从资源中释放一个文件到磁盘（自动创建父目录）
static bool ExtractResource(HINSTANCE hInst, LPCWSTR resName, const std::wstring& outPath)
{
    HRSRC hRes = FindResourceW(hInst, resName, MAKEINTRESOURCEW(RT_RCDATA));
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return false;
    void* p = LockResource(hData);
    DWORD size = SizeofResource(hInst, hRes);
    if (!p || size == 0) return false;

    size_t pos = outPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        if (!EnsureDir(outPath.substr(0, pos))) return false;

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, p, size, &written, NULL);
    CloseHandle(hFile);
    return ok && written == size;
}

// 释放文件；目标被占用时自动换一个带序号的文件名。
//
// 为什么需要：正在运行的程序会锁住已加载的 DLL，此时覆盖必然
// ERROR_SHARING_VIOLATION。以前这里直接报"释放文件失败：CatTextService.dll"
// 就中止了 —— 用户看到这句话完全不知道发生了什么（升级时必现）。
// 换成 CatTextService.1.dll / .2.dll ... 落盘即可：注册表指向哪个文件
// 是由我们决定的，换了名字照样注册成功，用户不用去关程序。
static bool ExtractWithFallback(HINSTANCE hInst, LPCWSTR resName,
                                const std::wstring& outPath, std::wstring& actualPath)
{
    if (ExtractResource(hInst, resName, outPath)) { actualPath = outPath; return true; }

    // 只有带扩展名的文件才换名（目录名上加序号没有意义）
    size_t dot = outPath.find_last_of(L'.');
    size_t slash = outPath.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return false;

    const std::wstring stem = outPath.substr(0, dot);
    const std::wstring ext  = outPath.substr(dot);
    for (int i = 1; i <= 8; i++)
    {
        wchar_t suffix[16];
        swprintf_s(suffix, L".%d", i);
        const std::wstring cand = stem + suffix + ext;
        if (ExtractResource(hInst, resName, cand)) { actualPath = cand; return true; }
    }
    return false;
}

// 读取内嵌的文件清单（UTF-8，每行 "资源名=相对路径"）
static std::wstring LoadList(HINSTANCE hInst)
{
    HRSRC h = FindResourceW(hInst, L"FILELIST", MAKEINTRESOURCEW(RT_RCDATA));
    if (!h) return L"";
    HGLOBAL hd = LoadResource(hInst, h);
    DWORD size = SizeofResource(hInst, h);
    const char* p = (const char*)LockResource(hd);
    if (!p || size == 0) return L"";

    std::string s(p, size);
    // 去掉 UTF-8 BOM
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s = s.substr(3);

    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return L"";
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// 解析清单，返回 (资源名 -> 相对路径) 列表
static std::vector<std::pair<std::wstring, std::wstring>> ParseList(const std::wstring& text)
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    std::wstring cur;
    for (wchar_t c : text)
    {
        if (c == L'\r' || c == L'\n')
        {
            if (!cur.empty())
            {
                size_t eq = cur.find(L'=');
                if (eq != std::wstring::npos)
                    out.emplace_back(cur.substr(0, eq), cur.substr(eq + 1));
            }
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
    {
        size_t eq = cur.find(L'=');
        if (eq != std::wstring::npos)
            out.emplace_back(cur.substr(0, eq), cur.substr(eq + 1));
    }
    return out;
}

// 调用 DLL 的自注册 / 反注册
static HRESULT RegisterDll(const std::wstring& dllPath, bool unregister)
{
    HMODULE m = LoadLibraryW(dllPath.c_str());
    if (!m) return HRESULT_FROM_WIN32(GetLastError());

    const char* name = unregister ? "DllUnregisterServer" : "DllRegisterServer";
    FARPROC pfn = GetProcAddress(m, name);
    HRESULT hr = E_FAIL;
    if (pfn)
    {
        typedef HRESULT(STDAPICALLTYPE * PFN)();
        hr = ((PFN)pfn)();
    }
    FreeLibrary(m);
    return hr;
}

// 重启输入服务（结束 TextInputHost，系统会自动拉起，刷新 TSF 输入法列表）
static void RestartInputHost()
{
    system("taskkill /F /IM TextInputHost.exe >nul 2>&1");
}

// 删除安装目录（best effort）
static void DeleteInstallDir()
{
    std::wstring from = g_installDir;
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_installDir = GetInstallDir();

    // 解析命令行参数
    bool uninstall = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"/u") == 0 || _wcsicmp(argv[i], L"-u") == 0 || _wcsicmp(argv[i], L"/uninstall") == 0)
                uninstall = true;
        }
        LocalFree(argv);
    }

    if (uninstall)
    {
        // 反注册 -> 重启输入服务释放占用 -> 删除文件
        HRESULT hr = RegisterDll(g_installDir + L"\\CatTextService.dll", true);
        RestartInputHost();
        Sleep(1200);
        DeleteInstallDir();

        if (SUCCEEDED(hr))
            MessageBoxW(NULL, L"喵喵助手已卸载完成。", L"喵喵助手", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(NULL, L"反注册失败，部分文件可能已删除。\n可尝试重启后重试。", L"喵喵助手", MB_OK | MB_ICONWARNING);
        return 0;
    }

    // 安装：先重启输入服务，释放可能被占用的旧 DLL
    RestartInputHost();

    if (!EnsureDir(g_installDir))
    {
        MessageBoxW(NULL, L"无法创建安装目录。", L"喵喵助手", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 读取清单并释放所有文件
    std::wstring list = LoadList(hInst);
    auto entries = ParseList(list);
    if (entries.empty())
    {
        MessageBoxW(NULL, L"安装包数据损坏（未找到文件清单）。", L"喵喵助手", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring failPath;
    // 输入法 DLL 实际落盘的名字。被占用时会换成 CatTextService.N.dll，
    // 注册必须指向真正写成功的那个文件，否则注册了也加载不到。
    std::wstring dllPath = g_installDir + L"\\CatTextService.dll";
    for (auto& e : entries)
    {
        const std::wstring target = g_installDir + L"\\" + e.second;
        std::wstring actual;
        if (!ExtractWithFallback(hInst, e.first.c_str(), target, actual))
        {
            failPath = e.second;
            break;
        }
        if (_wcsicmp(e.second.c_str(), L"CatTextService.dll") == 0)
            dllPath = actual;
    }

    if (!failPath.empty())
    {
        std::wstring msg = L"释放文件失败：" + failPath +
                           L"\n\n多半是某个程序正锁着旧文件。\n"
                           L"关掉聊天软件 / 浏览器 / 资源管理器后重试，"
                           L"或者先注销一次再装。";
        MessageBoxW(NULL, msg.c_str(), L"喵喵助手", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 注册 TSF（指向实际落盘的那个 DLL）
    HRESULT hr = RegisterDll(dllPath, false);
    RestartInputHost();

    if (SUCCEEDED(hr))
    {
        std::wstring msg = L"喵喵助手安装完成！\n\n"
                           L"请在系统设置 -> 时间和语言 -> 语言和区域 中添加中文输入法：\n"
                           L"喵喵助手。\n\n安装位置：\n" + g_installDir +
                           L"\n\n第三方组件许可见安装目录下的 THIRD-PARTY-NOTICES.txt";
        MessageBoxW(NULL, msg.c_str(), L"喵喵助手", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        MessageBoxW(NULL, L"文件已安装，但注册输入法失败（可能需要以管理员身份运行）。", L"喵喵助手", MB_OK | MB_ICONERROR);
        return 1;
    }

    return 0;
}