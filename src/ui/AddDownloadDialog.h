#pragma once

#include "media/StreamResolver.h"
#include "network/HttpProbe.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QTimer>

#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace haul {
class DownloadManager;
class SettingsRepository;
}

namespace haul::ui {

// What checking a link found: the response, and the manifest if it turned out to be one.
struct LinkInspection {
    ProbeResult probe;
    StreamInfo stream;
    bool isStream = false;
};

// "Add download" dialog: validates the URL, inspects the server's response headers in the
// background (cancellable), suggests a safe file name and warns about name conflicts.
class AddDownloadDialog : public QDialog
{
    Q_OBJECT
public:
    AddDownloadDialog(DownloadManager *manager, SettingsRepository *settings, QWidget *parent = nullptr);
    ~AddDownloadDialog() override;

    void setUrl(const QString &url);
    void setFileName(const QString &name);
    void setReferrer(const QString &referrer);
    void setPreferredVariant(const QString &variantId);
    qint64 createdTaskId() const { return m_createdId; }

private:
    DownloadLogin login() const;
    void onUrlEdited();
    void startInspection();
    void cancelInspection();
    void onInspectionFinished();
    void showStream(const StreamInfo &stream, const QString &suggestedName);
    void updateQualityDetails();
    void browseDirectory();
    void updateConflictHint();
    void updateButtons();
    void submit(bool startNow);

    DownloadManager *m_manager;
    SettingsRepository *m_settings;

    QLineEdit *m_url;
    QLabel *m_urlError;
    QLabel *m_inspectStatus;
    QPushButton *m_cancelInspect;
    QLabel *m_sizeValue;
    QLabel *m_resumeValue;
    QLabel *m_typeValue;
    QLabel *m_qualityCaption;
    QComboBox *m_quality;
    QLabel *m_qualityNote;
    QLineEdit *m_directory;
    QLineEdit *m_fileName;
    QLabel *m_conflictHint;
    QLineEdit *m_checksum;
    QComboBox *m_priority;
    QToolButton *m_loginToggle;
    QWidget *m_loginBox;
    QLineEdit *m_username;
    QLineEdit *m_password;
    QLineEdit *m_cookie;
    QLabel *m_loginNote;
    QLabel *m_formError;
    QPushButton *m_queueButton;
    QPushButton *m_startButton;

    QTimer m_debounce;
    QFutureWatcher<LinkInspection> m_watcher;
    std::shared_ptr<std::atomic_bool> m_probeCancel;
    QString m_inspectedUrl;
    QString m_referrer;
    QString m_preferredVariant;
    StreamInfo m_stream;
    bool m_nameEditedByUser = false;
    bool m_probeFailedPermanently = false;
    qint64 m_createdId = 0;
};

} // namespace haul::ui
