#pragma once

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

namespace haul {
class DownloadManager;
class SettingsRepository;
}

namespace haul::ui {

// "Add download" dialog: validates the URL, inspects the server's response headers in the
// background (cancellable), suggests a safe file name and warns about name conflicts.
class AddDownloadDialog : public QDialog
{
    Q_OBJECT
public:
    AddDownloadDialog(DownloadManager *manager, SettingsRepository *settings, QWidget *parent = nullptr);
    ~AddDownloadDialog() override;

    void setUrl(const QString &url);
    qint64 createdTaskId() const { return m_createdId; }

private:
    void onUrlEdited();
    void startInspection();
    void cancelInspection();
    void onInspectionFinished();
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
    QLineEdit *m_directory;
    QLineEdit *m_fileName;
    QLabel *m_conflictHint;
    QLineEdit *m_checksum;
    QComboBox *m_priority;
    QLabel *m_formError;
    QPushButton *m_queueButton;
    QPushButton *m_startButton;

    QTimer m_debounce;
    QFutureWatcher<ProbeResult> m_watcher;
    std::shared_ptr<std::atomic_bool> m_probeCancel;
    QString m_inspectedUrl;
    bool m_nameEditedByUser = false;
    bool m_probeFailedPermanently = false;
    qint64 m_createdId = 0;
};

} // namespace haul::ui
