// CatTextService.cpp
// 喵喵助手 TSF 文本服务（键盘 TIP），集成 Rime/librime 实现中文拼音输入，
// 候选词使用自绘置顶分层窗口显示（不依赖应用程序渲染）。
//
// 编译：直接用项目根目录的 build.ps1（它会处理 /MT 静态 CRT、GDI+ 依赖和部署）。
// 手工编译等价于：
//   cl /LD /MT /EHsc /utf-8 /W3 /I"<librime 的 include 目录>" CatTextService.cpp ^
//      /Fe:CatTextService.dll ^
//      /link /DEF:CatTextService.def ole32.lib oleaut32.lib uuid.lib ^
//      user32.lib advapi32.lib gdi32.lib
// 注意 /MT 不能省：否则会依赖 MSVCP140.dll / VCRUNTIME140.dll，
// 在没装 VC++ 运行库的机器上这个 DLL 根本加载不起来。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <string>
#include <vector>
// GDI+ 用来画候选窗：抗锯齿圆角、柔和阴影、带 alpha 的文字。
// 必须放在 windows.h 之后；objidl.h 是 gdiplus.h 的前置依赖。
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
using Gdiplus::REAL;   // GDI+ 的 REAL 在 Gdiplus 命名空间里，用惯了不带前缀
#include <share.h>
#include <cstdlib>
#include "rime_api.h"
#include <cstdio>
#include <cstdarg>

// ---------------- 调试日志（运行时开关，实现见 g_dllDir 之后） ----------------
static void DbgLog(const wchar_t* fmt, ...);

// ---------------- GUID ----------------
static const CLSID CLSID_CatTextService =
{ 0x1F8A3C21, 0x5B7E, 0x4A2D, { 0x9E, 0x3F, 0x4C, 0x8B, 0x1D, 0x2E, 0x7A, 0x5F } };
static const GUID GUID_PROFILE =
{ 0x3F8A3C21, 0x5B7E, 0x4A2D, { 0x9E, 0x3F, 0x4C, 0x8B, 0x1D, 0x2E, 0x7A, 0x5F } };

static HINSTANCE g_hInst = NULL;

// ---------------- Rime 动态加载 ----------------
typedef RimeApi* (*PFN_rime_get_api)(void);

static HMODULE        g_hRime = NULL;
static RimeApi*       g_api = NULL;
static RimeSessionId  g_session = 0;
static bool           g_rimeReady = false;
static bool           g_csReady = false;
static int            g_rimeFailCount = 0;   // 初始化失败次数，够了就进入退避
static ULONGLONG      g_rimeRetryAt = 0;     // 到点之后才允许再试一次（GetTickCount64，不受 49 天回绕影响）
static CRITICAL_SECTION g_rimeCs;
static std::string    g_sharedDir;
static std::string    g_userDir;
static std::wstring   g_dllDir;

// 引擎初始化失败几次后进入退避：每 30 秒只允许再试一次。
// 失败不是免费的 —— 如果 rime.dll 被删掉/被杀软隔离，LoadLibraryW 会真的去搜
// 路径、查注册表，而它在每一次按键上都会重来一遍。退避期内所有按键原样透传给
// 应用：用户至少还能正常打字，而不是每敲一个字都白跑一次失败的加载。
// 用退避而不是彻底放弃，是为了万一 rime.dll 被补回来（重装、杀软放行），
// 不用重启程序也能自己恢复。
static const int   kRimeMaxRetry   = 3;
static const DWORD kRimeRetryDelay = 30000;   // ms

// 记一次失败，必要时进入退避
static void NoteRimeFailure()
{
    g_rimeFailCount++;
    if (g_rimeFailCount >= kRimeMaxRetry)
        g_rimeRetryAt = GetTickCount64() + kRimeRetryDelay;
}

// 累积原文的长度上限。DoTransform 回读校验用的是 1024 字缓冲，超过这个数
// 必然对不上、静默不喵化；同时也防止一直不打标点也不回车时内存无界增长。
static const size_t kMaxSessionOriginal = 900;

// 中英文状态不再由本文件维护：Shift / Caps Lock 原样交给 Rime 的
// ascii_composer，开关状态由 Rime 的 ascii_mode 选项说了算。

// ---------------- 调试日志：运行时开关 ----------------
// DLL 同目录下存在 CatTextService.debug 这个空文件时才写 cand_debug.log，
// 没有它就是彻底的空操作（每次按键只多一次 bool 判断）。
// 这样不用改代码重编译就能开关日志，也方便确认手上这个 DLL 到底是哪一版。
// 注意：日志里包含你打的字，排查完记得把 CatTextService.debug 删掉。
static FILE* g_pLog = NULL;
static bool  g_logChecked = false;

static void DbgLog(const wchar_t* fmt, ...)
{
    if (!g_logChecked)
    {
        g_logChecked = true;
        std::wstring flag = g_dllDir + L"CatTextService.debug";
        if (GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            std::wstring path = g_dllDir + L"cand_debug.log";
            // 必须用 _wfsopen 显式 _SH_DENYNO：默认的独占式打开会让排查时
            // 连日志都读不出来（另一个进程正拿着这个文件）。
            g_pLog = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
        }
    }
    if (!g_pLog) return;

    va_list ap;
    va_start(ap, fmt);
    vfwprintf_s(g_pLog, fmt, ap);
    va_end(ap);
    fputwc(L'\n', g_pLog);
    fflush(g_pLog);
}

// X11 修饰键掩码（与 librime key_table.h 一致）
enum
{
    kShiftMask   = 1 << 0,
    kLockMask    = 1 << 1,
    kControlMask = 1 << 2,
    kAltMask     = 1 << 3,
    kReleaseMask = 1 << 30,
};

// X11 keysym
enum
{
    XK_space     = 0x20,
    XK_BackSpace = 0xff08,
    XK_Tab       = 0xff09,
    XK_Return    = 0xff0d,
    XK_Escape    = 0xff1b,
    XK_Delete    = 0xffff,
    XK_Home      = 0xff50,
    XK_Left      = 0xff51,
    XK_Up        = 0xff52,
    XK_Right     = 0xff53,
    XK_Down      = 0xff54,
    XK_Page_Up   = 0xff55,
    XK_Page_Down = 0xff56,
    XK_End       = 0xff57,
    // 修饰键：必须送进 Rime，它的 ascii_composer 才知道中英怎么切
    // （见 rime_data/default.yaml 的 ascii_composer.switch_key）
    XK_Shift_L   = 0xffe1,
    XK_Shift_R   = 0xffe2,
    XK_Caps_Lock = 0xffe5,
};

// ---------------- UTF-8 <-> UTF-16 ----------------
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return L"";
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

// ---------------- Rime 初始化 ----------------
//
// 这里的核心原则：**任何一步失败都必须让按键原样透传给应用**，而不是
// 半死不活地吃键。TSF 输入法是 in-proc 加载进宿主进程的，它坏掉的时候
// 用户最不能接受的是"打字没反应"。
static bool EnsureRime()
{
    EnterCriticalSection(&g_rimeCs);

    // 退避期内直接放行按键，不再尝试初始化
    if (g_rimeRetryAt != 0 && GetTickCount64() < g_rimeRetryAt)
    {
        LeaveCriticalSection(&g_rimeCs);
        return false;
    }

    if (g_api == NULL)
    {
        std::wstring dllPath = g_dllDir + L"rime.dll";
        g_hRime = LoadLibraryW(dllPath.c_str());
        if (g_hRime)
        {
            PFN_rime_get_api pfn = (PFN_rime_get_api)GetProcAddress(g_hRime, "rime_get_api");
            if (pfn) g_api = pfn();
        }

        if (g_api == NULL)
        {
            if (g_hRime) { FreeLibrary(g_hRime); g_hRime = NULL; }
            NoteRimeFailure();
            DbgLog(L"[Rime] 加载 rime.dll 失败 %d/%d（%s）—— 按键将原样透传",
                   g_rimeFailCount, kRimeMaxRetry, dllPath.c_str());
            LeaveCriticalSection(&g_rimeCs);
            return false;
        }

        g_sharedDir = WideToUtf8(g_dllDir + L"rime_data");
        g_userDir = WideToUtf8(g_dllDir + L"rime_user");

        RIME_STRUCT(RimeTraits, traits);
        traits.shared_data_dir = g_sharedDir.c_str();
        traits.user_data_dir = g_userDir.c_str();
        traits.app_name = "rime.cat-tsf";
        traits.min_log_level = 2; // ERROR

        g_api->setup(&traits);
        g_api->initialize(&traits);
        if (g_api->start_maintenance(False))
            g_api->join_maintenance_thread();
    }

    if (g_session == 0)
    {
        g_session = g_api->create_session();
        if (g_session == 0)
        {
            NoteRimeFailure();
            DbgLog(L"[Rime] create_session 失败 %d/%d", g_rimeFailCount, kRimeMaxRetry);
            LeaveCriticalSection(&g_rimeCs);
            return false;
        }

        // 方案必须选上。无方案状态的 session 仍然会吃掉一部分按键但什么都不
        // 上屏 —— 用户看到的就是"按了没反应"。这种半死状态比完全不可用更糟，
        // 所以宁可销毁 session、让按键全部透传，也不能装作能用。
        if (!g_api->select_schema(g_session, "luna_pinyin"))
        {
            g_api->destroy_session(g_session);
            g_session = 0;
            NoteRimeFailure();
            DbgLog(L"[Rime] select_schema(luna_pinyin) 失败 %d/%d —— 按键将原样透传",
                   g_rimeFailCount, kRimeMaxRetry);
            LeaveCriticalSection(&g_rimeCs);
            return false;
        }
        g_api->set_option(g_session, "zh_hans", True); // 简体
    }

    g_rimeReady = true;
    g_rimeFailCount = 0;    // 成功就清掉失败计数和退避
    g_rimeRetryAt = 0;
    LeaveCriticalSection(&g_rimeCs);
    return true;
}

