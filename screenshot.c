#include <windows.h>

#define TIMER_ID 1

HBITMAP hBitmap = NULL;
int screenWidth = 0;
int screenHeight = 0;

/* 截取整个屏幕 */
HBITMAP CaptureScreen(void)
{
    HDC hScreenDC = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);

    screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HBITMAP hBmp = CreateCompatibleBitmap(
        hScreenDC, screenWidth, screenHeight);

    SelectObject(hMemDC, hBmp);

    BitBlt(hMemDC, 0, 0,
           screenWidth, screenHeight,
           hScreenDC, 0, 0, SRCCOPY);

    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);

    return hBmp;
}

/* 窗口过程 */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wParam == TIMER_ID)
        {
            KillTimer(hwnd, TIMER_ID);

            /* 5 秒到 → 截图 */
            hBitmap = CaptureScreen();

            /* 截图完成后才显示窗口 */
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
        break;

    case WM_PAINT:
        if (hBitmap)
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            HDC memDC = CreateCompatibleDC(hdc);
            SelectObject(memDC, hBitmap);

            BitBlt(hdc, 0, 0,
                   screenWidth, screenHeight,
                   memDC, 0, 0, SRCCOPY);

            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_HOME) {
            DestroyWindow(hwnd);
        } else {
            // 忽略其他按键
            return 0;
        }
        break;
    
    // 拦截鼠标消息
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
    //case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
        // 忽略所有鼠标输入
        return 0;

    case WM_CLOSE:
        /* 阻止通过其他方式关闭窗口，只能通过Home退出 */
        return 0;

    case WM_DESTROY:
        if (hBitmap)
            DeleteObject(hBitmap);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine,
                   int nCmdShow)
{
    /* 关闭 DPI 虚拟化 */
    SetProcessDPIAware();

    WNDCLASS wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "FullScreenShot";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    RegisterClass(&wc);

    screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,  // 添加工具窗口样式以避免出现在Alt+Tab中
        wc.lpszClassName,
        "Screenshot Viewer",
        WS_POPUP,
        0, 0,
        screenWidth, screenHeight,
        NULL, NULL, hInstance, NULL);

    // 确保窗口始终保持在最前
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    /* ❗注意：此时不显示窗口 */

    /* 5 秒后触发截图 */
    SetTimer(hwnd, TIMER_ID, 5000, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}