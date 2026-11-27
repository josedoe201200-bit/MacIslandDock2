// wh_compat.h - capa de compatibilidad que reemplaza <windhawk_utils.h>.
//
// El mod "Mac Island & Dock" (caliberda, GPL-3.0) usa muy pocas funciones de
// Windhawk. Aqui se reimplementan para que el mismo codigo de la isla corra
// como una aplicacion normal de Windows:
//   * Wh_GetIntValue / Wh_SetIntValue / Wh_GetStringValue / Wh_SetStringValue /
//     Wh_DeleteValue  -> registro de Windows (HKCU\Software\MacIslandDock\Values)
//   * Wh_GetModStoragePath -> %LOCALAPPDATA%\MacIslandDock\storage
//   * Wh_GetIntSetting / StringSetting -> %LOCALAPPDATA%\MacIslandDock\settings.ini
//     (con los valores por defecto de abajo)
//   * Wh_Log -> OutputDebugString + log.txt
//   * WindhawkUtils::SetWindowSubclassFromAnyThread (solo mismo hilo)
//   * Los hooks de explorer.exe NO existen aqui (la app no se inyecta).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#define WH_MOD_ID L"mac-island-dock"
#define WH_MOD_VERSION L"3.3.0"

namespace whc {

inline std::wstring DataDir() {
    static std::wstring dir = [] {
        WCHAR base[MAX_PATH] = L"";
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                                    base))) {
            GetTempPathW(MAX_PATH, base);
        }
        std::wstring d = std::wstring(base) + L"\\MacIslandDock";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

inline std::wstring IniPath() { return DataDir() + L"\\settings.ini"; }

// Valores por defecto de los ajustes que antes daba la pagina de ajustes de
// Windhawk. (La isla guarda casi todo lo demas en sus propios valores y tiene
// su propia pagina de ajustes.)
struct SettingDefault {
    const wchar_t* name;
    int number;
    const wchar_t* text;
};

inline const SettingDefault* FindDefault(const wchar_t* name) {
    static const SettingDefault table[] = {
        {L"showIsland", 1, L""},
        {L"taskbarMode", 0, L"windows"},
        {L"trayListIcons", 12, L""},
        {L"windowsHideTray", 0, L""},
        {L"animations.enabled", 1, L""},
        {L"animations.speed", 125, L""},
        {L"animations.level", 0, L"all"},
        {L"animations.frameRate", 0, L"screen"},
        {L"hiding.autoHide", 0, L""},
        {L"hiding.hideWhen", 0, L"never"},
        {L"hiding.showDuration", 250, L""},
        {L"hiding.hideDuration", 250, L""},
        {L"hiding.showDelay", 0, L""},
        {L"hiding.hideDelay", 300, L""},
        {L"hiding.revealHeight", 8, L""},
        {L"hiding.fullscreenGuard", 1, L""},
        {L"dock.enabled", 1, L""},
        {L"dock.taskbar", 0, L"autohide"},  // autohide | hide | keep
        {L"dock.cornerRadius", 16, L""},
        {L"dock.iconSize", 48, L""},
        {L"dock.iconGap", 8, L""},
        {L"dock.bottomGap", 8, L""},
        {L"dock.background", 1, L""},
        {L"dock.backgroundOpacity", 40, L""},
        {L"dock.hideTray", 0, L""},
        {L"dock.maxWidth", 0, L""},
        {L"dock.dotIndicator", 1, L""},
        {L"dock.separator", 1, L""},
        {L"dock.attentionMode", 0, L"icon"},
        {L"icons.magnification", 150, L""},
        {L"icons.magnificationRange", 140, L""},
        {L"icons.onlyApps", 0, L""},
        {L"icons.bounceOnLaunch", 1, L""},
        {L"icons.keepOpenOnLaunch", 0, L""},
        {L"icons.keepOpenDuration", 1500, L""},
    };
    for (const auto& entry : table) {
        if (wcscmp(entry.name, name) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

inline std::wstring FormatKey(const wchar_t* format, va_list args) {
    wchar_t key[256];
    _vsnwprintf_s(key, _TRUNCATE, format, args);
    return key;
}

// HKCU\Software\MacIslandDock\Values
inline HKEY OpenValues(REGSAM access) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\MacIslandDock\\Values", 0, nullptr, 0,
                        access, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return nullptr;
    }
    return key;
}

}  // namespace whc

////////////////////////////////////////////////////////////////////////////////
// API de Windhawk (solo lo que usa el mod).

