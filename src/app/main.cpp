// Haul entry point: sets up storage, the download engine, the scheduler and the window.

#include "app/DownloadManager.h"
#include "app/InstanceGuard.h"
#include "platform/Platform.h"
#include "scheduler/Scheduler.h"
#include "storage/Database.h"
#include "storage/DownloadRepository.h"
#include "storage/SettingsRepository.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QTimer>

#include <curl/curl.h>

using namespace haul;

namespace {

// Development helper used to produce the README screenshots from real application state.
void captureScreenshots(ui::MainWindow &window, const QString &directory)
{
    QDir().mkpath(directory);
    const struct {
        ui::MainWindow::Page page;
        const char *file;
    } pages[] = {
        { ui::MainWindow::Dashboard, "dashboard.png" },
        { ui::MainWindow::Queue, "queue.png" },
        { ui::MainWindow::Details, "details.png" },
        { ui::MainWindow::Settings, "settings.png" },
    };
    for (const auto &p : pages) {
        window.showPage(p.page);
        QApplication::processEvents();
        window.grab().save(QDir(directory).filePath(QString::fromLatin1(p.file)));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Haul"));
    QApplication::setApplicationName(QStringLiteral("Haul"));
    QApplication::setApplicationVersion(QStringLiteral(HAUL_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/haul.svg")));
    QApplication::setDesktopFileName(QStringLiteral("haul"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Haul — open-source download manager"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption addOption(QStringLiteral("add"), QStringLiteral("Add a download for <url> and start it."), QStringLiteral("url"));
    const QCommandLineOption minimizedOption(QStringLiteral("minimized"), QStringLiteral("Start minimized."));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshots"),
                                              QStringLiteral("Developer option: save page screenshots to <dir> after <delay> ms and exit."),
                                              QStringLiteral("dir"));
    const QCommandLineOption delayOption(QStringLiteral("screenshot-delay"), QStringLiteral("Delay before screenshots."),
                                         QStringLiteral("ms"), QStringLiteral("4000"));
    parser.addOption(addOption);
    parser.addOption(minimizedOption);
    parser.addOption(screenshotOption);
    parser.addOption(delayOption);
    parser.process(app);

    // Only one copy may use the database. A second copy passes its request on and leaves.
    InstanceGuard guard;
    if (!parser.isSet(screenshotOption) && !guard.tryBecomePrimary()) {
        QString error;
        const QStringList urls = parser.values(addOption);
        for (const QString &url : urls) {
            IpcCommand add;
            add.type = IpcCommand::Add;
            add.url = url;
            if (!InstanceGuard::send(add, &error))
                QMessageBox::warning(nullptr, QStringLiteral("Haul"), error);
        }
        if (urls.isEmpty() && !InstanceGuard::send(IpcCommand(), &error))
            QMessageBox::warning(nullptr, QStringLiteral("Haul"), QStringLiteral("Haul is already open, but it isn't responding."));
        return 0;
    }

    // libcurl must be initialised once, before any thread uses it.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SettingsRepository settings;
    ui::Theme::apply(app, settings.current().theme);

    Database database;
    if (!database.open(platform::databasePath()) || !database.migrate()) {
        QMessageBox::critical(nullptr, QStringLiteral("Haul"),
                              QStringLiteral("The download database could not be opened:\n%1\n\n%2")
                                  .arg(QDir::toNativeSeparators(platform::databasePath()), database.lastError()));
        curl_global_cleanup();
        return 1;
    }

    int exitCode = 0;
    {
        DownloadRepository repository(database);
        DownloadManager manager(&repository, &settings);
        manager.restore();
        Scheduler scheduler(&settings, &manager);
        scheduler.check(); // handles a start time that passed while Haul was closed

        ui::MainWindow window(&manager, &settings, &scheduler);
        if (parser.isSet(minimizedOption))
            window.showMinimized();
        else
            window.show();

        qint64 lastAdded = 0;
        auto addDirectly = [&](const QString &url) {
            AddRequest request;
            request.url = url;
            request.directory = settings.current().downloadDirectory;
            QString error;
            const qint64 id = manager.addDownload(request, &error);
            if (id == 0)
                window.notify(QStringLiteral("Could not add download"), error, true);
            else
                lastAdded = id;
        };
        for (const QString &url : parser.values(addOption))
            addDirectly(url);

        QObject::connect(&guard, &InstanceGuard::commandReceived, &window, [&](const IpcCommand &command) {
            if (command.type == IpcCommand::Show)
                window.bringToFront();
            else if (command.confirm)
                window.askAboutDownload(command.url, command.fileName);
            else
                addDirectly(command.url);
        });

        if (parser.isSet(screenshotOption)) {
            const QString dir = parser.value(screenshotOption);
            QTimer::singleShot(parser.value(delayOption).toInt(), &window, [&window, dir, lastAdded] {
                if (lastAdded)
                    window.showDetails(lastAdded);
                captureScreenshots(window, dir);
                QApplication::quit();
            });
        }

        exitCode = app.exec();
        manager.shutdown(); // pause running downloads cleanly before the database closes
    }

    curl_global_cleanup();
    return exitCode;
}