// ---------------- 按键 -> Rime keycode/mask ----------------
static bool VkToRimeKey(WPARAM vk, LPARAM lParam, int& keycode, int& mask)
{
    mask = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mask |= kShiftMask;
    if (GetKeyState(VK_CONTROL) & 0x8000) mask |= kControlMask;
    if (GetKeyState(VK_MENU) & 0x8000)    mask |= kAltMask;
    if (GetKeyState(VK_CAPITAL) & 0x0001) mask |= kLockMask;

    switch (vk)
    {
    case VK_BACK:   keycode = XK_BackSpace; return true;
    case VK_TAB:    keycode = XK_Tab;       return true;
    case VK_RETURN: keycode = XK_Return;    return true;
    case VK_ESCAPE: keycode = XK_Escape;    return true;
    case VK_SPACE:  keycode = XK_space;     return true;
    case VK_DELETE: keycode = XK_Delete;    return true;
    case VK_LEFT:   keycode = XK_Left;      return true;
    case VK_UP:     keycode = XK_Up;        return true;
    case VK_RIGHT:  keycode = XK_Right;     return true;
    case VK_DOWN:   keycode = XK_Down;      return true;
    case VK_PRIOR:  keycode = XK_Page_Up;   return true;
    case VK_NEXT:   keycode = XK_Page_Down; return true;
    case VK_HOME:   keycode = XK_Home;      return true;
    case VK_END:    keycode = XK_End;       return true;

    // Shift 必须原样送进 Rime，不能自己拦。
    // 原因：! 和 ? 是靠 Shift+1 / Shift+/ 打出来的，一旦在 Shift 上抢键
    // （切 ascii_mode、清累积原文），这两个标点就再也到不了 punctuator。
    // 左右 Shift 用扫描码区分（左 0x2A / 右 0x36），对应 Rime 的 Shift_L / Shift_R。
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    {
        UINT scanShift = (UINT)((lParam >> 16) & 0xff);
        keycode = (scanShift == 0x36) ? XK_Shift_R : XK_Shift_L;
        return true;
    }
    case VK_CAPITAL: keycode = XK_Caps_Lock; return true;
    }

    // 其它可打印键：交给 ToUnicodeEx 按当前布局取字符（清除 Ctrl/Alt，保留 Shift）
    //
    // 这里不能忽略 GetKeyboardState 的返回值（静态分析 C6031）：它失败时
    // keyState 会保持全零，ToUnicodeEx 于是按"没按 Shift"翻译 ——
    // 结果 Shift+1 得到 '1' 而不是 '!'，Shift+/ 得到 '/' 而不是 '?'，
    // 感叹号和问号就又打不出来了（正是之前修过的那条路）。
    // 所以无论如何都用 GetKeyState 把修饰键补齐，不依赖 GetKeyboardState 成功。
    BYTE keyState[256] = { 0 };
    if (!GetKeyboardState(keyState))
        ZeroMemory(keyState, sizeof(keyState));

    keyState[VK_SHIFT]   = (BYTE)((GetKeyState(VK_SHIFT)   & 0x8000) ? 0x80 : 0);
    keyState[VK_CAPITAL] = (BYTE)((GetKeyState(VK_CAPITAL) & 0x0001) ? 0x01 : 0);
    keyState[VK_CONTROL] = 0;
    keyState[VK_MENU] = 0;
    WCHAR buf[4] = { 0 };
    UINT scan = (UINT)((lParam >> 16) & 0xff);
    if (ToUnicodeEx((UINT)vk, scan, keyState, buf, 4, 0, NULL) == 1)
    {
        keycode = buf[0];
        return true;
    }
    return false;
}

// ---------------- 候选窗口（GDI+ 分层窗口：圆角 + 柔阴影）----------------
//
// 用 UpdateLayeredWindow 建 32bpp 带 per-pixel alpha 的分层窗口，用 GDI+ 绘制：
//   · 真圆角（不是 RoundRectRgn 那种硬切边）
//   · 柔和投影，不依赖 CS_DROPSHADOW
//   · 选中项是淡蓝圆角药丸 + 深色字，不再是刺眼的纯蓝实心块
//   · 跟随系统浅色 / 深色主题
//   · 字号与间距按插入点所在显示器的 DPI 缩放（150% 下不会太小也不会糊）
//   · 左侧一个「中 / 英」状态徽标
//
// 关于文字渲染：分层窗口带 alpha，ClearType 的次像素抗锯齿在这里会把 alpha 算错，
// 所以统一用灰阶抗锯齿 AntiAliasGridFit。
static const wchar_t* kCandWndClass = L"CatCandWindow";
static HWND  g_hCandWnd = NULL;
static bool  g_candWndRegistered = false;
static std::vector<std::wstring> g_candList;
static int   g_candSel = 0;
static bool  g_showCandidates = false;   // true=正在显示候选列表；false=只在闪状态徽标
static bool  g_candVisible = false;      // 窗口当前是否可见，避免重复 ShowWindow 造成闪烁
static bool  g_curAsciiMode = false;     // 仅用于显示，真值在 Rime 的 ascii_mode 选项里

static ULONG_PTR g_gdiplusToken = 0;
static bool      g_gdiplusReady = false;
static Gdiplus::FontFamily* g_pCandFamily = NULL;

static void EnsureGdiplus()
{
    if (g_gdiplusReady) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, NULL) == Gdiplus::Ok)
        g_gdiplusReady = true;
}

// 中文字体优先，找不到就退回 Segoe UI（英文版系统）
static Gdiplus::FontFamily* CandFontFamily()
{
    if (!g_pCandFamily)
    {
        g_pCandFamily = new Gdiplus::FontFamily(L"Microsoft YaHei UI");
        if (!g_pCandFamily->IsAvailable())
        {
            delete g_pCandFamily;
            g_pCandFamily = new Gdiplus::FontFamily(L"Segoe UI");
        }
    }
    return g_pCandFamily;
}

struct CandTheme
{
    Gdiplus::Color bg, border, text, num, selBg, selText, selNum, shadow;
    Gdiplus::Color badgeBg, badgeText;
    REAL textPx, numPx, badgePx;
    REAL radius, padX, padY, gap, numGap, badgeGap;
    REAL lineH, shadowBlur, shadowDy;
};

static bool SystemUsesDarkTheme()
{
    DWORD v = 1, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

// 取插入点所在显示器的 DPI（150% -> 144）。
//
// 这里刻意不去问候选窗自己：早期版本为了拿到「窗口所在屏幕」的 DPI，先把窗口
// 挪到插入点、再缩成 8x8 去探测，结果每次按键候选框都要藏起来缩一下再展开，
// 打字时肉眼可见地一闪一闪。改成按插入点直接查显示器，一次调用解决，完全不碰窗口。
static int DpiForPoint(POINT pt)
{
    typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor pfn = NULL;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        HMODULE s = LoadLibraryW(L"shcore.dll");   // Win8.1+
        if (s) pfn = (PFN_GetDpiForMonitor)(void*)GetProcAddress(s, "GetDpiForMonitor");
    }
    if (pfn)
    {
        UINT dx = 0, dy = 0;
        // MDT_EFFECTIVE_DPI = 0
        if (SUCCEEDED(pfn(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), 0, &dx, &dy)) &&
            dx >= 72 && dx <= 960)
            return (int)dx;
    }
    HDC hdc = GetDC(NULL);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(NULL, hdc);
    return (dpi >= 72 && dpi <= 960) ? dpi : 96;
}

