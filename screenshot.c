/*
 * screenshot.c —— 全屏延时截图查看器
 * ================================================================
 * 相对最初简版实现的改动：
 *   [P0] WM_PAINT 无条件 BeginPaint/EndPaint —— 彻底消除无效区不校验
 *        导致的 WM_PAINT 风暴（100% CPU 空转）
 *   [P0] RegisterClassExW / CreateWindowExW / SetTimer / 位图创建全部
 *        检查返回值，失败弹窗并带错误码退出，不再静默挂死
 *   [P0] 支持多显示器：默认抓取整个虚拟桌面（SM_*VIRTUALSCREEN），
 *        源坐标取虚拟原点，修正副屏在主屏左/上方时的错位
 *   [P0] ShowWindow 后显式 SetForegroundWindow + SetFocus，保证 Home
 *        键一定收得到
 *   [P0] 拦截 WM_SYSCOMMAND 的 SC_CLOSE/SC_MINIMIZE/SC_RESTORE/
 *        SC_MOVE/SC_SIZE/SC_KEYMENU，堵住 Alt+Space / Win+Down 绕行
 *   [P1] SelectObject 一律恢复旧对象后再 DeleteDC；位图集中释放
 *   [P1] 互斥体单实例，避免重复双击叠加多个全屏窗口
 *   [P1] 退出通道：Home 或 Ctrl+Q，可选 --timeout=秒 自动退出
 *   [P2] 重绘按 ps.rcPaint 裁剪；WM_ERASEBKGND 直接返回 1 防闪烁；
 *        去掉冗余的 UpdateWindow
 *   [P2] 截图使用 SRCCOPY|CAPTUREBLT，连带分层窗口一起抓
 *   [P2] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 让本窗口在
 *        其它截屏工具中不可见（Win10 2004+，可用 --no-affinity 关闭）
 *   [P2] 鼠标光标保持可见（按需求不隐藏），仅吞掉鼠标按键与滚轮
 *   [P3] 宽字符入口 wWinMain + UNICODE（需 -municode）
 *   [P3] 每个显示器 DPI 感知（SetProcessDpiAwarenessContext，V1 回退）
 *   [P3] 参数化：--delay / --timeout / --primary / --virtual /
 *        --selftest / --log / --no-affinity / --help
 *
 * 已知边界（不是缺陷，是设计边界）：
 *   本程序拦不住 Ctrl+Alt+Del、任务管理器结束进程、Win+L 锁屏，
 *   也不阻止 Alt+Tab（WS_EX_TOOLWINDOW 已使其不出现在切换列表，
 *   但 Win 键 / 快捷键仍可切走）。它只是一个"全屏展示层"，不是
 *   安全机制；请仅在已获授权的场景使用。
 *
 * 构建：见 compile.bat
 *   gcc -o run.exe screenshot.c screenshot_res.o \
 *       -O2 -s -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8 \
 *       -municode -lgdi32 -luser32 -mwindows
 */

#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE          /* -municode 已定义时避免重复定义告警 */
#endif
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define APP_NAME            L"Screenshot Viewer"
#define CLASS_NAME          L"FullScreenShot"
#define MUTEX_NAME          L"Local\\FullScreenShot.SingleInstance"
#define DEFAULT_DELAY_MS    5000
#define MAX_DELAY_MS        3600000U
#define TIMER_ID_CAPTURE    1
#define TIMER_ID_QUIT       2

/* ---------------- 全局状态 ---------------- */
static HBITMAP g_hBitmap  = NULL;
static int     g_srcX     = 0;      /* 截图源原点（虚拟屏坐标） */
static int     g_srcY     = 0;
static int     g_bmpW     = 0;
static int     g_bmpH     = 0;

static UINT    g_delayMs  = DEFAULT_DELAY_MS;
static UINT    g_timeoutS = 0;      /* 0 = 不自动退出 */
static BOOL    g_primary  = FALSE;  /* TRUE = 只抓主屏 */
static BOOL    g_selfTest = FALSE;
static BOOL    g_affinity = TRUE;
static char    g_logPath[MAX_PATH] = "selftest.log";

