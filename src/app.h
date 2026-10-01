#pragma once

#include <windows.h>

#define WM_APP_THUMBS_CHANGED (WM_APP + 1)

inline UINT GetDpiForWindowSafe(HWND hwnd)
{
    typedef UINT(WINAPI * Fn)(HWND);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = fn ? fn(hwnd) : 96;
    return d ? d : 96;
}

inline UINT GetDpiForSystemSafe()
{
    typedef UINT(WINAPI * Fn)();
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
    UINT d = fn ? fn() : 96;
    return d ? d : 96;
}