static CandTheme MakeTheme(bool dark, bool asciiMode, int dpi)
{
    const REAL k = dpi / 96.0f;
    CandTheme t;
    if (dark)
    {
        t.bg      = Gdiplus::Color(255,  43,  45,  49);
        t.border  = Gdiplus::Color(255,  60,  63,  68);
        t.text    = Gdiplus::Color(255, 231, 233, 236);
        t.num     = Gdiplus::Color(255, 138, 145, 153);
        t.selBg   = Gdiplus::Color(255,  54,  68, 102);
        t.selText = Gdiplus::Color(255, 255, 255, 255);
        t.selNum  = Gdiplus::Color(255, 138, 180, 248);
        t.shadow  = Gdiplus::Color( 70,   0,   0,   0);
        t.badgeBg   = asciiMode ? Gdiplus::Color(255,  58,  61,  66) : Gdiplus::Color(255,  54,  68, 102);
        t.badgeText = asciiMode ? Gdiplus::Color(255, 168, 176, 186) : Gdiplus::Color(255, 138, 180, 248);
    }
    else
    {
        t.bg      = Gdiplus::Color(255, 253, 253, 254);
        t.border  = Gdiplus::Color(255, 226, 228, 233);
        t.text    = Gdiplus::Color(255,  28,  30,  34);
        t.num     = Gdiplus::Color(255, 152, 160, 172);
        t.selBg   = Gdiplus::Color(255, 232, 240, 254);
        t.selText = Gdiplus::Color(255,  16,  42,  94);
        t.selNum  = Gdiplus::Color(255,  26,  86, 219);
        t.shadow  = Gdiplus::Color( 34,   0,   0,   0);
        t.badgeBg   = asciiMode ? Gdiplus::Color(255, 241, 242, 244) : Gdiplus::Color(255, 232, 240, 254);
        t.badgeText = asciiMode ? Gdiplus::Color(255, 107, 114, 128) : Gdiplus::Color(255,  26,  86, 219);
    }
    t.textPx   = 15.0f * k;
    t.numPx    = 11.0f * k;
    t.badgePx  = 11.0f * k;
    t.radius   = 9.0f * k;
    t.padX     = 6.0f * k;
    t.padY     = 4.0f * k;
    t.gap      = 2.0f * k;
    t.numGap   = 4.0f * k;
    t.badgeGap = 6.0f * k;
    t.lineH    = t.textPx * 1.30f;
    t.shadowBlur = 8.0f * k;
    t.shadowDy   = 2.0f * k;
    return t;
}

// 字体和 StringFormat 创建都不便宜，而候选框每敲一个字就要整个重画一遍。
// 按 DPI 缓存住，别再每次按键新建 6 个 Font 对象。
static int g_fontDpi = 0;
static Gdiplus::Font* g_fText = NULL;
static Gdiplus::Font* g_fNum = NULL;
static Gdiplus::Font* g_fBadge = NULL;
static Gdiplus::StringFormat* g_sfMeasure = NULL;   // 量尺寸用（默认 Near 对齐）
static Gdiplus::StringFormat* g_sfDraw = NULL;      // 画字用（居中）