/* ---------------- 小工具 ---------------- */

static void LogF(const char *fmt, ...)
{
    FILE *f = fopen(g_logPath, "ab");
    va_list ap;
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void WideToNarrow(const WCHAR *src, char *dst, size_t dstChars)
{
    if (dstChars == 0)
        return;
    if (!WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)dstChars, NULL, NULL))
        dst[0] = '\0';
}

static void ShowError(const WCHAR *what, DWORD code)
{
    WCHAR buf[320];
    wsprintfW(buf, L"%s 失败（错误码 %lu）。", what, (unsigned long)code);
    MessageBoxW(NULL, buf, APP_NAME, MB_ICONERROR | MB_OK);
}

/* 每个显示器 DPI 感知；失败回退 V1（仅主屏正确） */
static void EnableDpiAwareness(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
        union { FARPROC raw; PFN_SPDAC fn; } u;
        u.raw = GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
        if (u.fn && u.fn((HANDLE)-4))
            return;
    }
    SetProcessDPIAware();   /* manifest 已声明时会失败，属正常，忽略 */
}

/* 计算截图区域：默认整个虚拟桌面，--primary 时只取主屏 */
static BOOL ComputeCaptureRect(int *px, int *py, int *pw, int *ph)
{
    int x, y, w, h;

    if (g_primary) {
        x = 0;
        y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    } else {
        x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (w <= 0 || h <= 0) {         /* 退化回主屏 */
            x = 0;
            y = 0;
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    if (w <= 0 || h <= 0)
        return FALSE;

    *px = x;
    *py = y;
    *pw = w;
    *ph = h;
    return TRUE;
}

/* 截屏。失败返回 NULL 并写 pErr。源坐标 (x,y) 是虚拟桌面坐标 */
static HBITMAP CaptureScreen(int x, int y, int w, int h, DWORD *pErr)
{
    HDC     hScreenDC = NULL;
    HDC     hMemDC    = NULL;
    HBITMAP hBmp      = NULL;
    HGDIOBJ hOld      = NULL;

    *pErr = 0;

    hScreenDC = GetDC(NULL);
    if (!hScreenDC) {
        *pErr = GetLastError();
        return NULL;
    }

    hMemDC = CreateCompatibleDC(hScreenDC);
    if (!hMemDC) {
        *pErr = GetLastError();
        goto done;
    }

    hBmp = CreateCompatibleBitmap(hScreenDC, w, h);
    if (!hBmp) {
        *pErr = GetLastError();
        goto done;
    }

    hOld = SelectObject(hMemDC, hBmp);
    if (!hOld || hOld == HGDI_ERROR) {
        *pErr = GetLastError();
        DeleteObject(hBmp);
        hBmp = NULL;
        goto done;
    }

    if (!BitBlt(hMemDC, 0, 0, w, h, hScreenDC, x, y, SRCCOPY | CAPTUREBLT)) {
        *pErr = GetLastError();
        SelectObject(hMemDC, hOld);     /* 先恢复，再删位图 */
        DeleteObject(hBmp);
        hBmp = NULL;
        goto done;
    }

    SelectObject(hMemDC, hOld);         /* 恢复默认位图，避免 GDI 状态残留 */

done:
    if (hMemDC)
        DeleteDC(hMemDC);
    if (hScreenDC)
        ReleaseDC(NULL, hScreenDC);
    return hBmp;
}

static void ReleaseBitmap(void)
{
    if (g_hBitmap) {
        DeleteObject(g_hBitmap);
        g_hBitmap = NULL;
    }
}

/* ---------------- 截图动作 ---------------- */

static void DoCapture(HWND hwnd)
{
    int   x = 0, y = 0, w = 0, h = 0;
    DWORD err = 0;

    if (!ComputeCaptureRect(&x, &y, &w, &h)) {
        MessageBoxW(NULL, L"无法获取屏幕尺寸。", APP_NAME, MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        return;
    }

    g_hBitmap = CaptureScreen(x, y, w, h, &err);
    if (!g_hBitmap) {
        WCHAR buf[256];
        wsprintfW(buf, L"屏幕截取失败（错误码 %lu）。", (unsigned long)err);
        MessageBoxW(NULL, buf, APP_NAME, MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        return;
    }

    g_srcX = x;
    g_srcY = y;
    g_bmpW = w;
    g_bmpH = h;
    LogF("capture ok: %dx%d at (%d,%d)", w, h, x, y);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);          /* 保证键盘焦点，Home/Ctrl+Q 才有效 */
    SetFocus(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
    /* 不调用 UpdateWindow：重绘交给消息循环，少一次同步强制刷新 */

    /* 自动退出计时从"截图完成"开始，而不是从进程启动开始 */
    if (g_timeoutS) {
        if (!SetTimer(hwnd, TIMER_ID_QUIT, g_timeoutS * 1000U, NULL))
            LogF("warn: SetTimer(quit) failed: %lu", (unsigned long)GetLastError());
    }
}

/* ---------------- 窗口过程 ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_ID_CAPTURE) {
            KillTimer(hwnd, TIMER_ID_CAPTURE);
            DoCapture(hwnd);
        } else if (wParam == TIMER_ID_QUIT) {
            KillTimer(hwnd, TIMER_ID_QUIT);
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* 整屏由位图覆盖，不做背景擦除 */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);        /* 无条件配平，杜绝 WM_PAINT 风暴 */
        if (g_hBitmap) {
            HDC mem = CreateCompatibleDC(hdc);
            if (mem) {
                HGDIOBJ old = SelectObject(mem, g_hBitmap);
                if (old && old != HGDI_ERROR) {
                    RECT rc = ps.rcPaint;       /* 只重绘脏区 */
                    if (rc.left < rc.right && rc.top < rc.bottom) {
                        BitBlt(hdc, rc.left, rc.top,
                               rc.right - rc.left, rc.bottom - rc.top,
                               mem, rc.left, rc.top, SRCCOPY);
                    }
                    SelectObject(mem, old);
                }
                DeleteDC(mem);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        (void)lParam;
        if (wParam == VK_HOME ||
            (wParam == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000))) {
            DestroyWindow(hwnd);
        }
        return 0;                       /* 吞掉其它按键，避免 Esc/空格触发默认行为 */

    case WM_SYSKEYDOWN:
        return 0;                       /* 吞掉 Alt+F4 / Alt+Space 等系统组合键 */

    case WM_SYSCOMMAND: {
        UINT cmd = (UINT)(wParam & 0xFFF0);
        if (cmd == SC_CLOSE || cmd == SC_MINIMIZE || cmd == SC_RESTORE ||
            cmd == SC_MOVE  || cmd == SC_SIZE     || cmd == SC_KEYMENU)
            return 0;
        break;
    }

    /* 吞掉所有鼠标按键与滚轮；鼠标移动仍交给 DefWindowProc，
       这样系统照常绘制并显示光标（本项目有意不隐藏光标） */
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return 0;

    case WM_CLOSE:
        return 0;                       /* 只允许 Home / Ctrl+Q / 超时退出 */

    case WM_DESTROY:
        ReleaseBitmap();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------------- 命令行 ---------------- */

static const WCHAR *NextToken(const WCHAR **pp)
{
    static WCHAR buf[512];
    const WCHAR *p = *pp;
    size_t n = 0;

    while (*p == L' ' || *p == L'\t')
        ++p;
    if (!*p) {
        *pp = p;
        return NULL;
    }

    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"' && n < 511)
            buf[n++] = *p++;
        if (*p == L'"')
            ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t' && n < 511)
            buf[n++] = *p++;
    }
    buf[n] = L'\0';
    *pp = p;
    return buf;
}

/* 返回 FALSE 表示请求显示帮助 */
static BOOL ParseArgs(LPWSTR cmd)
{
    const WCHAR *p = cmd;
    const WCHAR *tok;

    while ((tok = NextToken(&p)) != NULL) {
        if (_wcsicmp(tok, L"--help") == 0 || _wcsicmp(tok, L"-h") == 0 ||
            _wcsicmp(tok, L"/?") == 0) {
            return FALSE;
        } else if (_wcsnicmp(tok, L"--delay=", 8) == 0) {
            g_delayMs = (UINT)_wtoi(tok + 8);
            if (g_delayMs == 0)
                g_delayMs = 1;
            if (g_delayMs > MAX_DELAY_MS)
                g_delayMs = MAX_DELAY_MS;
        } else if (_wcsnicmp(tok, L"--timeout=", 10) == 0) {
            g_timeoutS = (UINT)_wtoi(tok + 10);
            if (g_timeoutS > 86400U)
                g_timeoutS = 86400U;
        } else if (_wcsicmp(tok, L"--primary") == 0) {
            g_primary = TRUE;
        } else if (_wcsicmp(tok, L"--virtual") == 0) {
            g_primary = FALSE;
        } else if (_wcsicmp(tok, L"--selftest") == 0) {
            g_selfTest = TRUE;
        } else if (_wcsicmp(tok, L"--no-affinity") == 0) {
            g_affinity = FALSE;
        } else if (_wcsnicmp(tok, L"--log=", 6) == 0) {
            WideToNarrow(tok + 6, g_logPath, MAX_PATH);
        }
        /* 未知参数静默忽略，保证被其它程序带参调用时也能启动 */
    }
    return TRUE;
}

static void ShowUsage(void)
{
    MessageBoxW(NULL,
        L"Screenshot Viewer\n\n"
        L"用法：run.exe [选项]\n\n"
        L"  --delay=毫秒    截图延时，默认 5000\n"
        L"  --timeout=秒    到时自动退出（截图后计时），默认 0 不自动退出\n"
        L"  --virtual       抓取整个虚拟桌面（默认）\n"
        L"  --primary       只抓主屏\n"
        L"  --no-affinity   允许本窗口被其它截屏工具拍到\n"
        L"  --selftest      只做截图管线自检，不创建窗口，结果写入 --log 指定文件\n"
        L"  --log=文件      自检日志路径，默认 selftest.log\n"
        L"  --help          显示本帮助\n\n"
        L"运行中：Home 或 Ctrl+Q 退出；鼠标按键与滚轮被吞掉，光标保持可见。",
        APP_NAME, MB_ICONINFORMATION | MB_OK);
}

/* ---------------- 自检（无窗口，可自动化） ---------------- */

static int RunSelfTest(void)
{
    int     x = 0, y = 0, w = 0, h = 0;
    DWORD   err = 0;
    HBITMAP bmp;
    BITMAP  bm;
    HDC     dc  = NULL;
    HGDIOBJ old = NULL;
    int     rc  = 0;

    LogF("--- selftest start ---");
    LogF("rect mode: %s", g_primary ? "primary" : "virtual");

    if (!ComputeCaptureRect(&x, &y, &w, &h)) {
        LogF("FAIL: ComputeCaptureRect returned FALSE");
        return 1;
    }
    LogF("rect: %dx%d at (%d,%d)", w, h, x, y);

    bmp = CaptureScreen(x, y, w, h, &err);
    if (!bmp) {
        LogF("FAIL: CaptureScreen err=%lu", (unsigned long)err);
        return 2;
    }
    LogF("bitmap handle: 0x%p", (void *)bmp);

    ZeroMemory(&bm, sizeof(bm));
    if (!GetObjectW(bmp, sizeof(bm), &bm)) {
        LogF("FAIL: GetObject err=%lu", (unsigned long)GetLastError());
        rc = 3;
        goto cleanup;
    }
    LogF("bitmap: %ldx%ld, %u bpp, bits=%p",
         bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, (void *)bm.bmBits);
    if (bm.bmWidth != w || bm.bmHeight != h) {
        LogF("FAIL: bitmap size mismatch (expected %dx%d)", w, h);
        rc = 4;
        goto cleanup;
    }

    dc = CreateCompatibleDC(NULL);
    if (!dc) {
        LogF("FAIL: CreateCompatibleDC err=%lu", (unsigned long)GetLastError());
        rc = 5;
        goto cleanup;
    }
    old = SelectObject(dc, bmp);
    if (!old || old == HGDI_ERROR) {
        LogF("FAIL: SelectObject err=%lu", (unsigned long)GetLastError());
        rc = 6;
        goto cleanup;
    }

    {
        COLORREF pts[5];
        COLORREF uniq[5];
        int i, j, distinct = 0;

        pts[0] = GetPixel(dc, 0, 0);
        pts[1] = GetPixel(dc, w - 1, 0);
        pts[2] = GetPixel(dc, 0, h - 1);
        pts[3] = GetPixel(dc, w - 1, h - 1);
        pts[4] = GetPixel(dc, w / 2, h / 2);

        for (i = 0; i < 5; ++i) {
            int found = 0;
            if (pts[i] == CLR_INVALID) {
                LogF("FAIL: GetPixel #%d returned CLR_INVALID", i);
                rc = 7;
                goto cleanup;
            }
            for (j = 0; j < distinct; ++j) {
                if (uniq[j] == pts[i]) {
                    found = 1;
                    break;
                }
            }
            if (!found)
                uniq[distinct++] = pts[i];
        }

        LogF("pixels: TL=0x%06lX TR=0x%06lX BL=0x%06lX BR=0x%06lX C=0x%06lX distinct=%d",
             (unsigned long)pts[0], (unsigned long)pts[1], (unsigned long)pts[2],
             (unsigned long)pts[3], (unsigned long)pts[4], distinct);
        if (distinct == 1)
            LogF("WARN: 5 个采样点颜色相同，可能桌面本身就是纯色");
    }

    LogF("PASS: capture pipeline ok");

cleanup:
    if (dc) {
        if (old && old != HGDI_ERROR)
            SelectObject(dc, old);
        DeleteDC(dc);
    }
    DeleteObject(bmp);
    LogF("--- selftest end rc=%d ---", rc);
    return rc;
}

/* ---------------- 入口 ---------------- */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND        hwnd;
    MSG         msg;
    HANDLE      mutex = NULL;
    int         x = 0, y = 0, w = 0, h = 0;
    int         rc = 0;

    (void)hPrevInstance;
    (void)nCmdShow;

    if (!ParseArgs(lpCmdLine)) {
        ShowUsage();
        return 0;
    }

    if (g_selfTest)
        return RunSelfTest();

    /* 单实例：避免重复双击叠加多个全屏窗口 */
    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"程序已在运行中。", APP_NAME, MB_ICONINFORMATION | MB_OK);
        CloseHandle(mutex);
        return 2;
    }

    EnableDpiAwareness();

    if (!ComputeCaptureRect(&x, &y, &w, &h)) {
        ShowError(L"获取屏幕尺寸", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 3;
    }
    LogF("app start: delay=%ums timeout=%us rect=%dx%d at (%d,%d)",
         g_delayMs, g_timeoutS, w, h, x, y);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;            /* 无背景刷，配合 WM_ERASEBKGND 免闪 */

    if (!RegisterClassExW(&wc)) {
        ShowError(L"RegisterClassExW", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 4;
    }

    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,   /* TOOLWINDOW：不出现在 Alt+Tab */
        CLASS_NAME, APP_NAME, WS_POPUP,
        x, y, w, h,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        ShowError(L"CreateWindowExW", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 5;
    }

    if (g_affinity)
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    /* 注意：此时窗口仍不可见，延时截图后再显示 */

    if (!SetTimer(hwnd, TIMER_ID_CAPTURE, g_delayMs, NULL)) {
        ShowError(L"SetTimer(截图)", GetLastError());
        DestroyWindow(hwnd);
        if (mutex)
            CloseHandle(mutex);
        return 6;
    }

    if (g_timeoutS)
        LogF("auto-quit armed: %us after capture", g_timeoutS);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT)
        rc = (int)msg.wParam;

    if (mutex)
        CloseHandle(mutex);
    return rc;
}
