// ads_err.cpp —— 用真正的 Win32 CreateFileW 看「打开 ADS 失败」时系统给什么错误码
//
// 为什么要单独测：C++ 探针必须区分「确实没有 Zone.Identifier」和「打不开」。
// 如果分不清，在装了安全软件的机器上就会把 ACCESS_DENIED 报成「没有标记」，
// 给出一个假的阴性结论。这里把真实错误码打出来，确认分支判断有没有写对。
//
// 注意：不能用 .NET 测 —— .NET 的路径校验会直接拒绝 "file:stream" 语法，
// 报 "The given path's format is not supported"，那是 .NET 的限制，不是系统的答案。
// 另外全程用窄字符 printf：wprintf 在重定向到管道时中文会变 '?'，且与 printf 混用时输出会错行。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>   // _countof

static const char* Explain(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return "FILE_NOT_FOUND  -> 报「没有标记」";
    case ERROR_PATH_NOT_FOUND: return "PATH_NOT_FOUND  -> 报「没有标记」";
    case ERROR_ACCESS_DENIED:  return "ACCESS_DENIED   -> 报「无法确认」";
    default:                   return "其它错误        -> 报「无法确认」";
    }
}

static void Try(const wchar_t* base)
{
    wchar_t wpath[512];
    _snwprintf_s(wpath, _countof(wpath), _TRUNCATE, L"%s:Zone.Identifier", base);

    char shown[512];
    WideCharToMultiByte(CP_UTF8, 0, base, -1, shown, sizeof(shown), NULL, NULL);

    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        printf("%-46s => 打开成功（确实带标记）\n", shown);
        CloseHandle(h);
        return;
    }
    DWORD e = GetLastError();
    printf("%-46s => err=%-3lu %s\n", shown, (unsigned long)e, Explain(e));
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    printf("--- 打开 \"<文件>:Zone.Identifier\" 的真实结果 ---\n\n");

    // 1. 普通文件、没有这个数据流
    Try(L"D:\\123\\tsf_cat\\_build\\CatTextService.dll");

    // 2. 存在的系统文件（通常不可读）—— 关键：看是 ACCESS_DENIED 还是 FILE_NOT_FOUND
    Try(L"C:\\Windows\\System32\\lsass.exe");
    Try(L"C:\\Windows\\System32\\config\\SAM");
    Try(L"C:\\Windows\\System32\\config\\SYSTEM");

    // 3. 根本不存在的文件
    Try(L"D:\\123\\tsf_cat\\__不存在的文件__.dll");

    // 4. 对照：明确带标记的文件（由调用方预先造好）
    Try(L"D:\\123\\tsf_cat\\_probe\\_adstest.bin");

    printf("\n结论：只把 %lu / %lu 当成「没有标记」，其余一律报「无法确认」。\n",
           (unsigned long)ERROR_FILE_NOT_FOUND, (unsigned long)ERROR_PATH_NOT_FOUND);
    return 0;
}
