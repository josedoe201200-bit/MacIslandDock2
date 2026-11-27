# Mac Island & Dock — app para Windows

App independiente (un `.exe`, sin Windhawk) basada en el mod
[mac-island-dock](https://windhawk.net/mods/mac-island-dock) v3.3.0 de **caliberda**
(GPL-3.0).

## Qué es cada parte

| Parte | Cómo está hecha |
|---|---|
| **Isla** (reloj, centro de control, notificaciones, media, calendario, mascota, ajustes propios…) | **El código original del mod, sin reescribir** (`third_party/mac-island-dock.cpp`, 151k líneas). `tools/make_island_port.py` lo convierte en código de app normal y `src/wh_compat.h` reemplaza las ~8 funciones de Windhawk que usa (valores, ajustes, ruta de almacenamiento, log). |
| **Dock** | **Código nuevo** (`src/dock.cpp`). El dock del mod se inyecta en el XAML de la barra de tareas dentro de `explorer.exe`; una app normal no puede hacer eso, así que aquí es una ventana propia con el mismo estilo: panel redondeado translúcido, magnificación, punto de app abierta, separador, rebote al abrir, autoocultar y etiqueta con el nombre. |
| **Bandeja** | `src/app_main.cpp`: mostrar/ocultar dock, reiniciar isla, iniciar con Windows, abrir carpeta de datos, salir. |

### Lo que NO se puede (y por qué)
- Los hooks a `taskbar.dll` (la parte del mod que corre dentro de explorer.exe) no
  existen: se quitan en la conversión. Por eso el dock es nuevo y la lista de iconos de la
  bandeja del sistema que la isla recibe *desde explorer* puede venir vacía o limitada.
- El fondo del dock es translúcido, **sin desenfoque (blur)** del escritorio.
- Los ajustes que en Windhawk estaban en su pantalla de ajustes ahora están en
  `%LOCALAPPDATA%\MacIslandDock\settings.ini` (los valores por defecto son una estimación;
  la isla guarda el resto de sus opciones en su propia pantalla de ajustes).

## Compilar (sin instalar nada en tu PC): GitHub Actions
1. Sube esta carpeta a un repositorio nuevo de GitHub (el `.github/workflows/build.yml` ya está).
2. Pestaña **Actions → Build MacIslandDock → Run workflow** (o simplemente haz *push*).
3. Al terminar, descarga el artefacto `MacIslandDock-ClangCL` (o `-v143`) → `MacIslandDock.exe`.

La compilación del archivo de la isla es muy pesada (puede tardar varios minutos).

### Compilar en tu PC
Visual Studio 2022 (carga de trabajo *Desarrollo para el escritorio con C++* y, para el
primer toolset, el componente *Herramientas de C++ Clang*), CMake y Python 3:
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build build --config Release
```

## Uso
- Ejecuta `MacIslandDock.exe`. Aparece un icono en la bandeja y arrancan la isla y el dock.
- Por defecto la barra de Windows pasa a *autoocultar* (se restaura al salir). En
  `settings.ini`, `dock.taskbar` acepta `autohide`, `hide` o `keep`.
- Si el programa se cerró de golpe y la barra quedó rara: `MacIslandDock.exe --restore-taskbar`.
- Argumentos: `--no-dock`, `--no-island`.
- Dock: clic = abrir/activar/minimizar; clic central = nueva instancia; clic derecho = menú
  (mantener/quitar del Dock, cerrar, ocultar automáticamente). Las apps fijadas se importan
  la primera vez desde las fijadas en la barra de Windows.

## Ajustes de `settings.ini` (sección `[settings]`)
`dock.enabled`, `dock.taskbar`, `dock.iconSize` (24–64), `dock.iconGap`, `dock.bottomGap`,
`dock.cornerRadius`, `dock.background`, `dock.backgroundOpacity` (0–100),
`dock.dotIndicator`, `dock.separator`, `icons.magnification` (100–200),
`icons.magnificationRange`, `icons.bounceOnLaunch`, `hiding.autoHide`, `hiding.showDelay`,
`hiding.hideDelay`, `hiding.revealHeight`, `hiding.fullscreenGuard`, `showIsland`,
`animations.*`. Añade `[app]` / `log=1` para escribir `log.txt`.

## Estado
Escrito y revisado **sin poder compilar** (el entorno donde se creó no tiene compilador de
Windows). El generador `make_island_port.py` sí se probó contra el archivo original. Es
probable que la primera compilación pida pequeños ajustes (librerías de enlace, algún
encabezado que MSVC/Clang-CL trate distinto a MinGW): pega el log de Actions y se corrigen.

## Licencia y créditos
GPL-3.0. Isla: © 2026 caliberda — https://github.com/cesarkali . El código que alcanza el XAML
de la barra en el mod original se basa en "Taskbar tray auto-hide" de m417z. Ver `NOTICE`.
