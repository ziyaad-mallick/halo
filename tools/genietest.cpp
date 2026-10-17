// A plain Win32 window for testing the genie mod: it uses DefWindowProcW (charmap is a
// dialog, whose system commands never reach the exported DefWindowProcW), and paints a
// gradient with a frame so the warp is easy to see. Build: see tools/genie-test.ps1.
#include <windows.h>

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        for (int y = 0; y < r.bottom; y++) {
            HBRUSH b = CreateSolidBrush(RGB(30 + y * 200 / r.bottom, 90, 220 - y * 150 / r.bottom));
            RECT line = {0, y, r.right, y + 1};
            FillRect(dc, &line, b);
            DeleteObject(b);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, L"HALO GENIE TEST", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"GenieTest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"GenieTest", L"Genie Test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 150, 640, 420, nullptr,
                           nullptr, inst, nullptr);
    MSG msg;
    while (h && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
