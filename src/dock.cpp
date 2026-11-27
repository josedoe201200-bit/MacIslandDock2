// dock.cpp - dock estilo macOS como ventana propia (GDI+ + UpdateLayeredWindow).
//
// Codigo nuevo escrito para esta app (GPL-3.0, igual que el mod original). Imita
// el dock del mod "Mac Island & Dock": fondo redondeado translucido, iconos con
// magnificacion, punto indicador de apps abiertas, separador, rebote al abrir,
// autoocultar, y la barra de Windows en autoocultar/oculta mientras corre.
//
// Diferencia con el mod: el mod edita el XAML de la barra dentro de explorer.exe;
// una app normal no puede hacerlo, asi que este dock es una ventana aparte.

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "dock.h"
#include "wh_compat.h"

namespace dock {
namespace {

constexpr wchar_t kClassName[] = L"MacIslandDockStandalone";
constexpr UINT_PTR kTimerFrame = 1;
constexpr UINT_PTR kTimerRefresh = 2;
constexpr UINT kMsgToggle = WM_APP + 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuPin = 2;
constexpr UINT kMenuClose = 3;
constexpr UINT kMenuAutoHide = 4;
constexpr UINT kMenuQuit = 5;
constexpr double kPi = 3.14159265358979323846;

struct Rc {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct Config {
    double scale = 1.0;  // DPI
    int iconSize = 48;
    int gap = 8;
    int bottomGap = 8;
    int radius = 16;
    bool background = true;
    double opacity = 0.40;
    double magnification = 1.5;
    int magRange = 140;
    bool dots = true;
    bool separator = true;
    bool bounce = true;
    bool autoHide = false;
    int showDelay = 0;
    int hideDelay = 300;
    int reveal = 8;
    bool fullscreenGuard = true;
    std::wstring taskbarMode = L"autohide";
};

struct PinnedDef {
    std::wstring path;  // lo que se lanza
    std::wstring exe;   // exe resuelto en minusculas ("" si no se conoce)
    std::wstring name;
};

struct Item {
    std::wstring id;    // "p:<ruta>" (fijado) o "r:<clave>" (solo abierto)
    std::wstring path;  // lo que se lanza ("" si no se puede lanzar)
    std::wstring exe;
    std::wstring name;
    bool pinned = false;
    std::vector<HWND> windows;  // en orden Z (la primera esta arriba)
    std::shared_ptr<Gdiplus::Bitmap> icon;
    double scale = 1.0;
    DWORD bounceStart = 0;
    Rc rect;  // icono, en coordenadas de la ventana
    Rc slot;  // zona de clic
};

struct OpenWindow {
    HWND hwnd;
    std::wstring exe;
    std::wstring title;
};

struct State {
    Config cfg;
    HWND hwnd = nullptr;
    HANDLE thread = nullptr;
    HANDLE ready = nullptr;
    std::vector<PinnedDef> pinned;
    std::vector<Item> items;
    int hovered = -1;
    bool userHidden = false;
    bool guardHidden = false;
    double slide = 0.0;  // 0 visible .. 1 oculto
    DWORD leaveSince = 0;
    DWORD revealSince = 0;
    DWORD lastTick = 0;
    DWORD lastTopmost = 0;
    DWORD lastTaskbarCheck = 0;
    bool needRender = true;
    HDC memDc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int bufW = 0, bufH = 0;
    int winW = 0, winH = 0, winX = 0, winY = 0;
    RECT monitor{};
    Rc panel;
    bool tookAutoHide = false;
    bool hidTaskbar = false;
    ULONG_PTR gdiplusToken = 0;
    std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> iconCache;
} g;

////////////////////////////////////////////////////////////////////////////////
// Utilidades

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::wstring Stem(const std::wstring& path) {
    std::wstring name = path;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";
    struct Translation {
        WORD lang, codePage;
    }* translation = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        (LPVOID*)&translation, &length) ||
        length < sizeof(Translation)) {
        return L"";
    }
    wchar_t sub[96];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               translation->lang, translation->codePage);
    wchar_t* text = nullptr;
    if (VerQueryValueW(data.data(), sub, (LPVOID*)&text, &length) && text &&
        *text) {
        return text;
    }
    return L"";
}

std::wstring ResolveLink(const std::wstring& link) {
    std::wstring result;
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&shellLink)))) {
        return result;
    }
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&file)))) {
        if (SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
            wchar_t raw[MAX_PATH] = L"";
            if (SUCCEEDED(shellLink->GetPath(raw, MAX_PATH, nullptr,
                                             SLGP_RAWPATH)) &&
                raw[0]) {
                wchar_t expanded[MAX_PATH] = L"";
                ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
                result = expanded;
            }
        }
        file->Release();
    }
    shellLink->Release();
    return result;
}

