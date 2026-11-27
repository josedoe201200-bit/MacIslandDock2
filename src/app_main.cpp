// app_main.cpp - punto de entrada de la app independiente.
//
//  * Arranca la isla (el codigo del mod original, via IslandPort_*).
//  * Arranca el dock propio (dock.cpp).
//  * Icono en la bandeja: mostrar/ocultar dock, reiniciar isla, inicio con
//    Windows, abrir carpeta de datos, salir.
//
// Argumentos: --restore-taskbar | --no-dock | --no-island

#include <windows.h>
#include <shellapi.h>

#include <string>

#include "dock.h"
#include "wh_compat.h"

// Definidas al final de island_port.cpp (generado por tools/make_island_port.py).
bool IslandPort_Init();
void IslandPort_SettingsChanged();
void IslandPort_Uninit();

namespace {

constexpr wchar_t kTrayClass[] = L"MacIslandDockTray";
constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kCmdToggleDock = 1;
constexpr UINT kCmdRestartIsland = 2;
constexpr UINT kCmdStartup = 3;
constexpr UINT kCmdOpenFolder = 4;
constexpr UINT kCmdOpenIni = 5;
constexpr UINT kCmdQuit = 6;

bool g_islandRunning = false;
bool g_dockRunning = false;
bool g_wantIsland = true;
bool g_wantDock = true;
UINT g_taskbarCreated = 0;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MacIslandDock";

bool StartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    const bool present =
        RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) ==
        ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

void SetStartup(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring command = std::wstring(L"\"") + exe + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)command.c_str(),
                       (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = hwnd;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"Mac Island & Dock");
    Shell_NotifyIconW(NIM_ADD, &data);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = hwnd;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (g_dockRunning) {
        AppendMenuW(menu, MF_STRING, kCmdToggleDock,
                    dock::IsVisible() ? L"Ocultar Dock" : L"Mostrar Dock");
    }
    if (g_wantIsland) {
        AppendMenuW(menu, MF_STRING, kCmdRestartIsland, L"Reiniciar isla");
    }
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0),
                kCmdStartup, L"Iniciar con Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpenFolder, L"Abrir carpeta de datos");
    AppendMenuW(menu, MF_STRING, kCmdOpenIni, L"Editar settings.ini");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Salir");

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd);
    const UINT command = (UINT)TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x,
        cursor.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
        case kCmdToggleDock:
            dock::ToggleVisible();
            break;
        case kCmdRestartIsland:
            if (g_islandRunning) {
                IslandPort_Uninit();
            }
            g_islandRunning = IslandPort_Init();
            break;
        case kCmdStartup:
            SetStartup(!StartupEnabled());
            break;
        case kCmdOpenFolder:
            ShellExecuteW(nullptr, L"open", whc::DataDir().c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
            break;
        case kCmdOpenIni: {
            const std::wstring ini = whc::IniPath();
            if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
                WritePrivateProfileStringW(L"settings", L"dock.iconSize", L"48",
                                           ini.c_str());
            }
            ShellExecuteW(nullptr, L"open", L"notepad.exe",
                          (L"\"" + ini + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case kCmdQuit:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
    }
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon(hwnd);  // Explorer se reinicio
        return 0;
    }
    switch (message) {
        case kTrayMessage:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
                ShowTrayMenu(hwnd);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // DPI por monitor (la isla y el dock dibujan en pixeles reales).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (wcscmp(argv[i], L"--restore-taskbar") == 0) {
            dock::RestoreTaskbarIfLeftover();
            LocalFree(argv);
            return 0;
        }
        if (wcscmp(argv[i], L"--no-dock") == 0) g_wantDock = false;
        if (wcscmp(argv[i], L"--no-island") == 0) g_wantIsland = false;
    }
    if (argv) LocalFree(argv);

    // Una sola instancia.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"MacIslandDockStandalone_single");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Mac Island & Dock ya está en ejecución.",
                    L"Mac Island & Dock", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Si la sesion anterior se cerro de golpe, la barra de Windows vuelve a su sitio.
    dock::RestoreTaskbarIfLeftover();

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);
    HWND tray = CreateWindowExW(0, kTrayClass, L"Mac Island & Dock", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, nullptr);
    if (tray) AddTrayIcon(tray);

    if (g_wantIsland) {
        g_islandRunning = IslandPort_Init();
        if (!g_islandRunning) {
            Wh_Log(L"La isla no pudo iniciar");
        }
    }
    if (g_wantDock) {
        g_dockRunning = dock::Start();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_dockRunning) dock::Stop();
    if (g_islandRunning) IslandPort_Uninit();
    ReleaseMutex(single);
    CloseHandle(single);
    return 0;
}
