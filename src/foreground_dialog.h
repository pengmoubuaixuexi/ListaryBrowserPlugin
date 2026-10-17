#pragma once

#include <Windows.h>

// The protocol-launch process inherits the user's foreground permission, but
// the already-running host does not. Transfer it before forwarding the action.
inline void GrantForegroundToWindow(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId) AllowSetForegroundWindow(processId);
}

inline int ForegroundMessageBox(HWND owner, const wchar_t* text,
    const wchar_t* title, UINT flags) {
    // TOPMOST alone changes only z-order. SETFOREGROUND activates the dialog
    // and MessageBox puts keyboard focus on its default button (OK normally).
    return MessageBoxW(owner, text, title, flags | MB_SETFOREGROUND | MB_TOPMOST);
}