std::wstring ExeOfProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";
    wchar_t path[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(path);
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) result = path;
    CloseHandle(process);
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Iconos

std::shared_ptr<Gdiplus::Bitmap> FromHicon(HICON icon) {
    if (!icon) return nullptr;
    std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromHICON(icon));
    if (!source || source->GetLastStatus() != Gdiplus::Ok) return nullptr;
    const int w = (int)source->GetWidth();
    const int h = (int)source->GetHeight();
    if (w <= 0 || h <= 0) return nullptr;
    auto result = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics canvas(result.get());
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    canvas.DrawImage(source.get(), 0, 0, w, h);
    return result;
}

std::shared_ptr<Gdiplus::Bitmap> IconOfFile(const std::wstring& path) {
    const auto cached = g.iconCache.find(path);
    if (cached != g.iconCache.end()) return cached->second;

    std::shared_ptr<Gdiplus::Bitmap> result;
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_LARGEICON) &&
        info.hIcon) {
        result = FromHicon(info.hIcon);
        DestroyIcon(info.hIcon);
    }

    if (result) g.iconCache[path] = result;
    return result;
}

std::shared_ptr<Gdiplus::Bitmap> IconOfWindow(HWND window) {
    DWORD_PTR icon = 0;
    if (!SendMessageTimeoutW(window, WM_GETICON, ICON_BIG, 0,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &icon) ||
        !icon) {
        icon = GetClassLongPtrW(window, GCLP_HICON);
    }
    return FromHicon((HICON)icon);
}

////////////////////////////////////////////////////////////////////////////////
// Configuracion y apps fijadas

void LoadConfig() {
    Config c;
    HDC screen = GetDC(nullptr);
    c.scale = std::max(1.0, GetDeviceCaps(screen, LOGPIXELSX) / 96.0);
    ReleaseDC(nullptr, screen);
    c.iconSize = std::clamp(Wh_GetIntSetting(L"dock.iconSize"), 24, 64);
    c.gap = std::clamp(Wh_GetIntSetting(L"dock.iconGap"), 0, 32);
    c.bottomGap = std::clamp(Wh_GetIntSetting(L"dock.bottomGap"), 0, 24);
    c.radius = std::clamp(Wh_GetIntSetting(L"dock.cornerRadius"), 0, 32);
    c.background = Wh_GetIntSetting(L"dock.background") != 0;
    c.opacity = std::clamp(Wh_GetIntSetting(L"dock.backgroundOpacity"), 0, 100) / 100.0;
    c.magnification = std::clamp(Wh_GetIntSetting(L"icons.magnification"), 100, 200) / 100.0;
    c.magRange = std::clamp(Wh_GetIntSetting(L"icons.magnificationRange"), 40, 300);
    c.dots = Wh_GetIntSetting(L"dock.dotIndicator") != 0;
    c.separator = Wh_GetIntSetting(L"dock.separator") != 0;
    c.bounce = Wh_GetIntSetting(L"icons.bounceOnLaunch") != 0;
    c.autoHide = Wh_GetIntSetting(L"hiding.autoHide") != 0;
    c.showDelay = std::clamp(Wh_GetIntSetting(L"hiding.showDelay"), 0, 2000);
    c.hideDelay = std::clamp(Wh_GetIntSetting(L"hiding.hideDelay"), 0, 5000);
    c.reveal = std::clamp(Wh_GetIntSetting(L"hiding.revealHeight"), 1, 120);
    c.fullscreenGuard = Wh_GetIntSetting(L"hiding.fullscreenGuard") != 0;
    c.taskbarMode = WindhawkUtils::StringSetting::make(L"dock.taskbar").get();
    g.cfg = c;
}

void SavePinned() {
    std::wstring text = L"v1";
    for (const auto& def : g.pinned) {
        text += L'\n';
        text += def.path;
    }
    Wh_SetStringValue(L"dockPinned", text.c_str());
}

PinnedDef MakePinned(const std::wstring& path) {
    PinnedDef def;
    def.path = path;
    if (EndsWithNoCase(path, L".lnk")) {
        def.exe = Lower(ResolveLink(path));
        def.name = Stem(path);
    } else {
        def.exe = Lower(path);
        def.name = FileDescription(path);
        if (def.name.empty()) def.name = Stem(path);
    }
    return def;
}

