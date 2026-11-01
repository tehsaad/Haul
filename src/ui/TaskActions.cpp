#include "TaskActions.h"

#include "Theme.h"
#include "app/DownloadManager.h"
#include "storage/SettingsRepository.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QUrl>

namespace haul::ui {

TaskActions::TaskActions(DownloadManager *manager, SettingsRepository *settings, QWidget *dialogParent)
    : QObject(dialogParent)
    , m_manager(manager)
    , m_settings(settings)
    , m_dialogParent(dialogParent)
{
}

QMenu *TaskActions::buildMenu(qint64 id, QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const DownloadTask *t = m_manager->task(id);
    if (!t)
        return menu;
    const TaskState s = t->state;

    QAction *open = menu->addAction(Theme::icon(QStringLiteral("open")), QStringLiteral("Open file"), this, [this, id] { openFile(id); });
    open->setEnabled(s == TaskState::Completed);
    menu->addAction(Theme::icon(QStringLiteral("folder")), QStringLiteral("Open containing folder"), this, [this, id] { openFolder(id); });
    menu->addAction(Theme::icon(QStringLiteral("details")), QStringLiteral("Details"), this, [this, id] { emit detailsRequested(id); });
    menu->addSeparator();

    QAction *pause = menu->addAction(Theme::icon(QStringLiteral("pause")), QStringLiteral("Pause"), this, [this, id] { m_manager->pause(id); });
    pause->setEnabled(s == TaskState::Downloading || s == TaskState::Queued);
    QAction *resume = menu->addAction(Theme::icon(QStringLiteral("play")), QStringLiteral("Resume"), this, [this, id] { m_manager->resume(id); });
    resume->setEnabled(s == TaskState::Paused);
    QAction *retry = menu->addAction(Theme::icon(QStringLiteral("retry")), QStringLiteral("Retry"), this, [this, id] { m_manager->retry(id); });
    retry->setEnabled(s == TaskState::Failed || s == TaskState::Cancelled || (s == TaskState::Queued && t->retryAt.isValid()));
    QAction *cancel = menu->addAction(Theme::icon(QStringLiteral("cancel")), QStringLiteral("Cancel"), this, [this, id] { m_manager->cancel(id); });
    cancel->setEnabled(!isFinishedState(s));
    menu->addSeparator();

    QAction *login = menu->addAction(QStringLiteral("Log in…"), this, [this, id] { askForLogin(id); });
    login->setEnabled(s != TaskState::Completed);
    menu->addAction(Theme::icon(QStringLiteral("copy")), QStringLiteral("Copy URL"), this, [this, id] { copyUrl(id); });
    menu->addAction(Theme::icon(QStringLiteral("trash"), Theme::current().danger), QStringLiteral("Remove from list…"), this,
                    [this, id] { removeWithConfirmation(id); });
    return menu;
}

void TaskActions::askForLogin(qint64 id)
{
    const DownloadTask *t = m_manager->task(id);
    if (!t)
        return;
    QDialog dialog(m_dialogParent);
    dialog.setWindowTitle(QStringLiteral("Log in"));
    auto *form = new QFormLayout(&dialog);
    auto *user = new QLineEdit(&dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    auto *cookie = new QLineEdit(&dialog);
    form->addRow(new QLabel(t->fileName, &dialog));
    form->addRow(QStringLiteral("User name"), user);
    form->addRow(QStringLiteral("Password"), password);
    form->addRow(QStringLiteral("Cookie"), cookie);
    form->addRow(new QLabel(QStringLiteral("Not saved. Haul forgets this when it closes."), &dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    DownloadLogin login;
    login.username = user->text();
    login.password = password->text();
    login.cookie = cookie->text().trimmed().toUtf8();
    m_manager->logIn(id, login);
}

void TaskActions::openFile(qint64 id)
{
    const DownloadTask *t = m_manager->task(id);
    if (!t || t->state != TaskState::Completed)
        return;
    if (!QFileInfo::exists(t->finalPath())) {
        emit message(QStringLiteral("File not found"), QStringLiteral("%1 was moved or deleted.").arg(t->fileName), true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(t->finalPath()));
}

void TaskActions::openFolder(qint64 id)
{
    const DownloadTask *t = m_manager->task(id);
    if (!t)
        return;
#if defined(Q_OS_WIN)
    if (QFileInfo::exists(t->finalPath())) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                { QStringLiteral("/select,"), QDir::toNativeSeparators(t->finalPath()) });
        return;
    }
#endif
    if (!QFileInfo::exists(t->directory)) {
        emit message(QStringLiteral("Folder not found"), t->directory, true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(t->directory));
}

void TaskActions::copyUrl(qint64 id)
{
    if (const DownloadTask *t = m_manager->task(id)) {
        QApplication::clipboard()->setText(t->url);
        emit message(QStringLiteral("Copied"), QStringLiteral("The download URL is on the clipboard."), false);
    }
}

bool TaskActions::removeWithConfirmation(qint64 id)
{
    const DownloadTask *t = m_manager->task(id);
    if (!t)
        return false;
    const bool completed = t->state == TaskState::Completed;
    bool deleteFile = false;

    if (m_settings->current().confirmBeforeDelete) {
        QMessageBox box(m_dialogParent);
        box.setWindowTitle(QStringLiteral("Remove download"));
        box.setText(QStringLiteral("Remove “%1” from the list?").arg(t->fileName));
        box.setInformativeText(completed ? QStringLiteral("The downloaded file stays on disk unless you choose to delete it.")
                                         : QStringLiteral("Any partially downloaded data will be deleted."));
        QCheckBox *alsoDelete = nullptr;
        if (completed) {
            alsoDelete = new QCheckBox(QStringLiteral("Also delete the file from disk"), &box);
            box.setCheckBox(alsoDelete);
        }
        QPushButton *remove = box.addButton(QStringLiteral("Remove"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != remove)
            return false;
        deleteFile = alsoDelete && alsoDelete->isChecked();
    }
    return m_manager->remove(id, deleteFile);
}

} // namespace haul::ui
