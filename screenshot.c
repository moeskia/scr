#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define APP_NAME         L"Screenshot Viewer"
#define CLASS_NAME       L"FullScreenShot"
#define MUTEX_NAME       L"Local\\FullScreenShot.SingleInstance"
#define CAPTURE_DELAY_MS 5000
#define FOCUS_RETRY_MS   250
#define FOCUS_MAX_TRIES  3
#define TIMER_ID_CAPTURE 1
#define TIMER_ID_FOCUS   2

static HBITMAP g_hBitmap = NULL;
static UINT g_focusTries = 0;

static void EnableDpiAwareness(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");

    if (u32) {
        typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
        union { FARPROC raw; PFN_SPDAC fn; } u;
        u.raw = GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (u.fn && u.fn((HANDLE)-4))
            return;
    }
    SetProcessDPIAware();
}

static BOOL ComputeCaptureRect(int *px, int *py, int *pw, int *ph)
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (w <= 0 || h <= 0) {
        x = 0;
        y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    if (w <= 0 || h <= 0)
        return FALSE;

    *px = x;
    *py = y;
    *pw = w;
    *ph = h;
    return TRUE;
}

static HBITMAP CaptureScreen(int x, int y, int w, int h, DWORD *pErr)
{
    HDC hScreenDC = NULL;
    HDC hMemDC = NULL;
    HBITMAP hBmp = NULL;
    HGDIOBJ hOld = NULL;

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
        SelectObject(hMemDC, hOld);
        DeleteObject(hBmp);
        hBmp = NULL;
        goto done;
    }

    SelectObject(hMemDC, hOld);

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

static void ShowError(const WCHAR *what, DWORD code)
{
    WCHAR buf[320];

    wsprintfW(buf, L"%s 失败（错误码 %lu）。", what, (unsigned long)code);
    MessageBoxW(NULL, buf, APP_NAME, MB_ICONERROR | MB_OK);
}

static void ForceForeground(HWND hwnd)
{
    HWND fg;
    DWORD fgThread;
    DWORD thisThread = GetCurrentThreadId();

    if (GetForegroundWindow() == hwnd) {
        SetFocus(hwnd);
        return;
    }

    fg = GetForegroundWindow();
    fgThread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;

    if (fgThread && fgThread != thisThread && AttachThreadInput(fgThread, thisThread, TRUE)) {
        BringWindowToTop(hwnd);
        SetActiveWindow(hwnd);
        SetForegroundWindow(hwnd);
        AttachThreadInput(fgThread, thisThread, FALSE);
    } else {
        BringWindowToTop(hwnd);
        SetActiveWindow(hwnd);
        SetForegroundWindow(hwnd);
    }

    if (GetForegroundWindow() != hwnd) {
        INPUT in[2];

        ZeroMemory(in, sizeof(in));
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_MENU;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(hwnd);
    }

    SetFocus(hwnd);
}

static void DoCapture(HWND hwnd)
{
    int x = 0, y = 0, w = 0, h = 0;
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

    g_focusTries = 0;
    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    ForceForeground(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
    SetTimer(hwnd, TIMER_ID_FOCUS, FOCUS_RETRY_MS, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_ID_CAPTURE) {
            KillTimer(hwnd, TIMER_ID_CAPTURE);
            DoCapture(hwnd);
        } else if (wParam == TIMER_ID_FOCUS) {
            KillTimer(hwnd, TIMER_ID_FOCUS);
            ForceForeground(hwnd);
            if (g_focusTries < FOCUS_MAX_TRIES && GetForegroundWindow() != hwnd) {
                ++g_focusTries;
                SetTimer(hwnd, TIMER_ID_FOCUS, FOCUS_RETRY_MS << g_focusTries, NULL);
            }
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            SetFocus(hwnd);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_hBitmap) {
            HDC mem = CreateCompatibleDC(hdc);
            if (mem) {
                HGDIOBJ old = SelectObject(mem, g_hBitmap);
                if (old && old != HGDI_ERROR) {
                    RECT rc = ps.rcPaint;
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
        return 0;

    case WM_SYSKEYDOWN:
        return 0;

    case WM_SYSCOMMAND: {
        UINT cmd = (UINT)(wParam & 0xFFF0);

        if (cmd == SC_CLOSE || cmd == SC_MINIMIZE || cmd == SC_RESTORE ||
            cmd == SC_MOVE  || cmd == SC_SIZE     || cmd == SC_KEYMENU)
            return 0;
        break;
    }

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
        return 0;

    case WM_DESTROY:
        ReleaseBitmap();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    HANDLE mutex = NULL;
    int x = 0, y = 0, w = 0, h = 0;
    int rc = 0;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

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

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    if (!RegisterClassExW(&wc)) {
        ShowError(L"RegisterClassExW", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 4;
    }

    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CLASS_NAME, APP_NAME,
                           WS_POPUP, x, y, w, h, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        ShowError(L"CreateWindowExW", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 5;
    }

    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    if (!SetTimer(hwnd, TIMER_ID_CAPTURE, CAPTURE_DELAY_MS, NULL)) {
        ShowError(L"SetTimer", GetLastError());
        DestroyWindow(hwnd);
        if (mutex)
            CloseHandle(mutex);
        return 6;
    }

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
