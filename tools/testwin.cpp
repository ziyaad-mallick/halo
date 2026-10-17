// A plain Win32 window (DefWindowProc, like most apps) with a recognisable pattern, for testing the
// genie: charmap is a dialog and never reaches the mod's DefWindowProc hook. Usage: testwin.exe [w h]
#include <windows.h>
#include <wchar.h>

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            for (int i = 0; i < 8; i++) {  // horizontal colour bands, so warping is easy to see
                RECT b = {0, r.bottom * i / 8, r.right, r.bottom * (i + 1) / 8};
                HBRUSH br = CreateSolidBrush(RGB(40 + i * 25, 90 + (i % 3) * 50, 220 - i * 22));
                FillRect(dc, &b, br);
                DeleteObject(br);
            }
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            SelectObject(dc, pen);
            for (int x = 0; x < r.right; x += 60) { MoveToEx(dc, x, 0, nullptr); LineTo(dc, x, r.bottom); }
            for (int y = 0; y < r.bottom; y += 60) { MoveToEx(dc, 0, y, nullptr); LineTo(dc, r.right, y); }
            SetBkMode(dc, TRANSPARENT);
            HFONT font = CreateFontW(48, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, 0, 0, L"Segoe UI");
            SelectObject(dc, font);
            DrawTextW(dc, L"Halo genie test window", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(font);
            DeleteObject(pen);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    int w = 900, hh = 640;
    swscanf(cmd, L"%d %d", &w, &hh);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HaloTestWin";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(wc.lpszClassName, L"Halo test window", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 300, 120, w, hh,
                           nullptr, nullptr, inst, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return h ? 0 : 1;
}
