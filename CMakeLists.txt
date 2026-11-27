cmake_minimum_required(VERSION 3.20)
project(MacIslandDock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # sin depender del VC++ Redistributable

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 1) El mod original -> codigo para app normal (ver tools/make_island_port.py)
set(ISLAND_PORT ${CMAKE_BINARY_DIR}/gen/island_port.cpp)
add_custom_command(
    OUTPUT ${ISLAND_PORT}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_island_port.py
            --in ${CMAKE_SOURCE_DIR}/third_party/mac-island-dock.cpp
            --out ${ISLAND_PORT}
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/mac-island-dock.cpp
            ${CMAKE_SOURCE_DIR}/tools/make_island_port.py
    COMMENT "Generando island_port.cpp a partir del mod de Windhawk")

add_executable(MacIslandDock WIN32
    src/app_main.cpp
    src/dock.cpp
    ${ISLAND_PORT})

target_include_directories(MacIslandDock PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Los mismos defines que usa Windhawk al compilar el mod.
target_compile_definitions(MacIslandDock PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000007
    _CRT_SECURE_NO_WARNINGS)

# La isla usa std::min/std::max sin parentesis: sin las macros de windows.h.
set_source_files_properties(${ISLAND_PORT} PROPERTIES COMPILE_DEFINITIONS NOMINMAX)

if(MSVC)
    target_compile_options(MacIslandDock PRIVATE /utf-8 /EHsc /bigobj /W0 /MP)
endif()

# Equivalentes de los -l del @compilerOptions del mod, mas lo que MSVC pide aparte.
target_link_libraries(MacIslandDock PRIVATE
    ole32 oleaut32 runtimeobject shell32 dwmapi comctl32 gdi32 msimg32
    d2d1 dwrite wlanapi uuid version bthprops dxva2 shlwapi winhttp winmm
    iphlpapi shcore comdlg32 imm32 setupapi cfgmgr32 userenv bcrypt
    gdiplus user32 advapi32 ws2_32 pdh psapi dxgi wbemuuid propsys
    uiautomationcore)
