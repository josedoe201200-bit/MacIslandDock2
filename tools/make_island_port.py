#!/usr/bin/env python3
"""Convierte el mod de Windhawk en codigo compilable como aplicacion normal.

Entrada : third_party/mac-island-dock.cpp   (mod original, sin tocar)
Salida  : <out>/island_port.cpp

Cambios (todos mecanicos y verificados; si el original cambia y un marcador
no se encuentra, el script falla en lugar de generar algo a medias):
  1. #include <windhawk_utils.h>  ->  #include "wh_compat.h"
  2. La tabla de simbolos de taskbar.dll (hooks en explorer.exe) se quita:
     HookTaskbarDllSymbols() devuelve false. La app no se inyecta en explorer.
  3. Se corta el "snippet" de Windhawk para tool mods (Wh_ModInit, etc.) y se
     reemplaza por IslandPort_Init / IslandPort_SettingsChanged /
     IslandPort_Uninit, que llaman a WhTool_ModInit/... del propio mod.
"""
import argparse
import pathlib
import sys

INCLUDE_OLD = "#include <windhawk_utils.h>"
INCLUDE_NEW = '#include "wh_compat.h"'

HOOK_START = "    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {"
HOOK_END = (
    "    if (!HookSymbols(module, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks))) {\n"
    '        IslandLog(L"HookSymbols failed");\n'
    "        return false;\n"
    "    }\n"
    "    return true;\n"
    "}\n"
)
HOOK_NEW = (
    '    IslandLog(L"Standalone build: taskbar hooks are not available");\n'
    "    (void)module;\n"
    "    return false;\n"
    "}\n"
)

TAIL_MARKER = "bool g_isToolModProcessLauncher;"
TAIL_NEW = r'''
////////////////////////////////////////////////////////////////////////////////
// Standalone entry points (replace Windhawk's tool-mod launcher snippet).

HANDLE g_toolModProcessMutex = nullptr;

bool IslandPort_Init() {
    g_module = GetModuleHandle(nullptr);
    g_isExplorer = false;
    // Same mutex name the mod looks for to know whether "the island's process"
    // is running (see IslandProcessRunning).
    g_toolModProcessMutex =
        CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
    if (!g_toolModProcessMutex) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_toolModProcessMutex);
        g_toolModProcessMutex = nullptr;
        return false;
    }
    return WhTool_ModInit() != FALSE;
}

void IslandPort_SettingsChanged() { WhTool_ModSettingsChanged(); }

void IslandPort_Uninit() { WhTool_ModUninit(); }
'''


def replace_once(text: str, old: str, new: str, what: str) -> str:
    if text.count(old) != 1:
        sys.exit(f"ERROR: marcador '{what}' encontrado {text.count(old)} veces (se esperaba 1)")
    return text.replace(old, new)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", dest="out", required=True)
    args = ap.parse_args()

    text = pathlib.Path(args.src).read_text(encoding="utf-8")

    text = replace_once(text, INCLUDE_OLD, INCLUDE_NEW, "include windhawk_utils.h")

    # Ajustes de compatibilidad con Windows SDK / MSVC.
    text = text.replace(
        INCLUDE_NEW,
        INCLUDE_NEW + "\n#ifdef small\n#undef small\n#endif\n"
        "#ifndef WINBOOL\n#define WINBOOL BOOL\n#endif"
    )

    text = text.replace(\n        "#undef WINRT_LEAN_AND_MEAN",\n        "#undef WINRT_LEAN_AND_MEAN\\n#ifdef small\\n#undef small\\n#endif"\n    )\n\n    # SUBCLASSPROC requiere WINAPI (__stdcall), sin depender de CALLBACK.
    for proc in ("ThumbnailSubclassProc", "TaskbarSubclassProc", "TaskbandSubclassProc"):
        text = text.replace(
            f"LRESULT CALLBACK {proc}(",
            f"LRESULT __stdcall {proc}("
        )

    # Evita colisiones con las interfaces ya declaradas por mfidl.h.
    for name in (
        "IMFSensorProcessActivity",
        "IMFSensorActivityReport",
        "IMFSensorActivitiesReport",
        "IMFSensorActivitiesReportCallback",
        "IMFSensorActivityMonitor",
    ):
        text = text.replace(name, name + "Compat")

    # shellapi.h redirige ShellExecute a ShellExecuteW, también en el método COM.
    old_call = (
        "    const HRESULT hr =\n"
        "        file ? shell->ShellExecute(file, args, none, none, show)\n"
        "             : E_OUTOFMEMORY;"
    )
    new_call = (
        '    #pragma push_macro("ShellExecute")\n'
        "    #undef ShellExecute\n"
        "    const HRESULT hr =\n"
        "        file ? shell->ShellExecute(file, args, none, none, show)\n"
        "             : E_OUTOFMEMORY;\n"
        '    #pragma pop_macro("ShellExecute")'
    )
    if text.count(old_call) != 1:
        sys.exit("ERROR: no se encontro la llamada COM ShellExecute esperada")
    text = text.replace(old_call, new_call)

    start = text.find(HOOK_START)
    end = text.find(HOOK_END)
    if start < 0 or end < 0 or end < start or text.count(HOOK_START) != 1:
        sys.exit("ERROR: no se encontro la tabla SYMBOL_HOOK de taskbar.dll")
    text = text[:start] + HOOK_NEW + text[end + len(HOOK_END):]

    tail = text.find(TAIL_MARKER)
    if tail < 0 or text.count(TAIL_MARKER) != 1:
        sys.exit("ERROR: no se encontro el snippet de tool mod de Windhawk")
    text = text[:tail] + TAIL_NEW

    for leftover in ("WindhawkUtils::SYMBOL_HOOK", "HookSymbols(", "Wh_SetFunctionHook",
                     "windhawk_utils.h"):
        if leftover in text:
            sys.exit(f"ERROR: quedo una referencia a {leftover}")

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"OK: {out} ({len(text.splitlines())} lineas)")


if __name__ == "__main__":
    main()