static void EnsureCandFonts(int dpi, const CandTheme& th)
{
    if (!g_sfMeasure)
        g_sfMeasure = Gdiplus::StringFormat::GenericTypographic()->Clone();
    if (!g_sfDraw)
    {
        g_sfDraw = Gdiplus::StringFormat::GenericTypographic()->Clone();
        g_sfDraw->SetAlignment(Gdiplus::StringAlignmentCenter);
        g_sfDraw->SetLineAlignment(Gdiplus::StringAlignmentCenter);
    }
    if (g_fontDpi == dpi && g_fText && g_fNum && g_fBadge) return;

    delete g_fText; delete g_fNum; delete g_fBadge;
    Gdiplus::FontFamily* fam = CandFontFamily();
    g_fText  = new Gdiplus::Font(fam, th.textPx,  Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_fNum   = new Gdiplus::Font(fam, th.numPx,   Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_fBadge = new Gdiplus::Font(fam, th.badgePx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_fontDpi = dpi;
}

static void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, REAL radius)
{
    REAL d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 1.0f) { path.AddRectangle(r); return; }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static Gdiplus::SizeF MeasureStr(Gdiplus::Graphics& g, const wchar_t* s, int len,
                                 Gdiplus::Font* f, Gdiplus::StringFormat* fmt)
{
    if (!s || len <= 0) return Gdiplus::SizeF(0.0f, 0.0f);
    Gdiplus::RectF layout(0.0f, 0.0f, 4096.0f, 512.0f);
    Gdiplus::RectF out(0.0f, 0.0f, 0.0f, 0.0f);
    g.MeasureString(s, len, f, layout, fmt, &out);
    return Gdiplus::SizeF(out.Width, out.Height);
}

// 画一次并显示。cands 为空时只画状态徽标（切中英时闪一下用）。
static void RenderCandWindow(const RECT& caretRect, bool haveCaret,
                             const std::vector<std::wstring>& cands, int sel)
{
    EnsureGdiplus();
    if (!g_gdiplusReady || !g_hCandWnd) return;

    POINT ptDpi = { haveCaret ? caretRect.left : 0, haveCaret ? caretRect.bottom : 0 };
    const int dpi = DpiForPoint(ptDpi);
    const CandTheme th = MakeTheme(SystemUsesDarkTheme(), g_curAsciiMode, dpi);
    EnsureCandFonts(dpi, th);

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return;

    // ---- 第一遍：量尺寸 ----
    // 量尺寸用的 DC 和位图也缓存住，别每次按键都 CreateCompatibleDC 一遍。
    std::vector<REAL> itemW(cands.size(), 0.0f), numW(cands.size(), 0.0f), textW(cands.size(), 0.0f);
    REAL badgeW = 0.0f, itemsW = 0.0f;
    {
        static HDC     s_hdcM = NULL;
        static HBITMAP s_hbmM = NULL;
        static HGDIOBJ s_oldM = NULL;
        if (!s_hdcM)
        {
            s_hdcM = CreateCompatibleDC(hdcScreen);
            s_hbmM = CreateCompatibleBitmap(hdcScreen, 8, 8);
            if (s_hdcM && s_hbmM)
                s_oldM = SelectObject(s_hdcM, s_hbmM);
        }
        if (s_hdcM)
        {
            Gdiplus::Graphics gm(s_hdcM);
            gm.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

            badgeW = MeasureStr(gm, g_curAsciiMode ? L"英" : L"中", 1, g_fBadge, g_sfMeasure).Width
                     + th.padX * 2.2f;

            for (size_t i = 0; i < cands.size(); i++)
            {
                wchar_t num[8];
                swprintf_s(num, L"%d", (int)i + 1);
                numW[i]  = MeasureStr(gm, num, (int)wcslen(num), g_fNum, g_sfMeasure).Width + 1.0f;
                textW[i] = MeasureStr(gm, cands[i].c_str(), (int)cands[i].size(), g_fText, g_sfMeasure).Width + 2.0f;
                itemW[i] = numW[i] + th.numGap + textW[i] + th.padX * 2.0f;
                itemsW += itemW[i];
                if (i > 0) itemsW += th.gap;
            }
        }
        else
        {
            // 极端情况下量不了尺寸，给个兜底免得算出 0 宽窗口
            for (size_t i = 0; i < cands.size(); i++) { itemW[i] = 60.0f; itemsW += 62.0f; }
            badgeW = 34.0f;
        }
    }

    const REAL barH = th.lineH + th.padY * 2.0f;
    const REAL barW = th.padX + badgeW + (cands.empty() ? 0.0f : th.badgeGap + itemsW) + th.padX;

    const int sb = (int)(th.shadowBlur + 0.999f);
    const int winW = (int)(barW + 0.999f) + sb * 2;
    const int winH = (int)(barH + 0.999f) + sb * 2;

    // ---- 定位：光标下方优先，放不下翻到上方，再夹进显示器工作区 ----
    int x = haveCaret ? caretRect.left : 60;
    int y = haveCaret ? (caretRect.bottom + 2) : 60;
    POINT ptRef = { haveCaret ? caretRect.left : x, haveCaret ? caretRect.bottom : y };
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromPoint(ptRef, MONITOR_DEFAULTTONEAREST), &mi))
        wa = mi.rcWork;
    if (x + winW > wa.right)  x = wa.right - winW;
    if (x < wa.left)          x = wa.left;
    if (y + winH > wa.bottom) y = (haveCaret ? caretRect.top : y) - winH - 2;
    if (y < wa.top)           y = wa.top;

    // ---- 第二遍：画到 32bpp DIB ----
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = winW;
    bmi.bmiHeader.biHeight      = -winH;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) { ReleaseDC(NULL, hdcScreen); return; }
    ZeroMemory(bits, (size_t)winW * winH * 4);   // 从全透明起步

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbm);
    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        const Gdiplus::RectF bar((REAL)sb, (REAL)sb, barW, barH);

        // 柔和投影：由外到内叠几层低透明度圆角矩形
        for (int i = sb; i >= 1; i--)
        {
            Gdiplus::GraphicsPath sp;
            Gdiplus::RectF sr(bar.X - i, bar.Y - i + th.shadowDy,
                              bar.Width + i * 2.0f, bar.Height + i * 2.0f);
            AddRoundRect(sp, sr, th.radius + i);
            BYTE a = (BYTE)(th.shadow.GetA() * (sb - i + 1) / (sb * 1.6f));
            Gdiplus::SolidBrush sbr(Gdiplus::Color(a, 0, 0, 0));
            g.FillPath(&sbr, &sp);
        }

        // 卡片本体 + 1px 描边
        {
            Gdiplus::GraphicsPath bp;
            AddRoundRect(bp, bar, th.radius);
            Gdiplus::SolidBrush br(th.bg);
            g.FillPath(&br, &bp);
            Gdiplus::Pen pen(th.border, 1.0f);
            g.DrawPath(&pen, &bp);
        }

        // 用缓存好的字体 / 格式，别每次按键重建
        Gdiplus::Font& fText  = *g_fText;
        Gdiplus::Font& fNum   = *g_fNum;
        Gdiplus::Font& fBadge = *g_fBadge;
        Gdiplus::StringFormat* fmt = g_sfDraw;

        Gdiplus::SolidBrush brText(th.text), brNum(th.num);
        Gdiplus::SolidBrush brSelText(th.selText), brSelNum(th.selNum);
        Gdiplus::SolidBrush brBadgeText(th.badgeText);

        // 中/英 状态徽标
        Gdiplus::RectF badge(bar.X + th.padX, bar.Y + th.padY * 0.5f,
                             badgeW, barH - th.padY);
        {
            Gdiplus::GraphicsPath pp;
            AddRoundRect(pp, badge, badge.Height / 2.0f);
            Gdiplus::SolidBrush bbr(th.badgeBg);
            g.FillPath(&bbr, &pp);
            g.DrawString(g_curAsciiMode ? L"英" : L"中", 1, &fBadge, badge, fmt, &brBadgeText);
        }

        REAL ix = badge.GetRight() + th.badgeGap;
        for (size_t i = 0; i < cands.size(); i++)
        {
            const bool selected = ((int)i == sel);
            Gdiplus::RectF cell(ix, bar.Y + th.padY * 0.5f, itemW[i], barH - th.padY);
            if (selected)
            {
                Gdiplus::GraphicsPath hp;
                AddRoundRect(hp, cell, th.radius - 2.0f);
                Gdiplus::SolidBrush hb(th.selBg);
                g.FillPath(&hb, &hp);
            }

            wchar_t num[8];
            swprintf_s(num, L"%d", (int)i + 1);
            Gdiplus::RectF nrc(cell.X + th.padX, cell.Y, numW[i], cell.Height);
            Gdiplus::RectF trc(nrc.GetRight() + th.numGap, cell.Y, textW[i], cell.Height);
            g.DrawString(num, (INT)wcslen(num), &fNum, nrc, fmt,
                         selected ? &brSelNum : &brNum);
            g.DrawString(cands[i].c_str(), (INT)cands[i].size(), &fText, trc, fmt,
                         selected ? &brSelText : &brText);

            ix = cell.GetRight() + th.gap;
        }
    }

    // ---- 提交到分层窗口 ----
    POINT ptSrc = { 0, 0 };
    POINT ptDst = { x, y };
    SIZE  size  = { winW, winH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hCandWnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
    // 只在还没显示时才 ShowWindow。已经可见的话，UpdateLayeredWindow 会原子地
    // 换掉位置+尺寸+内容，不需要先藏再显 —— 那正是打字时闪的原因。
    if (!g_candVisible)
    {
        // 只在「隐藏 -> 显示」这一刻重新置顶，一次一行，不可见也不闪。
        // 之前是每次按键都 SetWindowPos(HWND_TOPMOST)，那正是闪烁的来源；
        // 但完全不置顶又不行 —— 置顶窗口（任务栏、悬浮窗等）被激活后会盖住它。
        SetWindowPos(g_hCandWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        ShowWindow(g_hCandWnd, SW_SHOWNOACTIVATE);
        g_candVisible = true;
    }

    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    DeleteObject(hbm);
    ReleaseDC(NULL, hdcScreen);

    DbgLog(L"[Cand] pos=(%d,%d) size=(%dx%d) n=%d sel=%d ascii=%d",
           x, y, winW, winH, (int)cands.size(), sel, g_curAsciiMode ? 1 : 0);
}

LRESULT CALLBACK CandWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        // 状态徽标只闪一下就收，别挡视线
        if (wParam == 1)
        {
            KillTimer(hwnd, 1);
            if (!g_showCandidates) { ShowWindow(hwnd, SW_HIDE); g_candVisible = false; }
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureCandWindow()
{
    if (g_hCandWnd) return;
    if (!g_candWndRegistered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = CandWndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kCandWndClass;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.style = 0;   // 内容全部由 UpdateLayeredWindow 提供，不需要重绘标志
        // 注册成功才置位。否则以后每次都跳过注册、CreateWindowEx 一直失败，
        // 却看不出原因（表现为"候选框永远不出现"）。
        // ERROR_CLASS_ALREADY_EXISTS 说明类已经在了，那种情况算成功。
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            g_candWndRegistered = true;
    }
    g_hCandWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kCandWndClass, L"", WS_POPUP,
        0, 0, 8, 8, NULL, NULL, g_hInst, NULL);
}

static void HideCandWindow()
{
    if (g_hCandWnd)
    {
        KillTimer(g_hCandWnd, 1);
        if (g_candVisible)
        {
            ShowWindow(g_hCandWnd, SW_HIDE);
            g_candVisible = false;
        }
    }
    g_showCandidates = false;
    g_candList.clear();
}

static void ShowCandWindow(const RECT& caretRect, const std::vector<std::wstring>& cands, int sel)
{
    if (cands.empty()) { HideCandWindow(); return; }
    EnsureCandWindow();
    if (!g_hCandWnd) return;

    // 关键：这里不要 ShowWindow(SW_HIDE)，更不要把窗口缩成 8x8。
    // UpdateLayeredWindow 本身就会原子地更新「位置 + 尺寸 + 内容」，
    // 提前藏起来或改尺寸只会让候选框在每次按键时闪一下。
    g_candList = cands;
    g_candSel = sel;
    g_showCandidates = true;
    KillTimer(g_hCandWnd, 1);
    RenderCandWindow(caretRect, true, g_candList, g_candSel);
}

// 中英状态变化时闪一下徽标。以前完全没有状态提示，切到英文后打不出汉字
// 只能干瞪眼 —— 这条提示就是为了不再出现那种情况。
static void ShowModeBadge(const RECT& caretRect, bool haveCaret)
{
    if (g_showCandidates) return;   // 候选列表正开着，别抢
    EnsureCandWindow();
    if (!g_hCandWnd) return;
    std::vector<std::wstring> none;
    RenderCandWindow(caretRect, haveCaret, none, -1);
    SetTimer(g_hCandWnd, 1, 800, NULL);
}


// ---------------- 喵喵风格转换（对齐原 APK TextProcessor） ----------------
static const wchar_t* const kEmoticons[] = {
    L"^⌯𖥦⌯^ ੭ ^", L"⌯'ㅅ'⌯", L"=^𖥦^=", L"⌯•ㅅ•⌯", L"ฅ•̀∀•́ฅ",
    L"ฅ ̳͒•ˑ̫• ̳͒ฅ♡", L"ฅ(̳•·̫•̳ฅ)♡", L"ฅ^••^ฅ", L"=^•ω•^=", L"₍^ >ヮ<^₎",
    L"/ᐠ - ˕ -マ Ⳋ", L"ฅ^•ﻌ•^ฅ", L"ฅ՞•ﻌ•՞ฅ", L"(ฅ´ω`ฅ)", L"ฅ(*`ω´*)ฅ",
    L"ฅ꒰ ⸝˶• •˶⸝꒱ฅ", L"₍˄·͈༝·͈˄*₎◞ ̑̑", L"!!^⌯𖥦⌯^ ੭!!", L"₍^⸝⸝> ·̫ <⸝⸝ ^₎", L"ฅ^._.^ฅ",
    L"₍🎀˄•͈༝•͈˄₎ฅ˒˒", L"^•͈༝•^ฅ", L"꒰ఎ(^ . ֑ .^)໒꒱", L"ฅ●ω●ฅ", L"₍⸍⸌·͈༝·͈⸍⸌₎◞",
    L"(>^ω^<)", L"ฅ^-﹃-^ฅ", L"^ ̳ට ̫ ට ̳^", L"୧₍˄·͈༝·͈˄₎୨", L"^ ̳ᴗ  ̫ ᴗ ̳^",
    L"˓˓ก(⸍⸌̣ʷ̣̫⸍̣⸌₎ค˒˒", L"ヽ(ฅ≧へ≦)ฅ", L"(`･ω･´)ฅ", L"(=^･ᴥ･^=)", L"(^ω^ฅ)",
    L"ฅ(≧▽≦)ฅ", L"ฅ(=´▽`=)ฅ", L"ヾ((๑˘ㅂ˘๑)ฅ", L"(ฅ◑ω◑ฅ)", L"(๑•̀ω•́ฅ)",
    L"(ฅ>ω<*ฅ)", L"(=^.^=)", L"(=´ᴥ`)", L"(=ↀωↀ=)", L"(=^-ω-^=)",
    L"ฅ(*°ω°*ฅ)", L"ヽ(=^･ω･^=)丿", L"(^•ᴥ•^)", L"( Φ ω Φ )", L"(=^x^=)",
    L"ฅ( ̳• ◡ • ̳)ฅ", L"o( =•ω•= )m", L"~o( =∩ω∩= )m", L"≡ω≡", L"ฅ(=^·ω·^=)ฅ",
    L"(=^-ω-^=)", L"ฅ(•ω•)ฅ", L"ฅ(´˙ω˙`)ฅ", L"ฅ(´∪ω∪`)ฅ", L"ฅ(¯·ω·¯)ฅ",
    L"ฅ(¯│ω│¯)ฅ", L"ฅ(´│ω│`)ฅ", L"ฅ(´˚ω˚`)ฅ", L"ฅ(·ω·`)ฅ", L"(ฅ´◔ω◔`)ฅ",
    L"ฅ(≡ω≡)ฅ", L"(=^x_x^=)", L"(TωT)", L"(¯³°)ω(°³¯)", L"ฅ(´·ω·`)ฅ",
    L"(ฅ´˕ω˕`)ฅ", L"ฅ(´◡ω◡`)ฅ", L"ฅ(˔ω˔)ฅ", L"ฅ(´◕ω◕`)ฅ"
};

static bool IsSpaceW(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

static std::wstring TrimW(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && IsSpaceW(s[b])) b++;
    while (e > b && IsSpaceW(s[e - 1])) e--;
    return s.substr(b, e - b);
}

static std::wstring ReplaceAllW(std::wstring s, const std::wstring& from, const std::wstring& to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos)
    {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
    return s;
}

static bool IsSepW(wchar_t c)
{
    return c == L'，' || c == L',' || c == L'。' || c == L'！' ||
           c == L'!' || c == L'？' || c == L'?' || IsSpaceW(c);
}

static std::wstring AddMeowW(const std::wstring& text, const std::wstring& suffix)
{
    std::vector<std::wstring> parts;
    std::vector<std::wstring> seps;
    size_t lastEnd = 0;
    size_t i = 0;
    size_t n = text.size();
    while (i < n)
    {
        if (IsSepW(text[i]))
        {
            size_t j = i;
            while (j < n && IsSepW(text[j])) j++;
            parts.push_back(text.substr(lastEnd, i - lastEnd));
            seps.push_back(text.substr(i, j - i));
            lastEnd = j;
            i = j;
        }
        else
        {
            i++;
        }
    }
    if (lastEnd < n)
        parts.push_back(text.substr(lastEnd));
    else if (!parts.empty() && lastEnd == n)
        parts.push_back(L"");
    if (parts.empty())
        parts.push_back(text);

    std::wstring result;
    for (size_t k = 0; k < parts.size(); k++)
    {
        std::wstring part = TrimW(parts[k]);
        if (!part.empty())
        {
            result += part;
            result += suffix;
        }
        if (k < seps.size())
            result += seps[k];
    }
    std::wstring resultStr = TrimW(result);
    if (resultStr.empty())
        return text + suffix;
    return resultStr;
}

static std::wstring GetRandomEmoticonW()
{
    static bool seeded = false;
    if (!seeded) { srand((unsigned)GetTickCount64()); seeded = true; }
    size_t n = sizeof(kEmoticons) / sizeof(kEmoticons[0]);
    if (n == 0) return L"";
    return kEmoticons[rand() % n];
}

// 将「原文」转换为喵喵风格（使用与原 APK 一致的默认配置）
static std::wstring TransformText(const std::wstring& original)
{
    bool enableMeow = true;
    bool enableWoToBenmiao = true;
    bool enableNiToZhuren = false;
    bool enableRandomEmoticon = true;
    bool enableMaidMode = false;
    bool oho = false;

    if (original.empty()) return original;
    std::wstring text = TrimW(original);
    if (text.empty()) return original;

    std::wstring suffix = oho ? L"哦齁齁齁♥" : L"喵";
    std::wstring wo = oho ? L"我..我我" : L"本喵";
    std::wstring ni = oho ? L"主..主人♥" : L"主人";
    std::wstring maidSuffix = oho ? L"哦齁齁齁♥~" : L"喵~";

    if (enableWoToBenmiao)
        text = ReplaceAllW(text, L"我", wo);
    if (enableNiToZhuren)
    {
        text = ReplaceAllW(text, L"你", ni);
        if (enableMaidMode)
        {
            text = ReplaceAllW(text, L"好的", L"好的主人" + maidSuffix);
            text = ReplaceAllW(text, L"知道了", L"知道了主人" + maidSuffix);
            text = ReplaceAllW(text, L"是的", L"是的主人" + maidSuffix);
            text = ReplaceAllW(text, L"可以", L"可以" + maidSuffix);
            text = ReplaceAllW(text, L"嗯", L"嗯" + maidSuffix);
        }
    }
    if (enableMeow)
        text = AddMeowW(text, suffix);
    if (enableRandomEmoticon)
    {
        std::wstring em = GetRandomEmoticonW();
        if (!em.empty())
            return text + L" " + em;
    }
    return text;
}

// ── 喵化触发字符集（日常使用）────────────────────────────────
// 这些标点上屏后，把本段累积原文整体喵化。
//
// 关于逗号：以前你把它当触发符，但逗号打不出效果有两个原因 ——
//   1) 这里（改之前）根本没有 '，'，字符表里只有 。！？!? 和空格；
//   2) 更关键的是 ！ 和 ？ 必须按 Shift 才打得出来，而自定义的 Shift
//      逻辑会把模式切成英文并清空累积原文，所以那两个也跟着废了。
// 现在 Shift 交还给 Rime，！？ 恢复正常。
//
// 逗号默认仍然不触发，但这是有意的：AddMeowW 内部本来就按「，。！？」
// 分句逐段加「喵」，整句改写一次就能得到
//     你好喵，今天不错喵 (表情)
// 逗号单独触发的话，一条消息会冒出多个颜文字，而且打字打到一半就
// 眼看着前面的字被改掉，很打扰。真要开，把下面那行取消注释即可。
static bool IsTriggerPunctW(wchar_t c)
{
    return c == L'。' || c == L'！' || c == L'？' || c == L'…' || c == L'；'
        || c == L'!'  || c == L'?'  || c == L';'
        /* || c == L'，' || c == L',' */
        ;
}

static bool IsSentenceEndW(const std::wstring& s)
{
    if (s.empty()) return false;
    return IsTriggerPunctW(s[s.size() - 1]);
}

// 至少含一个汉字才值得喵化。否则地址栏、搜索框里随便按个句号
// 都会冒出「。喵 (表情)」。
static bool HasCJKW(const std::wstring& s)
{
    for (size_t i = 0; i < s.size(); i++)
    {
        wchar_t c = s[i];
        if (c >= 0x4E00 && c <= 0x9FFF) return true; // CJK 统一表意文字基本区
    }
    return false;
}

// 追加到累积原文，并做长度上限保护。
// 超长（一直不打句末标点也不按回车）时直接放弃这一句 —— 既避免内存无界增长，
// 也避免出现「回读校验永远对不上、喵化静默失效」这种更难查的状态。
static void AppendOriginalW(std::wstring& s, const std::wstring& add)
{
    s += add;
    if (s.size() > kMaxSessionOriginal)
    {
        s.clear();
        DbgLog(L"[Xform] 累积原文超过 %d 字，放弃这一句", (int)kMaxSessionOriginal);
    }
}

static void AppendOriginalW(std::wstring& s, wchar_t c)
{
    s += c;
    if (s.size() > kMaxSessionOriginal)
        s.clear();
}

// ---------------- 文本服务类 ----------------
class CCatTextService :
    public ITfTextInputProcessor,
    public ITfKeyEventSink,
    public ITfEditSession,
    public ITfCompositionSink
{
private:
    // EA_TRANSFORM：只做「喵化改写」，不碰组合/上屏。
    // 必须和 EA_UPDATE 分开：EA_UPDATE 会拿 m_commitW 去写组合，而 Enter
    // 触发时 m_commitW 还是上一次按键留下的陈旧内容，会重复上屏一个字。
    enum EditAction { EA_NONE, EA_UPDATE, EA_TRANSFORM };


    LONG m_ref;
    ITfThreadMgr* m_pThreadMgr;
    TfClientId m_tfClientId;
    ITfKeystrokeMgr* m_pKeyMgr;
    DWORD m_dwKeySinkCookie;
    ITfContext* m_pContext;
    ITfComposition* m_pComposition;
    bool m_fKeyDownPending;
    WPARAM m_lastUpForwarded;   // 已转发过抬起的键，防止 TestKeyUp/KeyUp 重复转发
    EditAction m_action;

    // 一次按键后的 Rime 状态
    std::wstring m_commitW;
    std::wstring m_preeditW;
    std::vector<std::wstring> m_candidates;
    UINT m_candSel;

    // 喵喵改写：会话累积的「原文」与触发标记
    std::wstring m_sessionOriginal;
    bool m_pendingTransform;

    // 组合文本的屏幕位置（候选窗口定位）
    RECT m_caretRect;
    bool m_hasCaret;

public:
    CCatTextService() :
        m_ref(1), m_pThreadMgr(NULL), m_tfClientId((TfClientId)0),
        m_pKeyMgr(NULL), m_dwKeySinkCookie(TF_INVALID_COOKIE), m_pContext(NULL),
        m_pComposition(NULL),
        m_fKeyDownPending(false), m_lastUpForwarded(0), m_action(EA_NONE),
        m_candSel(0), m_pendingTransform(false), m_hasCaret(false)
    {
        ZeroMemory(&m_caretRect, sizeof(m_caretRect));
    }

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj) return E_INVALIDARG;
        *ppvObj = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITfTextInputProcessor)))
            *ppvObj = static_cast<ITfTextInputProcessor*>(this);
        else if (IsEqualIID(riid, __uuidof(ITfKeyEventSink)))
            *ppvObj = static_cast<ITfKeyEventSink*>(this);
        else if (IsEqualIID(riid, __uuidof(ITfEditSession)))
            *ppvObj = static_cast<ITfEditSession*>(this);
        else if (IsEqualIID(riid, __uuidof(ITfCompositionSink)))
            *ppvObj = static_cast<ITfCompositionSink*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // ---- ITfCompositionSink ----
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition)
    {
        UNREFERENCED_PARAMETER(ecWrite);   // 接口要求有，但这里用不上（不写文档）
        if (m_pComposition == pComposition)
        {
            m_pComposition->Release();
            m_pComposition = NULL;
        }
        return S_OK;
    }

    // ---- ITfTextInputProcessor ----
    STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId)
    {
        m_pThreadMgr = pThreadMgr;
        m_pThreadMgr->AddRef();
        m_tfClientId = tfClientId;

        if (SUCCEEDED(pThreadMgr->QueryInterface(__uuidof(ITfKeystrokeMgr), (void**)&m_pKeyMgr)) && m_pKeyMgr)
        {
            m_pKeyMgr->AdviseKeyEventSink(m_tfClientId, static_cast<ITfKeyEventSink*>(this), TRUE);
        }

        ITfDocumentMgr* pDocMgr = NULL;
        if (SUCCEEDED(pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr)
        {
            ITfContext* pContext = NULL;
            if (SUCCEEDED(pDocMgr->GetTop(&pContext)) && pContext)
            {
                if (m_pContext) m_pContext->Release();
                m_pContext = pContext;
            }
            pDocMgr->Release();
        }
        return S_OK;
    }

    STDMETHODIMP Deactivate()
    {
        if (m_pKeyMgr)
        {
            m_pKeyMgr->UnadviseKeyEventSink(m_tfClientId);
            m_pKeyMgr->Release();
            m_pKeyMgr = NULL;
        }
        HideCandWindow();
        m_sessionOriginal.clear();
        m_pendingTransform = false;
        if (m_pComposition)
        {
            m_pComposition->Release();
            m_pComposition = NULL;
        }
        if (m_pContext)
        {
            m_pContext->Release();
            m_pContext = NULL;
        }
        if (m_pThreadMgr)
        {
            m_pThreadMgr->Release();
            m_pThreadMgr = NULL;
        }
        m_tfClientId = (TfClientId)0;
        return S_OK;
    }

    // ---- 处理一次按键：返回是否被 Rime 吃掉 ----
    bool ProcessKey(ITfContext* pic, WPARAM wParam, LPARAM lParam)
    {
        if (m_pContext != pic)
        {
            if (pic) pic->AddRef();
            if (m_pContext) m_pContext->Release();
            m_pContext = pic;
        }
        if (!m_pContext) return false;
        if (!EnsureRime()) return false;

        // 新的一次按下，允许这个键的抬起再被转发一次
        m_lastUpForwarded = 0;

        // 注意：这里以前有一段自定义的 Shift 处理（自己切 ascii_mode）。
        // 它有两个致命副作用，已删除：
        //   1) 打 ! 和 ? 必须按 Shift，Shift 一按下就把模式切成英文，
        //      后续的 1 / / 在英文模式下被 Rime 直接放行，标点再也出不来；
        //   2) 同一处还把 m_sessionOriginal 清空，把已经打好的整句丢掉。
        // 现在 Shift / Caps 原样交给 Rime 的 ascii_composer 处理，见
        // VkToRimeKey 与 ForwardKeyUp：单按一下 Shift 切中英、Shift+字母
        // 临时英文、Caps Lock 切中英，都由 rime_data/default.yaml 配置决定。

        int keycode = 0, mask = 0;
        if (!VkToRimeKey(wParam, lParam, keycode, mask))
        {
            DbgLog(L"[Key] vk=0x%02X UNMAPPED", (int)wParam);
            return false;
        }

        DbgLog(L"[Key] vk=0x%02X keycode=0x%04X mask=0x%08X", (int)wParam, keycode, mask);
        if (!g_api->process_key(g_session, keycode, mask))
        {
            // Rime 不处理的键会直接上屏到应用，这里同步维护会话原文，
            // 保证句末改写按长度回退时不会错位（例如数字）。
            // 注意：keycode 是 X11 keysym，只有可见 ASCII 才等于 Unicode 字符；
            // BackSpace 等特殊键（>=0xFF00）不能当字符追加（否则会产生乱字符）。
            bool isEnter = (keycode == XK_Return);
            // Shift/Ctrl/Alt+Enter 在聊天软件里通常是「换行」而不是「发送」，
            // 这时候不该抢先把消息喵化掉。
            bool modHeld = ((GetKeyState(VK_SHIFT)   & 0x8000) != 0) ||
                           ((GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
                           ((GetKeyState(VK_MENU)    & 0x8000) != 0);

            if (keycode == XK_BackSpace)
            {
                // 退格在应用里删掉的是「一个字符」，而 std::wstring 按 UTF-16
                // code unit 计数：emoji 这类代理对占两个 unit，只 pop 一次会
                // 留下半个代理，后面的偏移就全错了（会被回读校验拦下，
                // 代价是这一句之后喵化静默失效）。所以先吃掉低代理。
                if (m_sessionOriginal.size() >= 2)
                {
                    wchar_t last = m_sessionOriginal[m_sessionOriginal.size() - 1];
                    if (last >= 0xDC00 && last <= 0xDFFF)
                        m_sessionOriginal.pop_back();
                }
                if (!m_sessionOriginal.empty())
                    m_sessionOriginal.pop_back();
            }
            else if (isEnter)
            {
                // 保留累积原文，交给下面的 Enter 喵化；绝不能走 clear 分支
            }
            else if ((mask & (kControlMask | kAltMask)) == 0 &&
                     keycode >= 0x20 && keycode <= 0x7E)
            {
                AppendOriginalW(m_sessionOriginal, (wchar_t)keycode);
            }
            else if (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
                     wParam == VK_CAPITAL || wParam == VK_CONTROL || wParam == VK_MENU)
            {
                // 纯修饰键不影响句子连续性，不要清空累积原文。
                // （以前 Shift 走到这里会把已经打好的整句抹掉）
            }
            else
            {
                // 光标移动/功能键/Ctrl、Alt 组合键会破坏句子连续性，重置累积原文
                m_sessionOriginal.clear();
            }
            DbgLog(L"[Key]   not-handled session='%s'", m_sessionOriginal.c_str());

            // ── Enter 触发喵化（聊天场景的主要路径）──
            // 打完一句按 Enter 发送是最常走的一条路。以前这里直接 return，
            // 累积的整句被丢掉，所以聊天时喵化几乎从不生效。
            // RequestEditSession 用的是 TF_ES_SYNC，改写会在宿主处理 Enter
            // 之前就落到文档里，所以发出去的已经是喵化后的文本。
            if (isEnter && !modHeld && HasCJKW(m_sessionOriginal))
            {
                m_action = EA_TRANSFORM;
                HRESULT hrInner = S_OK;
                HRESULT hrReq = m_pContext->RequestEditSession(
                    m_tfClientId, this, TF_ES_SYNC | TF_ES_READWRITE, &hrInner);
                m_action = EA_NONE;
                DbgLog(L"[Key]   Enter 喵化 ReqEdit=0x%08X inner=0x%08X",
                       (int)hrReq, (int)hrInner);
                if (FAILED(hrReq) || FAILED(hrInner))
                    m_sessionOriginal.clear(); // 改写没成功，别留下错位的偏移
            }
            NotifyModeIfChanged();
            return false;
        }

        // 读取 Rime 状态（上屏文本 / 组合文本 / 候选）
        m_commitW.clear();
        m_preeditW.clear();
        m_candidates.clear();
        {
            RIME_STRUCT(RimeCommit, c);
            if (g_api->get_commit(g_session, &c))
            {
                if (c.text) m_commitW = Utf8ToWide(c.text);
                g_api->free_commit(&c);
            }
            RIME_STRUCT(RimeContext, ctx);
            if (g_api->get_context(g_session, &ctx))
            {
                if (ctx.composition.preedit) m_preeditW = Utf8ToWide(ctx.composition.preedit);
                m_candSel = (UINT)ctx.menu.highlighted_candidate_index;
                for (int i = 0; i < ctx.menu.num_candidates; i++)
                {
                    if (ctx.menu.candidates[i].text)
                        m_candidates.push_back(Utf8ToWide(ctx.menu.candidates[i].text));
                }
                g_api->free_context(&ctx);
            }
        }

        // 累积上屏原文，句末标点触发整句改写
        if (!m_commitW.empty())
        {
            AppendOriginalW(m_sessionOriginal, m_commitW);
            // 这条和 Enter 那条路必须用同一个守卫。少了 HasCJKW 的话，
            // 单独打个「！」就会变成「！ =表情」—— 日志里实测到过这条：
            //   [Xform] session='！' -> '！ =^•ω•^='
            if (IsSentenceEndW(m_sessionOriginal) && HasCJKW(m_sessionOriginal))
                m_pendingTransform = true;
        }
        DbgLog(L"[Key]   commit='%s' preedit='%s' session='%s' pending=%d",
            m_commitW.c_str(), m_preeditW.c_str(), m_sessionOriginal.c_str(),
            m_pendingTransform ? 1 : 0);

        // 同步刷新组合/上屏
        m_action = EA_UPDATE;
        HRESULT hr = S_OK;
        HRESULT hrReq = m_pContext->RequestEditSession(m_tfClientId, this, TF_ES_SYNC | TF_ES_READWRITE, &hr);
        m_action = EA_NONE;
        DbgLog(L"[Key] ReqEdit=0x%08X inner=0x%08X", (int)hrReq, (int)hr);

        // 刷新候选窗口
        UpdateCandidateUI();
        NotifyModeIfChanged();
        return true;
    }

    void UpdateCandidateUI()
    {
        DbgLog(L"[UI] cand=%d hasCaret=%d preedit='%s' rect=(%d,%d,%d,%d)",
            (int)m_candidates.size(), m_hasCaret ? 1 : 0, m_preeditW.c_str(),
            m_caretRect.left, m_caretRect.top, m_caretRect.right, m_caretRect.bottom);

        if (m_candidates.empty() || !m_hasCaret)
        {
            HideCandWindow();
            return;
        }
        ShowCandWindow(m_caretRect, m_candidates, (int)m_candSel);
    }

    // ---- ITfKeyEventSink ----
    //
    // 下面这几个是 COM/TSF 的方法边界，**绝不能让异常逃出去**。
    // ProcessKey 里用到 std::wstring / std::vector（累积原文、候选列表），
    // 低内存时会抛 bad_alloc；异常穿过 COM 边界是未定义行为，最坏直接
    // 带走宿主进程 —— 而这是个 in-proc DLL，宿主就是用户的 QQ/浏览器。
    // 所以一律兜住，出错时按「不吃这个键」处理：用户至少还能打出原始字符。
    STDMETHODIMP OnSetFocus(BOOL) { return S_OK; }

    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
    {
        if (pfEaten) *pfEaten = FALSE;
        if (m_fKeyDownPending)
        {
            if (pfEaten) *pfEaten = TRUE;
            return S_OK;
        }
        try
        {
            bool eaten = ProcessKey(pic, wParam, lParam);
            if (eaten) m_fKeyDownPending = true;
            if (pfEaten) *pfEaten = eaten ? TRUE : FALSE;
        }
        catch (...)
        {
            DbgLog(L"[Fatal] ProcessKey 抛异常，本键透传");
            if (pfEaten) *pfEaten = FALSE;
        }
        return S_OK;
    }

    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
    {
        if (pfEaten) *pfEaten = FALSE;
        if (m_fKeyDownPending)
        {
            m_fKeyDownPending = false;
            if (pfEaten) *pfEaten = TRUE;
            return S_OK;
        }
        try
        {
            bool eaten = ProcessKey(pic, wParam, lParam);
            if (pfEaten) *pfEaten = eaten ? TRUE : FALSE;
        }
        catch (...)
        {
            DbgLog(L"[Fatal] ProcessKey 抛异常，本键透传");
            if (pfEaten) *pfEaten = FALSE;
        }
        return S_OK;
    }

    // 把修饰键的「抬起」转发给 Rime。
    // Rime 的 ascii_composer 靠这个事件实现「单按一下 Shift 切中英」：
    // 实测 Shift 按下不切、抬起时才翻转 ascii_mode（default.yaml 里
    // Shift_L: inline_ascii）。以前从不转发抬起、也不把 Shift 送进 Rime，
    // 所以 Rime 自带这套行为完全是摆设。
    //
    // TestKeyUp 和 KeyUp 都要转发：按 TSF 惯例，很多宿主只在
    // OnTestKeyUp 返回「已吃掉」时才调 OnKeyUp，而这个 TIP 永远返回 FALSE，
    // 于是只挂 OnKeyUp 的话 Shift 抬起在某些程序里根本收不到，中英切不动。
    // 用 m_lastUpForwarded 去重，保证同一个键的抬起只翻转一次
    // （转发两次会翻两次，等于没切）。
    void ForwardKeyUp(WPARAM wParam, LPARAM lParam)
    {
        if (!g_api || !g_session) return;
        if (wParam != VK_SHIFT && wParam != VK_LSHIFT &&
            wParam != VK_RSHIFT && wParam != VK_CAPITAL) return;
        if (m_lastUpForwarded == wParam) return;

        int keycode = 0, mask = 0;
        if (!VkToRimeKey(wParam, lParam, keycode, mask)) return;

        m_lastUpForwarded = wParam;
        g_api->process_key(g_session, keycode, mask | kReleaseMask);
        DbgLog(L"[KeyUp] vk=0x%02X keycode=0x%04X", (int)wParam, keycode);
        NotifyModeIfChanged();
    }

    // 中英状态一变就闪一下徽标。以前完全没有状态提示，切到英文后打不出
    // 汉字只能干瞪眼，这条提示就是为了不再出现那种情况。
    void NotifyModeIfChanged()
    {
        if (!g_api || !g_session) return;
        bool asciiNow = (g_api->get_option(g_session, "ascii_mode") != False);
        if (asciiNow == g_curAsciiMode) return;
        g_curAsciiMode = asciiNow;
        DbgLog(L"[Mode] ascii_mode=%d", asciiNow ? 1 : 0);
        ShowModeBadge(m_caretRect, m_hasCaret);
    }

    STDMETHODIMP OnTestKeyUp(ITfContext*, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
    {
        m_fKeyDownPending = false;
        ForwardKeyUp(wParam, lParam);
        if (pfEaten) *pfEaten = FALSE;
        return S_OK;
    }
    STDMETHODIMP OnKeyUp(ITfContext*, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
    {
        m_fKeyDownPending = false;
        ForwardKeyUp(wParam, lParam);
        if (pfEaten) *pfEaten = FALSE;
        return S_OK;
    }
    STDMETHODIMP OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten) { if (pfEaten) *pfEaten = FALSE; return S_OK; }

    // ---- ITfEditSession ----
    STDMETHODIMP DoEditSession(TfEditCookie ec)
    {
        if (!m_pContext) return S_OK;
        // 同样是 COM 边界：DoTransform 里会构造 std::wstring、做子串比较，
        // 异常逃出去会直接掀掉宿主的编辑会话。
        try
        {
            // Enter 触发的喵化：只改写文档文本，不碰组合与上屏
            if (m_action == EA_TRANSFORM)
            {
                DoTransform(ec);
                return S_OK;
            }
            if (m_action != EA_UPDATE) return S_OK;

            UpdateComposition(ec, m_commitW, m_preeditW);
            if (m_pendingTransform)
            {
                DoTransform(ec);
                m_pendingTransform = false;
            }
        }
        catch (...)
        {
            DbgLog(L"[Fatal] DoEditSession 抛异常，放弃本次编辑");
            m_sessionOriginal.clear();
            m_pendingTransform = false;
        }
        return S_OK;
    }

    // 把光标前「本会话上屏的原文」替换为喵喵风格文本。
    // 改写前做回读校验：确认退回去的那一段确实就是累积的原文，对不上就整个放弃。
    // 绝不能盲改 —— 光标位置一旦有偏差（应用自己动过选区、跨字段、只读区、
    // 删过 emoji 导致 pop_back 计数漂移），盲改会吃掉文档里别的文字。
    void DoTransform(TfEditCookie ec)
    {
        if (m_sessionOriginal.empty()) return;

        std::wstring original = m_sessionOriginal;
        std::wstring transformed = TransformText(original);
        DbgLog(L"[Xform] session='%s' -> '%s'", original.c_str(), transformed.c_str());

        TF_SELECTION sel;
        ULONG cFetched = 0;
        if (FAILED(m_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched)) || cFetched != 1)
        {
            m_sessionOriginal.clear();
            return;
        }

        LONG cch = 0;
        sel.range->ShiftStart(ec, -(LONG)original.length(), &cch, NULL);
        DbgLog(L"[Xform] shiftBack=%d moved=%d", (int)original.length(), (int)cch);

        // —— 回读校验：退够了长度 + 读回来的内容逐字相同，才允许改写 ——
        bool ok = (cch == -(LONG)original.length());
        if (ok)
        {
            wchar_t buf[1024] = { 0 };
            ULONG cchRead = 0;
            if (SUCCEEDED(sel.range->GetText(ec, 0, buf, 1023, &cchRead)) &&
                cchRead == (ULONG)original.length())
            {
                if (original.compare(0, original.length(), buf, cchRead) != 0)
                    ok = false;
            }
            else
            {
                ok = false;
            }
        }

        if (!ok)
        {
            DbgLog(L"[Xform] 回读校验不通过，放弃改写（宁可不动，也不覆盖别处的文字）");
            sel.range->Release();
            m_sessionOriginal.clear();
            return;
        }

        sel.range->SetText(ec, 0, transformed.c_str(), (LONG)transformed.length());
        sel.range->Collapse(ec, TF_ANCHOR_END);
        m_pContext->SetSelection(ec, 1, &sel);
        sel.range->Release();

        m_sessionOriginal.clear();
    }

    void UpdateCaretRectFromComp(TfEditCookie ec, ITfComposition* pComp)
    {
        m_hasCaret = false;
        if (!pComp) return;

        ITfRange* pRange = NULL;
        if (SUCCEEDED(pComp->GetRange(&pRange)) && pRange)
        {
            pRange->Collapse(ec, TF_ANCHOR_START);
            ITfContextView* pView = NULL;
            if (SUCCEEDED(m_pContext->GetActiveView(&pView)) && pView)
            {
                BOOL clipped = FALSE;
                RECT rc = { 0 };
                if (SUCCEEDED(pView->GetTextExt(ec, pRange, &rc, &clipped)))
                {
                    m_caretRect = rc;
                    m_hasCaret = true;
                }
                pView->Release();
            }
            pRange->Release();
        }
    }

    void UpdateComposition(TfEditCookie ec, const std::wstring& commitW, const std::wstring& preeditW)
    {
        ITfContextComposition* pCtxComp = NULL;
        if (FAILED(m_pContext->QueryInterface(__uuidof(ITfContextComposition), (void**)&pCtxComp)) || !pCtxComp)
            return;

        // 1) 无内容：结束当前组合
        if (commitW.empty() && preeditW.empty())
        {
            if (m_pComposition)
            {
                ITfRange* pRange = NULL;
                if (SUCCEEDED(m_pComposition->GetRange(&pRange)) && pRange)
                {
                    pRange->SetText(ec, 0, L"", 0);
                    pRange->Release();
                }
                ITfComposition* pOld = m_pComposition;
                m_pComposition = NULL;
                pOld->EndComposition(ec);
                pOld->Release();
            }
        }
        // 2) 上屏
        else if (!commitW.empty())
        {
            if (m_pComposition)
            {
                ITfRange* pRange = NULL;
                if (SUCCEEDED(m_pComposition->GetRange(&pRange)) && pRange)
                {
                    pRange->SetText(ec, 0, commitW.c_str(), (LONG)commitW.length());
                    pRange->Collapse(ec, TF_ANCHOR_END);
                    TF_SELECTION sel;
                    sel.range = pRange;
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    m_pContext->SetSelection(ec, 1, &sel);
                    pRange->Release();
                }
                ITfComposition* pOld = m_pComposition;
                m_pComposition = NULL;
                pOld->EndComposition(ec);
                pOld->Release();
            }
            else
            {
                // 无组合时上屏（如标点）：InsertTextAtSelection 直接写入不可靠，
                // 这里用「查询模式」拿到插入位置后建立临时组合，写入文本再立即结束，
                // 复用组合写入机制确保文本真正上屏。
                ITfInsertAtSelection* pInsert = NULL;
                if (SUCCEEDED(m_pContext->QueryInterface(__uuidof(ITfInsertAtSelection), (void**)&pInsert)) && pInsert)
                {
                    ITfRange* pRangeComp = NULL;
                    if (SUCCEEDED(pInsert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0, &pRangeComp)) && pRangeComp)
                    {
                        ITfComposition* pNew = NULL;
                        HRESULT hrStart = pCtxComp->StartComposition(ec, pRangeComp, static_cast<ITfCompositionSink*>(this), &pNew);
                        if (SUCCEEDED(hrStart) && pNew)
                        {
                            ITfRange* pRange = NULL;
                            if (SUCCEEDED(pNew->GetRange(&pRange)) && pRange)
                            {
                                pRange->SetText(ec, 0, commitW.c_str(), (LONG)commitW.length());
                                pRange->Collapse(ec, TF_ANCHOR_END);
                                TF_SELECTION sel;
                                sel.range = pRange;
                                sel.style.ase = TF_AE_NONE;
                                sel.style.fInterimChar = FALSE;
                                m_pContext->SetSelection(ec, 1, &sel);
                                pRange->Release();
                            }
                            pNew->EndComposition(ec);
                            pNew->Release();
                        }
                        pRangeComp->Release();
                    }
                    pInsert->Release();
                }
            }
        }
        // 3) 组合中
        else
        {
            // 尚未开始组合则先开始（用查询模式拿到插入位置的空 range）
            if (!m_pComposition)
            {
                ITfInsertAtSelection* pInsert = NULL;
                ITfRange* pRangeComp = NULL;
                if (SUCCEEDED(m_pContext->QueryInterface(__uuidof(ITfInsertAtSelection), (void**)&pInsert)) && pInsert)
                {
                    if (SUCCEEDED(pInsert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0, &pRangeComp)) && pRangeComp)
                    {
                        ITfComposition* pNew = NULL;
                        HRESULT hrStart = pCtxComp->StartComposition(ec, pRangeComp, static_cast<ITfCompositionSink*>(this), &pNew);
                        DbgLog(L"[Comp] StartComp=0x%08X pNew=%p", (int)hrStart, pNew);
                        if (SUCCEEDED(hrStart) && pNew)
                            m_pComposition = pNew;
                        pRangeComp->Release();
                    }
                    pInsert->Release();
                }
            }

            if (m_pComposition)
            {
                ITfRange* pRange = NULL;
                if (SUCCEEDED(m_pComposition->GetRange(&pRange)) && pRange)
                {
                    pRange->SetText(ec, 0, preeditW.c_str(), (LONG)preeditW.length());
                    pRange->Collapse(ec, TF_ANCHOR_END);
                    TF_SELECTION sel;
                    sel.range = pRange;
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    m_pContext->SetSelection(ec, 1, &sel);
                    pRange->Release();
                }
                UpdateCaretRectFromComp(ec, m_pComposition);
            }
        }

        pCtxComp->Release();
    }
};

