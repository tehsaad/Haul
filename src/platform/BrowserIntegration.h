#pragma once

#include "app/InstanceGuard.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

// Browser hand-off works through the browsers' "native messaging" feature instead of a local
// web server, so no web page can ever reach Haul: the browser only starts haul-native-host for
// the one extension named in the manifest, and the host only forwards a validated link.
namespace haul::platform {

extern const char *const kNativeHostName;
extern const char *const kChromeExtensionId;
extern const char *const kFirefoxExtensionId;

// `arguments` are the host's command-line arguments (without the program name).
// Chromium passes the caller's origin; Firefox passes the manifest path and the add-on id.
bool isAllowedBrowserCaller(const QStringList &arguments);

// Parses {"type":"add","url":"...","filename":"..."} into a command that opens the Add dialog.
bool parseBrowserMessage(const QByteArray &json, IpcCommand *command, QString *error);

QByteArray nativeHostManifest(const QString &hostPath, bool firefox);

// Registers or removes the host with every supported browser for this user only.
bool isBrowserIntegrationEnabled();
bool setBrowserIntegrationEnabled(bool enabled, QString *error = nullptr);

// Where the installed copy of the extension lives (empty if it cannot be found).
QString browserExtensionDirectory();

} // namespace haul::platform