void LoadPinned() {
    g.pinned.clear();
    std::vector<wchar_t> buffer(32768);
    if (Wh_GetStringValue(L"dockPinned", buffer.data(), buffer.size()) &&
        wcsncmp(buffer.data(), L"v1", 2) == 0) {
        std::wstring all = buffer.data();
        size_t pos = all.find(L'\n');
        while (pos != std::wstring::npos) {
            const size_t next = all.find(L'\n', pos + 1);
            const std::wstring line = all.substr(
                pos + 1, next == std::wstring::npos ? next : next - pos - 1);
            if (!line.empty()) g.pinned.push_back(MakePinned(line));
            pos = next;
        }
        return;
    }
    // Primera vez: las apps fijadas en la barra de tareas de Windows.
    wchar_t appData[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        const std::wstring folder =
            std::wstring(appData) +
            L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
        WIN32_FIND_DATAW find;
        HANDLE handle = FindFirstFileW((folder + L"\\*.lnk").c_str(), &find);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                g.pinned.push_back(MakePinned(folder + L"\\" + find.cFileName));
            } while (FindNextFileW(handle, &find));
            FindClose(handle);
        }
    }
    if (g.pinned.empty()) {
        wchar_t windows[MAX_PATH] = L"";
        GetWindowsDirectoryW(windows, MAX_PATH);
        g.pinned.push_back(MakePinned(std::wstring(windows) + L"\\explorer.exe"));
    }
    SavePinned();
}

////////////////////////////////////////////////////////////////////////////////
// Ventanas abiertas -> items

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM param) {
    auto* out = (std::vector<OpenWindow>*)param;
    if (!IsWindowVisible(hwnd)) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_NOACTIVATE) ||
        ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW))) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
        return TRUE;
    }
    wchar_t title[256] = L"";
    if (!GetWindowTextW(hwnd, title, ARRAYSIZE(title)) || !title[0]) return TRUE;
    wchar_t className[64] = L"";
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    if (!wcscmp(className, L"Progman") || !wcscmp(className, L"WorkerW") ||
        !wcscmp(className, L"Shell_TrayWnd") ||
        !wcscmp(className, L"Shell_SecondaryTrayWnd") ||
        !wcscmp(className, kClassName)) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;  // incluye la isla
    OpenWindow window;
    window.hwnd = hwnd;
    window.exe = Lower(ExeOfProcess(pid));
    window.title = title;
    out->push_back(std::move(window));
    return TRUE;
}

// Reconstruye la lista. Devuelve true si cambio algo visible.
bool Refresh() {
    std::vector<OpenWindow> open;
    EnumWindows(EnumWindowsProc, (LPARAM)&open);

    auto previous = [&](const std::wstring& id) -> const Item* {
        for (const auto& item : g.items)
            if (item.id == id) return &item;
        return nullptr;
    };

    std::vector<Item> next;
    std::vector<bool> used(open.size(), false);

    for (const auto& def : g.pinned) {
        Item item;
        item.id = L"p:" + def.path;
        item.path = def.path;
        item.exe = def.exe;
        item.name = def.name;
        item.pinned = true;
        for (size_t i = 0; i < open.size(); i++) {
            if (!def.exe.empty() && open[i].exe == def.exe) {
                item.windows.push_back(open[i].hwnd);
                used[i] = true;
            }
        }
        if (const Item* old = previous(item.id)) {
            item.scale = old->scale;
            item.bounceStart = old->bounceStart;
            item.icon = old->icon;
        }
        if (!item.icon) item.icon = IconOfFile(def.path);
        next.push_back(std::move(item));
    }

    const size_t pinnedCount = next.size();
    for (size_t i = 0; i < open.size(); i++) {
        if (used[i]) continue;
        const bool uwpHost =
            open[i].exe.empty() ||
            open[i].exe.find(L"applicationframehost.exe") != std::wstring::npos;
        const std::wstring key = uwpHost ? L"w:" + open[i].title : open[i].exe;
        Item* target = nullptr;
        for (size_t k = pinnedCount; k < next.size(); k++)
            if (next[k].id == L"r:" + key) target = &next[k];
        if (!target) {
            Item item;
            item.id = L"r:" + key;
            item.exe = uwpHost ? L"" : open[i].exe;
            item.path = uwpHost ? L"" : open[i].exe;
            if (uwpHost) {
                item.name = open[i].title;
            } else {
                item.name = FileDescription(open[i].exe);
                if (item.name.empty()) item.name = Stem(open[i].exe);
            }
            if (const Item* old = previous(item.id)) {
                item.scale = old->scale;
                item.bounceStart = old->bounceStart;
                item.icon = old->icon;
            }
            if (!item.icon)
                item.icon = uwpHost ? IconOfWindow(open[i].hwnd)
                                    : IconOfFile(open[i].exe);
            if (!item.icon) item.icon = IconOfWindow(open[i].hwnd);
            next.push_back(std::move(item));
            target = &next.back();
        }
        target->windows.push_back(open[i].hwnd);
    }

    bool changed = next.size() != g.items.size();
    if (!changed) {
        for (size_t i = 0; i < next.size(); i++) {
            if (next[i].id != g.items[i].id ||
                next[i].windows.empty() != g.items[i].windows.empty() ||
                next[i].icon != g.items[i].icon) {
                changed = true;
                break;
            }
        }
    }
    g.items = std::move(next);
    if (changed) g.hovered = -1;
    return changed;
}