// ---------------- 类工厂 ----------------
class CCatClassFactory : public IClassFactory
{
private:
    LONG m_ref;
public:
    CCatClassFactory() : m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
            *ppv = static_cast<IClassFactory*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { ULONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv)
    {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        // 裸 new 失败会抛 bad_alloc，异常逃出 COM 边界是 UB —— 这里兜住并
        // 按规范返回 E_OUTOFMEMORY。（`if (!p)` 那种写法是死代码，new 不返回空）
        try
        {
            CCatTextService* p = new CCatTextService();
            HRESULT hr = p->QueryInterface(riid, ppv);
            p->Release();
            return hr;
        }
        catch (...)
        {
            if (ppv) *ppv = NULL;
            return E_OUTOFMEMORY;
        }
    }
    STDMETHODIMP LockServer(BOOL) { return S_OK; }
};

static void InitDllDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"\\/");
    g_dllDir = (pos == std::wstring::npos) ? L"" : full.substr(0, pos + 1);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInst;
        // 用带返回值的版本：InitializeCriticalSection 在极低内存下会**抛异常**，
        // 而在 DllMain 里抛异常等于直接杀掉宿主进程。建不起来就让本 DLL 加载失败，
        // 宿主那边只是输入法不可用（CoCreateInstance 失败），不会崩。
        if (!InitializeCriticalSectionAndSpinCount(&g_rimeCs, 0x400))
            return FALSE;
        g_csReady = true;
        InitDllDir();
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        if (g_pLog) { fclose(g_pLog); g_pLog = NULL; }
        // 不在 DllMain 里 DestroyWindow：这窗口属于宿主的 UI 线程，
        // 在加载器锁里动窗口容易和那个线程打架。进程都要退了，藏起来就行。
        if (g_hCandWnd) { ShowWindow(g_hCandWnd, SW_HIDE); g_hCandWnd = NULL; }
        if (g_api && g_session)
        {
            g_api->destroy_session(g_session);
            g_session = 0;
        }
        if (g_api)
        {
            g_api->finalize();
            g_api = NULL;
        }
        // 这里刻意不调 FreeLibrary(g_hRime)：在 DllMain 里释放别的模块会去抢
        // 加载器锁，是明确的死锁写法。DllCanUnloadNow 永远返回 S_FALSE，
        // 也就是说本模块只会随进程一起卸载 —— 到那一步系统本来就会收走 rime.dll。
        g_hRime = NULL;
        if (g_csReady) { DeleteCriticalSection(&g_rimeCs); g_csReady = false; }
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!IsEqualCLSID(rclsid, CLSID_CatTextService)) return CLASS_E_CLASSNOTAVAILABLE;
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    try
    {
        CCatClassFactory* p = new CCatClassFactory();
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    catch (...)
    {
        *ppv = NULL;
        return E_OUTOFMEMORY;
    }
}