inline void Wh_Log(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t text[1024];
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW((std::wstring(L"[MacIslandDock] ") + text + L"\n").c_str());
    static bool enabled = GetPrivateProfileIntW(L"app", L"log", 0,
                                                whc::IniPath().c_str()) != 0;
    if (enabled) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, (whc::DataDir() + L"\\log.txt").c_str(), L"a,ccs=UTF-8") == 0 &&
            file) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute,
                     now.wSecond, now.wMilliseconds, text);
            fclose(file);
        }
    }
}

inline int Wh_GetIntSetting(const wchar_t* name, ...) {
    va_list args;
    va_start(args, name);
    const std::wstring key = whc::FormatKey(name, args);
    va_end(args);
    const auto* entry = whc::FindDefault(key.c_str());
    return (int)GetPrivateProfileIntW(L"settings", key.c_str(),
                                      entry ? entry->number : 0,
                                      whc::IniPath().c_str());
}

inline int Wh_GetIntValue(const wchar_t* name, int defaultValue) {
    HKEY key = whc::OpenValues(KEY_QUERY_VALUE);
    if (!key) {
        return defaultValue;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, name, nullptr, &type,
                                         (LPBYTE)&value, &size);
    RegCloseKey(key);
    return (result == ERROR_SUCCESS && type == REG_DWORD) ? (int)value
                                                          : defaultValue;
}

inline BOOL Wh_SetIntValue(const wchar_t* name, int value) {
    HKEY key = whc::OpenValues(KEY_SET_VALUE);
    if (!key) {
        return FALSE;
    }
    const DWORD data = (DWORD)value;
    const LONG result = RegSetValueExW(key, name, 0, REG_DWORD,
                                       (const BYTE*)&data, sizeof(data));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// Devuelve los caracteres escritos (sin el nulo); 0 si no existe o no cabe.
inline size_t Wh_GetStringValue(const wchar_t* name, wchar_t* buffer,
                                size_t bufferChars) {
    if (!buffer || !bufferChars) {
        return 0;
    }
    buffer[0] = L'\0';
    HKEY key = whc::OpenValues(KEY_QUERY_VALUE);
    if (!key) {
        return 0;
    }
    DWORD size = (DWORD)(bufferChars * sizeof(wchar_t));
    DWORD type = 0;
    const LONG result =
        RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)buffer, &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ) {
        buffer[0] = L'\0';
        return 0;
    }
    buffer[bufferChars - 1] = L'\0';
    return wcslen(buffer);
}

inline BOOL Wh_SetStringValue(const wchar_t* name, const wchar_t* value) {
    HKEY key = whc::OpenValues(KEY_SET_VALUE);
    if (!key) {
        return FALSE;
    }
    const LONG result =
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value,
                       (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

inline BOOL Wh_DeleteValue(const wchar_t* name) {
    HKEY key = whc::OpenValues(KEY_SET_VALUE);
    if (!key) {
        return FALSE;
    }
    const LONG result = RegDeleteValueW(key, name);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

inline size_t Wh_GetModStoragePath(wchar_t* path, size_t pathChars) {
    const std::wstring dir = whc::DataDir() + L"\\storage";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!path || dir.size() + 1 > pathChars) {
        return 0;
    }
    wcscpy_s(path, pathChars, dir.c_str());
    return dir.size();
}

namespace WindhawkUtils {

class StringSetting {
   public:
    static StringSetting make(const wchar_t* name, ...) {
        va_list args;
        va_start(args, name);
        const std::wstring key = whc::FormatKey(name, args);
        va_end(args);
        const auto* entry = whc::FindDefault(key.c_str());
        wchar_t value[2048];
        GetPrivateProfileStringW(L"settings", key.c_str(),
                                 entry && entry->text ? entry->text : L"",
                                 value, ARRAYSIZE(value),
                                 whc::IniPath().c_str());
        StringSetting setting;
        setting.m_value = value;
        return setting;
    }
    const wchar_t* get() const { return m_value.c_str(); }
    operator const wchar_t*() const { return m_value.c_str(); }

   private:
    std::wstring m_value;
};

// Solo en el hilo de la ventana (aqui nadie se inyecta en otros procesos).
inline BOOL SetWindowSubclassFromAnyThread(HWND window, SUBCLASSPROC proc,
                                           DWORD_PTR refData) {
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) {
        return FALSE;
    }
    return SetWindowSubclass(window, proc, (UINT_PTR)proc, refData);
}

inline BOOL RemoveWindowSubclassFromAnyThread(HWND window, SUBCLASSPROC proc) {
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) {
        return FALSE;
    }
    return RemoveWindowSubclass(window, proc, (UINT_PTR)proc);
}

// Sin inyeccion no hay hooks: la parte de explorer.exe nunca se ejecuta.
template <typename A, typename B, typename C>
inline BOOL SetFunctionHook(A, B, C) {
    return FALSE;
}

}  // namespace WindhawkUtils
