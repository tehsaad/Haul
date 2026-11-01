#pragma once

#include "app/InstanceGuard.h"
#include "media/StreamResolver.h"
#include "network/HttpProbe.h"

#include <QByteArray>
#include <QJsonObject>
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

// {"type":"inspect","url":...} asks what qualities a video stream offers, so the browser panel
// can show them using this same parser. The answer never contains a login or a key.
bool isInspectMessage(const QByteArray &json, QUrl *url, QByteArray *referer);
QJsonObject describeStream(const StreamInfo &info);

// {"type":"verify","url":...} checks a media file with the server: its real first bytes, true size
// and whether it can resume. It answers from the response only and uses no browser cookies.
bool isVerifyMessage(const QByteArray &json, QUrl *url, QByteArray *referer);
QJsonObject describeFile(const ProbeResult &probe);

QByteArray nativeHostManifest(const QString &hostPath, bool firefox);

// Registers or removes the host with every supported browser for this user only.
bool isBrowserIntegrationEnabled();
bool setBrowserIntegrationEnabled(bool enabled, QString *error = nullptr);

// Where the installed copy of the extension lives (empty if it cannot be found).
QString browserExtensionDirectory();

} // namespace haul::platform
