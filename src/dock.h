// dock.h - dock estilo macOS, independiente (codigo nuevo, no inyectado).
#pragma once

namespace dock {

// Crea el dock en un hilo propio. Devuelve false si esta desactivado
// (dock.enabled=0 en settings.ini) o si falla.
bool Start();

// Cierra el dock y devuelve la barra de tareas de Windows a como estaba.
void Stop();

// Muestra u oculta el dock (menu del icono de la bandeja).
void ToggleVisible();
bool IsVisible();

// Si quedo la barra de tareas oculta/en autoocultar por un cierre abrupto.
void RestoreTaskbarIfLeftover();

}  // namespace dock
