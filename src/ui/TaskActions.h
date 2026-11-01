#pragma once

#include <QObject>

class QMenu;
class QWidget;

namespace haul {
class DownloadManager;
class SettingsRepository;
}

namespace haul::ui {

// The per-download actions shared by the dashboard, queue and details pages, so the
// context menu and the buttons always behave the same way.
class TaskActions : public QObject
{
    Q_OBJECT
public:
    TaskActions(DownloadManager *manager, SettingsRepository *settings, QWidget *dialogParent);

    QMenu *buildMenu(qint64 id, QWidget *parent);

    void askForLogin(qint64 id);
    void openFile(qint64 id);
    void openFolder(qint64 id);
    void copyUrl(qint64 id);
    // Asks for confirmation when the setting is on. Returns true if the task was removed.
    bool removeWithConfirmation(qint64 id);

signals:
    void detailsRequested(qint64 id);
    void message(const QString &title, const QString &text, bool isError);

private:
    DownloadManager *m_manager;
    SettingsRepository *m_settings;
    QWidget *m_dialogParent;
};

} // namespace haul::ui