STDAPI DllCanUnloadNow() { return S_FALSE; }

static void SetRegKey(HKEY root, const wchar_t* sub, const wchar_t* value, const wchar_t* data)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        const wchar_t* d = data ? data : L"";
        RegSetValueExW(k, value, 0, REG_SZ, (const BYTE*)d, (DWORD)((wcslen(d) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static void SetRegDword(HKEY root, const wchar_t* sub, const wchar_t* value, DWORD data)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(k, value, 0, REG_DWORD, (const BYTE*)&data, sizeof(DWORD));
        RegCloseKey(k);
    }
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hInst, path, MAX_PATH);

    SetRegKey(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", NULL, L"Cat Text Service");
    SetRegKey(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\InprocServer32", NULL, path);
    SetRegKey(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\InprocServer32", L"ThreadingModel", L"Apartment");

    SetRegKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", NULL, L"喵喵助手");
    SetRegKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\Category\\Category\\{34745C63-B2F0-4784-8B67-5E12C8701A31}\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", NULL, L"");

    SetRegKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\LanguageProfile\\0x00000804\\{3F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", NULL, L"喵喵助手");
    SetRegKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\LanguageProfile\\0x00000804\\{3F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", L"Description", L"喵喵助手文本转换");
    SetRegKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\LanguageProfile\\0x00000804\\{3F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", L"IconFile", path);
    SetRegDword(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\LanguageProfile\\0x00000804\\{3F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", L"IconIndex", 0);
    SetRegDword(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\LanguageProfile\\0x00000804\\{3F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", L"Enable", 1);

    SetRegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}\\LanguageProfile\\0x00000804\\{3F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}", L"Enable", 1);
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\CTF\\TIP\\{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}");
    return S_OK;
}