////////////////////////////////////////////////////////////////////////////////
// Barra de tareas de Windows

bool GetTaskbarAutoHide() {
    APPBARDATA data{sizeof(data)};
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!data.hWnd) return false;
    return (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0;
}

void SetTaskbarAutoHide(bool on) {
    APPBARDATA data{sizeof(data)};
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!data.hWnd) return;
    const UINT state = (UINT)SHAppBarMessage(ABM_GETSTATE, &data);
    data.lParam = on ? (state | ABS_AUTOHIDE) : (state & ~ABS_AUTOHIDE);
    SHAppBarMessage(ABM_SETSTATE, &data);
}

BOOL CALLBACK HideTaskbarsProc(HWND hwnd, LPARAM show) {
    wchar_t className[64] = L"";
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    if (!wcscmp(className, L"Shell_TrayWnd") ||
        !wcscmp(className, L"Shell_SecondaryTrayWnd")) {
        ShowWindow(hwnd, show ? SW_SHOWNA : SW_HIDE);
    }
    return TRUE;
}

void ApplyTaskbarMode() {
    if (_wcsicmp(g.cfg.taskbarMode.c_str(), L"autohide") == 0) {
        if (!GetTaskbarAutoHide()) {
            SetTaskbarAutoHide(true);
            g.tookAutoHide = true;
            Wh_SetIntValue(L"dockTookAutoHide", 1);
        }
    } else if (_wcsicmp(g.cfg.taskbarMode.c_str(), L"hide") == 0) {
        EnumWindows(HideTaskbarsProc, 0);
        g.hidTaskbar = true;
        Wh_SetIntValue(L"dockHidTaskbar", 1);
    }
}

void RestoreTaskbar() {
    if (g.tookAutoHide) {
        SetTaskbarAutoHide(false);
        g.tookAutoHide = false;
        Wh_DeleteValue(L"dockTookAutoHide");
    }
    if (g.hidTaskbar) {
        EnumWindows(HideTaskbarsProc, 1);
        g.hidTaskbar = false;
        Wh_DeleteValue(L"dockHidTaskbar");
    }
}

////////////////////////////////////////////////////////////////////////////////
// Geometria

void UpdateMonitor() {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    g.monitor = info.rcMonitor;
}

float Px(double v) { return (float)(v * g.cfg.scale); }

// Tamanos fijos de la ventana (el panel crece dentro de ella con la magnificacion).
void ComputeWindowSize() {
    const float s = Px(g.cfg.iconSize);
    const float gap = Px(g.cfg.gap);
    const float pad = Px(10);
    const size_t n = g.items.size();
    const float widest =
        n * (s * (float)g.cfg.magnification + gap) + Px(24) + 2 * pad + 2 * s;
    const int monitorWidth = g.monitor.right - g.monitor.left;
    g.winW = (int)std::min(widest, (float)monitorWidth);
    const float panelH = Px(10) + s + Px(8) + Px(6);
    g.winH = (int)(Px(g.cfg.bottomGap) + panelH +
                   s * (float)(g.cfg.magnification - 1.0) + s * 0.55f + Px(34));
    g.winX = g.monitor.left + (monitorWidth - g.winW) / 2;
}

bool SeparatorBefore(size_t index) {
    if (!g.cfg.separator || index == 0 || index >= g.items.size()) return false;
    return g.items[index - 1].pinned && !g.items[index].pinned;
}

