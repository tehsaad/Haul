#pragma once

#include <QString>

namespace haul::platform {

// Where Haul keeps its database:
//   Linux:   ~/.local/share/Haul/Haul/haul.db  (XDG_DATA_HOME)
//   Windows: %APPDATA%\Haul\Haul\haul.db
QString databasePath();

// Whether "launch at login" can be offered on this platform.
bool autostartSupported();
bool isAutostartEnabled();
// Linux: writes/removes ~/.config/autostart/haul.desktop.
// Windows: writes/removes HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Haul.
bool setAutostartEnabled(bool enabled);

} // namespace haul::platform