// Calcula rectangulos de cada item con las escalas actuales.
void Layout() {
    const float s = Px(g.cfg.iconSize);
    const float gap = Px(g.cfg.gap);
    const float padX = Px(10);
    const float padBottom = Px(6);
    const float dotArea = Px(8);
    const float padTop = Px(10);
    const float sepW = Px(14);

    float total = 2 * padX;
    for (size_t i = 0; i < g.items.size(); i++) {
        if (SeparatorBefore(i)) total += sepW;
        total += s * (float)g.items[i].scale + gap;
    }
    total -= gap;

    const float panelH = padTop + s + dotArea + padBottom;
    g.panel.w = total;
    g.panel.h = panelH;
    g.panel.x = ((float)g.winW - total) / 2;
    g.panel.y = (float)g.winH - Px(g.cfg.bottomGap) - panelH;

    const float iconBottom = g.panel.y + g.panel.h - padBottom - dotArea;
    const DWORD now = GetTickCount();
    float x = g.panel.x + padX;
    for (size_t i = 0; i < g.items.size(); i++) {
        Item& item = g.items[i];
        if (SeparatorBefore(i)) x += sepW;
        const float size = s * (float)item.scale;
        float lift = 0;
        if (item.bounceStart) {
            const double t = (now - item.bounceStart) / 1000.0;
            const bool launching = item.windows.empty() && t < 8.0;
            if (t < 0.9 || launching) {
                lift = (float)(std::fabs(std::sin(t * kPi / 0.42)) * s * 0.5 *
                               (launching ? 0.8 : std::exp(-t * 1.2)));
            } else {
                item.bounceStart = 0;
            }
        }
        item.rect = {x, iconBottom - size - lift, size, size};
        item.slot = {x - gap / 2, 0, size + gap, (float)g.winH};
        x += size + gap;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Dibujo

void EnsureBuffer() {
    if (g.dib && g.bufW == g.winW && g.bufH == g.winH) return;
    if (g.dib) DeleteObject(g.dib);
    if (!g.memDc) {
        HDC screen = GetDC(nullptr);
        g.memDc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = g.winW;
    info.bmiHeader.biHeight = -g.winH;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    g.dib = CreateDIBSection(g.memDc, &info, DIB_RGB_COLORS, &g.bits, nullptr, 0);
    SelectObject(g.memDc, g.dib);
    g.bufW = g.winW;
    g.bufH = g.winH;
}

void RoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h,
                 float r) {
    r = std::min(r, std::min(w, h) / 2);
    if (r <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    const float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void Render() {
    if (!g.hwnd || g.winW <= 0 || g.winH <= 0) return;
    EnsureBuffer();
    std::memset(g.bits, 0, (size_t)g.bufW * g.bufH * 4);
    Layout();
    {
        Gdiplus::Bitmap canvas(g.bufW, g.bufH, g.bufW * 4, PixelFormat32bppPARGB,
                               (BYTE*)g.bits);
        Gdiplus::Graphics gfx(&canvas);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        if (g.cfg.background) {
            Gdiplus::GraphicsPath path;
            RoundedRect(path, g.panel.x, g.panel.y, g.panel.w, g.panel.h,
                        Px(g.cfg.radius));
            Gdiplus::SolidBrush fill(Gdiplus::Color(
                (BYTE)(std::clamp(g.cfg.opacity, 0.0, 1.0) * 255), 28, 28, 32));
            gfx.FillPath(&fill, &path);
            Gdiplus::Pen border(Gdiplus::Color(70, 255, 255, 255), 1.0f);
            gfx.DrawPath(&border, &path);
        }

        for (size_t i = 0; i < g.items.size(); i++) {
            const Item& item = g.items[i];
            if (SeparatorBefore(i)) {
                Gdiplus::Pen line(Gdiplus::Color(90, 255, 255, 255), 1.0f);
                const float sx = item.rect.x - Px(g.cfg.gap) / 2 - Px(7);
                gfx.DrawLine(&line, sx, g.panel.y + Px(12), sx,
                             g.panel.y + g.panel.h - Px(12));
            }
            if (item.icon) {
                gfx.DrawImage(item.icon.get(),
                              Gdiplus::RectF(item.rect.x, item.rect.y,
                                             item.rect.w, item.rect.h));
            } else {
                Gdiplus::SolidBrush placeholder(Gdiplus::Color(120, 200, 200, 210));
                gfx.FillEllipse(&placeholder, item.rect.x, item.rect.y,
                                item.rect.w, item.rect.h);
            }
            if (g.cfg.dots && !item.windows.empty()) {
                Gdiplus::SolidBrush dot(Gdiplus::Color(220, 235, 235, 240));
                const float r = Px(2.2);
                const float cx = item.rect.x + item.rect.w / 2;
                const float cy = g.panel.y + g.panel.h - Px(6) - Px(3);
                gfx.FillEllipse(&dot, cx - r, cy - r, r * 2, r * 2);
            }
        }

        if (g.hovered >= 0 && g.hovered < (int)g.items.size() && g.slide < 0.5) {
            const Item& item = g.items[g.hovered];
            if (!item.name.empty()) {
                Gdiplus::FontFamily family(L"Segoe UI");
                Gdiplus::Font font(&family, Px(12), Gdiplus::FontStyleRegular,
                                   Gdiplus::UnitPixel);
                Gdiplus::RectF bounds;
                Gdiplus::PointF origin(0, 0);
                gfx.MeasureString(item.name.c_str(), -1, &font, origin, &bounds);
                const float w = bounds.Width + Px(18);
                const float h = bounds.Height + Px(6);
                float x = item.rect.x + item.rect.w / 2 - w / 2;
                x = std::clamp(x, 2.0f, std::max(2.0f, (float)g.winW - w - 2));
                const float y = std::max(2.0f, item.rect.y - h - Px(8));
                Gdiplus::GraphicsPath tip;
                RoundedRect(tip, x, y, w, h, Px(8));
                Gdiplus::SolidBrush tipFill(Gdiplus::Color(215, 30, 30, 34));
                gfx.FillPath(&tipFill, &tip);
                Gdiplus::Pen tipBorder(Gdiplus::Color(60, 255, 255, 255), 1.0f);
                gfx.DrawPath(&tipBorder, &tip);
                Gdiplus::SolidBrush text(Gdiplus::Color(255, 245, 245, 247));
                gfx.DrawString(item.name.c_str(), -1, &font,
                               Gdiplus::PointF(x + Px(9), y + Px(3)), &text);
            }
        }
        gfx.Flush();
    }

    const int y = g.monitor.bottom - g.winH + (int)std::lround(g.slide * g.winH);
    g.winY = y;
    POINT dst{g.winX, y};
    SIZE size{g.winW, g.winH};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g.hwnd, screen, &dst, &size, g.memDc, &src, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    g.needRender = false;
}

////////////////////////////////////////////////////////////////////////////////
// Animacion y entrada

bool IsFullscreenForeground() {
    HWND fore = GetForegroundWindow();
    if (!fore || fore == g.hwnd) return false;
    wchar_t className[64] = L"";
    GetClassNameW(fore, className, ARRAYSIZE(className));
    if (!wcscmp(className, L"Progman") || !wcscmp(className, L"WorkerW") ||
        !wcscmp(className, L"Shell_TrayWnd")) {
        return false;
    }
    RECT r;
    if (!GetWindowRect(fore, &r)) return false;
    return r.left <= g.monitor.left && r.top <= g.monitor.top &&
           r.right >= g.monitor.right && r.bottom >= g.monitor.bottom &&
           !(GetWindowLongPtrW(fore, GWL_STYLE) & WS_CAPTION);
}

void ApplyVisibility() {
    const bool hide = g.userHidden || g.guardHidden;
    const bool isShown = IsWindowVisible(g.hwnd) != FALSE;
    if (hide && isShown) ShowWindow(g.hwnd, SW_HIDE);
    if (!hide && !isShown) {
        ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
        g.needRender = true;
    }
}

void Tick() {
    const DWORD now = GetTickCount();
    const double dt = std::clamp((now - g.lastTick) / 1000.0, 0.001, 0.1);
    g.lastTick = now;

    if (g.cfg.fullscreenGuard) {
        const bool fullscreen = IsFullscreenForeground();
        if (fullscreen != g.guardHidden) {
            g.guardHidden = fullscreen;
            ApplyVisibility();
        }
    }
    if (!IsWindowVisible(g.hwnd)) return;

    if (now - g.lastTopmost > 1000) {
        SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g.lastTopmost = now;
    }
    if (_wcsicmp(g.cfg.taskbarMode.c_str(), L"hide") == 0 &&
        now - g.lastTaskbarCheck > 1000) {
        g.lastTaskbarCheck = now;
        EnumWindows(HideTaskbarsProc, 0);
    }

    POINT cursor;
    GetCursorPos(&cursor);
    const float mx = (float)(cursor.x - g.winX);
    const float my = (float)(cursor.y - g.winY);
    const bool onMonitor = cursor.x >= g.monitor.left && cursor.x < g.monitor.right;

    // Zona del dock (para mantenerlo abierto y para la magnificacion).
    const float extra = Px(g.cfg.iconSize) * (float)(g.cfg.magnification - 1.0) + Px(20);
    const bool inDock = onMonitor && g.slide < 0.95 &&
                        mx >= g.panel.x - Px(12) &&
                        mx <= g.panel.x + g.panel.w + Px(12) &&
                        my >= g.panel.y - extra &&
                        cursor.y < g.monitor.bottom;

    // Autoocultar
    double slideTarget = 0;
    if (g.cfg.autoHide) {
        const bool atEdge =
            onMonitor && cursor.y >= g.monitor.bottom - (int)Px(g.cfg.reveal);
        if (atEdge || inDock) {
            g.leaveSince = 0;
            if (!g.revealSince) g.revealSince = now;
            slideTarget = (now - g.revealSince >= (DWORD)g.cfg.showDelay) ? 0 : g.slide;
        } else {
            g.revealSince = 0;
            if (!g.leaveSince) g.leaveSince = now;
            slideTarget = (now - g.leaveSince >= (DWORD)g.cfg.hideDelay) ? 1 : g.slide;
            if (g.slide <= 0.0 && slideTarget == g.slide) slideTarget = 0;
        }
    }
    if (std::fabs(g.slide - slideTarget) > 0.0005) {
        const double step = dt / 0.25;
        g.slide = slideTarget > g.slide ? std::min(slideTarget, g.slide + step)
                                        : std::max(slideTarget, g.slide - step);
        g.needRender = true;
    }

    // Magnificacion
    const float s = Px(g.cfg.iconSize);
    const float gap = Px(g.cfg.gap);
    const float range = Px(g.cfg.magRange);
    float baseTotal = 2 * Px(10) - gap;
    for (size_t i = 0; i < g.items.size(); i++) {
        if (SeparatorBefore(i)) baseTotal += Px(14);
        baseTotal += s + gap;
    }
    float cx = ((float)g.winW - baseTotal) / 2 + Px(10);
    const double blend = 1.0 - std::exp(-dt * 20.0);
    for (size_t i = 0; i < g.items.size(); i++) {
        Item& item = g.items[i];
        if (SeparatorBefore(i)) cx += Px(14);
        const float center = cx + s / 2;
        cx += s + gap;
        double target = 1.0;
        if (inDock) {
            const double t = std::fabs(mx - center) / range;
            if (t < 1.0)
                target = 1.0 + (g.cfg.magnification - 1.0) * 0.5 *
                                   (1.0 + std::cos(t * kPi));
        }
        if (std::fabs(item.scale - target) > 0.0008) {
            item.scale += (target - item.scale) * blend;
            g.needRender = true;
        } else if (item.scale != target) {
            item.scale = target;
            g.needRender = true;
        }
        if (item.bounceStart) g.needRender = true;
    }

    int hovered = -1;
    if (inDock) {
        for (size_t i = 0; i < g.items.size(); i++) {
            if (mx >= g.items[i].slot.x &&
                mx < g.items[i].slot.x + g.items[i].slot.w) {
                hovered = (int)i;
                break;
            }
        }
    }
    if (hovered != g.hovered) {
        g.hovered = hovered;
        g.needRender = true;
    }

    if (g.needRender) Render();
}

int HitTest(int x, int y) {
    for (size_t i = 0; i < g.items.size(); i++) {
        const Item& item = g.items[i];
        const float top = item.rect.y;
        if ((float)x >= item.slot.x && (float)x < item.slot.x + item.slot.w &&
            (float)y >= top - Px(4) && (float)y <= g.panel.y + g.panel.h) {
            return (int)i;
        }
    }
    return -1;
}

void LaunchItem(Item& item) {
    if (item.path.empty()) return;
    wchar_t dir[MAX_PATH] = L"";
    wcscpy_s(dir, item.path.c_str());
    PathRemoveFileSpecW(dir);
    ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr,
                  dir[0] ? dir : nullptr, SW_SHOWNORMAL);
    if (g.cfg.bounce) item.bounceStart = GetTickCount();
}

void ActivateItem(int index) {
    if (index < 0 || index >= (int)g.items.size()) return;
    Item& item = g.items[index];
    if (item.windows.empty()) {
        LaunchItem(item);
        return;
    }
    HWND top = item.windows.front();
    if (IsIconic(top)) {
        ShowWindow(top, SW_RESTORE);
        SetForegroundWindow(top);
    } else if (GetForegroundWindow() == top && item.windows.size() == 1) {
        ShowWindow(top, SW_MINIMIZE);
    } else {
        SetForegroundWindow(top);
    }
}

void ShowMenu(int index, POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    const bool hasItem = index >= 0 && index < (int)g.items.size();
    if (hasItem) {
        Item& item = g.items[index];
        if (!item.path.empty())
            AppendMenuW(menu, MF_STRING, kMenuOpen,
                        item.windows.empty() ? L"Abrir" : L"Abrir nueva ventana");
        if (!item.windows.empty())
            AppendMenuW(menu, MF_STRING, kMenuClose, L"Cerrar");
        if (!item.path.empty())
            AppendMenuW(menu, MF_STRING, kMenuPin,
                        item.pinned ? L"Quitar del Dock" : L"Mantener en el Dock");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | (g.cfg.autoHide ? MF_CHECKED : 0),
                kMenuAutoHide, L"Ocultar automáticamente");
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Salir de Mac Island && Dock");
    SetForegroundWindow(g.hwnd);
    const UINT command = (UINT)TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_CENTERALIGN, screenPoint.x,
        screenPoint.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
        case kMenuOpen:
            if (hasItem) LaunchItem(g.items[index]);
            break;
        case kMenuClose:
            if (hasItem)
                for (HWND w : g.items[index].windows)
                    PostMessageW(w, WM_CLOSE, 0, 0);
            break;
        case kMenuPin:
            if (hasItem) {
                Item& item = g.items[index];
                if (item.pinned) {
                    g.pinned.erase(
                        std::remove_if(g.pinned.begin(), g.pinned.end(),
                                       [&](const PinnedDef& d) {
                                           return d.path == item.path;
                                       }),
                        g.pinned.end());
                } else {
                    g.pinned.push_back(MakePinned(item.path));
                }
                SavePinned();
                Refresh();
                ComputeWindowSize();
                g.needRender = true;
            }
            break;
        case kMenuAutoHide:
            g.cfg.autoHide = !g.cfg.autoHide;
            WritePrivateProfileStringW(L"settings", L"hiding.autoHide",
                                       g.cfg.autoHide ? L"1" : L"0",
                                       whc::IniPath().c_str());
            g.needRender = true;
            break;
        case kMenuQuit:
            if (HWND tray = FindWindowW(L"MacIslandDockTray", nullptr))
                PostMessageW(tray, WM_CLOSE, 0, 0);
            break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wParam == kTimerFrame) {
                Tick();
            } else if (wParam == kTimerRefresh) {
                if (Refresh()) {
                    ComputeWindowSize();
                    g.needRender = true;
                }
            }
            return 0;
        case WM_LBUTTONUP:
            ActivateItem(HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
            return 0;
        case WM_MBUTTONUP: {
            const int index = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (index >= 0) LaunchItem(g.items[index]);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = HitTest(p.x, p.y);
            ClientToScreen(hwnd, &p);
            ShowMenu(index, p);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            LoadConfig();
            UpdateMonitor();
            ComputeWindowSize();
            g.needRender = true;
            return 0;
        case kMsgToggle:
            g.userHidden = !g.userHidden;
            ApplyVisibility();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerFrame);
            KillTimer(hwnd, kTimerRefresh);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

DWORD WINAPI ThreadProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g.gdiplusToken, &input, nullptr);

    LoadConfig();
    LoadPinned();
    UpdateMonitor();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    Refresh();
    ComputeWindowSize();
    g.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"Mac Island & Dock", WS_POPUP, g.winX,
        g.monitor.bottom - g.winH, g.winW, g.winH, nullptr, nullptr,
        wc.hInstance, nullptr);
    if (g.hwnd) {
        ApplyTaskbarMode();
        g.lastTick = GetTickCount();
        Render();
        ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
        SetTimer(g.hwnd, kTimerFrame, 16, nullptr);
        SetTimer(g.hwnd, kTimerRefresh, 700, nullptr);
    }
    SetEvent(g.ready);

    if (g.hwnd) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    RestoreTaskbar();
    g.hwnd = nullptr;
    if (g.dib) DeleteObject(g.dib);
    if (g.memDc) DeleteDC(g.memDc);
    g.dib = nullptr;
    g.memDc = nullptr;
    g.items.clear();
    g.iconCache.clear();
    UnregisterClassW(kClassName, wc.hInstance);
    Gdiplus::GdiplusShutdown(g.gdiplusToken);
    CoUninitialize();
    return 0;
}

}  // namespace

bool Start() {
    if (Wh_GetIntSetting(L"dock.enabled") == 0) return false;
    if (g.thread) return true;
    g.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    if (!g.thread) return false;
    WaitForSingleObject(g.ready, 10000);
    CloseHandle(g.ready);
    g.ready = nullptr;
    return g.hwnd != nullptr;
}

void Stop() {
    if (!g.thread) return;
    if (g.hwnd) PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(g.thread, 8000);
    CloseHandle(g.thread);
    g.thread = nullptr;
}

void ToggleVisible() {
    if (g.hwnd) PostMessageW(g.hwnd, kMsgToggle, 0, 0);
}

bool IsVisible() { return g.hwnd && !g.userHidden; }

void RestoreTaskbarIfLeftover() {
    if (Wh_GetIntValue(L"dockTookAutoHide", 0)) {
        SetTaskbarAutoHide(false);
        Wh_DeleteValue(L"dockTookAutoHide");
    }
    if (Wh_GetIntValue(L"dockHidTaskbar", 0)) {
        EnumWindows(HideTaskbarsProc, 1);
        Wh_DeleteValue(L"dockHidTaskbar");
    }
}

}  // namespace dock
