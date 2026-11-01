// GUI tests (run with QT_QPA_PLATFORM=offscreen): every screen is reachable, the Add Download
// dialog inspects real server metadata, the dashboard reflects real state, and the event loop
// stays responsive while transfers run on worker threads.

#include "TestSupport.h"

#include "app/DownloadManager.h"
#include "scheduler/Scheduler.h"
#include "storage/Database.h"
#include "storage/DownloadRepository.h"
#include "storage/SettingsRepository.h"
#include "ui/AddDownloadDialog.h"
#include "ui/DashboardPage.h"
#include "ui/DownloadDetailsPage.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>

using namespace haul;
using namespace testsupport;

namespace {

QPushButton *buttonWithText(QWidget *root, const QString &text)
{
    for (QPushButton *b : root->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

bool hasVisibleLabelContaining(QWidget *root, const QString &text)
{
    for (QLabel *l : root->findChildren<QLabel *>()) {
        if (l->isVisibleTo(root) && l->text().contains(text))
            return true;
    }
    return false;
}

} // namespace

class TestUi : public QObject
{
    Q_OBJECT

    TestServer server;
    QTemporaryDir dir;
    Database db;
    std::unique_ptr<DownloadRepository> repo;
    std::unique_ptr<SettingsRepository> settings;
    std::unique_ptr<DownloadManager> manager;
    std::unique_ptr<Scheduler> scheduler;
    std::unique_ptr<ui::MainWindow> window;

private slots:
    void initTestCase()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        QVERIFY(server.start());
        QVERIFY(db.open(dir.filePath("ui.db")));
        QVERIFY(db.migrate());
        repo = std::make_unique<DownloadRepository>(db);
        settings = std::make_unique<SettingsRepository>(dir.filePath("ui.ini"));
        AppSettings s = settings->current();
        s.downloadDirectory = dir.path();
        s.confirmBeforeDelete = false;
        QVERIFY(settings->save(s));
        ui::Theme::apply(*qApp, QStringLiteral("dark"));
        manager = std::make_unique<DownloadManager>(repo.get(), settings.get());
        manager->restore();
        scheduler = std::make_unique<Scheduler>(settings.get(), manager.get());
        window = std::make_unique<ui::MainWindow>(manager.get(), settings.get(), scheduler.get());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }

    void cleanupTestCase()
    {
        window.reset();
        scheduler.reset();
        manager.reset();
        curl_global_cleanup();
    }

    void emptyStateOnFirstRun()
    {
        QWidget *dashboard = window->pageWidget(ui::MainWindow::Dashboard);
        QVERIFY(hasVisibleLabelContaining(dashboard, QStringLiteral("Nothing here yet")));
    }

    void allScreensAreReachable()
    {
        auto *stack = window->findChild<QStackedWidget *>();
        QVERIFY(stack);
        for (auto page : { ui::MainWindow::Queue, ui::MainWindow::Details, ui::MainWindow::Settings, ui::MainWindow::Dashboard }) {
            window->showPage(page);
            QCOMPARE(window->pageWidget(page)->isVisible(), true);
        }
        window->showPage(ui::MainWindow::Details);
        QVERIFY(hasVisibleLabelContaining(window->pageWidget(ui::MainWindow::Details), QStringLiteral("No download selected")));
        window->showPage(ui::MainWindow::Dashboard);
    }

    void addDialogRejectsBadUrls()
    {
        ui::AddDownloadDialog dialog(manager.get(), settings.get(), window.get());
        dialog.setUrl(QStringLiteral("gopher://example.com/file"));
        QPushButton *start = buttonWithText(&dialog, QStringLiteral("Start now"));
        QVERIFY(start);
        QVERIFY(!start->isEnabled());
        QVERIFY(hasVisibleLabelContaining(&dialog, QStringLiteral("Only http, https and ftp")));
    }

    void addDialogInspectsServer()
    {
        ui::AddDownloadDialog dialog(manager.get(), settings.get(), window.get());
        dialog.setUrl(server.url(QStringLiteral("/file/123456?name=report%20final.pdf")));
        // Wait for the debounce and the background probe.
        QTRY_VERIFY_WITH_TIMEOUT(hasVisibleLabelContaining(&dialog, QStringLiteral("Looks good")), 10000);
        QVERIFY(hasVisibleLabelContaining(&dialog, QStringLiteral("120.6 KB")));
        QVERIFY(hasVisibleLabelContaining(&dialog, QStringLiteral("Supported")));

        QLineEdit *name = nullptr;
        for (QLineEdit *e : dialog.findChildren<QLineEdit *>()) {
            if (e->text() == QStringLiteral("report final.pdf"))
                name = e;
        }
        QVERIFY2(name, "file name from Content-Disposition was not filled in");

        QPushButton *start = buttonWithText(&dialog, QStringLiteral("Start now"));
        QVERIFY(start->isEnabled());
        start->click();
        QVERIFY(dialog.createdTaskId() > 0);
        const qint64 id = dialog.createdTaskId();
        QTRY_COMPARE_WITH_TIMEOUT(manager->task(id)->state, TaskState::Completed, 15000);
        QCOMPARE(readFile(manager->task(id)->finalPath()), expectedContent(123456));
    }

    void dashboardShowsRealRows()
    {
        auto *table = window->dashboardPage()->table();
        QTRY_COMPARE(table->model()->rowCount(), static_cast<int>(manager->tasks().size()));
        QVERIFY(table->isVisible());
    }

    void detailsShowSegmentsOnlyWhenSegmented()
    {
        AddRequest request;
        request.url = server.url(QStringLiteral("/file/6000000?rate=1500000"));
        request.directory = dir.path();
        const qint64 id = manager->addDownload(request);
        QVERIFY(id > 0);
        window->showDetails(id);
        auto *details = window->detailsPage();
        QTRY_VERIFY_WITH_TIMEOUT(manager->task(id)->segments.size() > 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(hasVisibleLabelContaining(details, QStringLiteral("(segmented)")), 5000);
        QVERIFY(hasVisibleLabelContaining(details, QStringLiteral("Segments")));
        manager->cancel(id);
        QTRY_COMPARE_WITH_TIMEOUT(manager->task(id)->state, TaskState::Cancelled, 5000);
        QVERIFY(!hasVisibleLabelContaining(details, QStringLiteral("(segmented)")));
    }

    void uiStaysResponsiveDuringTransfers()
    {
        AddRequest request;
        request.url = server.url(QStringLiteral("/file/20000000?rate=4000000"));
        request.directory = dir.path();
        const qint64 id = manager->addDownload(request);
        QVERIFY(id > 0);
        window->showPage(ui::MainWindow::Dashboard);
        QTRY_VERIFY_WITH_TIMEOUT(manager->task(id)->downloadedBytes > 0, 10000);

        // Measure the gap between 10 ms timer ticks on the GUI thread for two seconds.
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0;
        qint64 worstGap = 0;
        QTimer ticker;
        connect(&ticker, &QTimer::timeout, this, [&] {
            const qint64 now = clock.elapsed();
            worstGap = std::max(worstGap, now - last);
            last = now;
        });
        ticker.start(10);
        QTest::qWait(2000);
        ticker.stop();
        manager->cancel(id);
        qInfo("worst GUI event-loop gap during an active download: %lld ms", worstGap);
        QVERIFY2(worstGap < 250, qPrintable(QString("GUI blocked for %1 ms").arg(worstGap)));
    }

    void layoutFitsAtMinimumSize()
    {
        window->showPage(ui::MainWindow::Dashboard);
        window->resize(window->minimumSize());
        QTest::qWait(100);
        QTableView *table = window->dashboardPage()->findChild<QTableView *>();
        QVERIFY(table->isVisible() || table->parentWidget()->isVisible());
        QVERIFY(table->parentWidget()->height() >= 150); // the list keeps its room at the smallest size
        window->resize(1120, 680);
    }

#if defined(Q_OS_LINUX)
    // Regression test: saving used to untick the box before it was read, so nothing was registered.
    void browserCheckboxStaysOnAfterSave()
    {
        QTemporaryDir home;
        const QByteArray oldHome = qgetenv("HOME");
        const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
        QFile helper(QCoreApplication::applicationDirPath() + "/haul-native-host");
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.close();

        QWidget *page = window->pageWidget(ui::MainWindow::Settings);
        QCheckBox *box = nullptr;
        for (QCheckBox *candidate : page->findChildren<QCheckBox *>()) {
            if (candidate->text().contains("browser extension"))
                box = candidate;
        }
        QPushButton *save = nullptr;
        for (QPushButton *candidate : page->findChildren<QPushButton *>()) {
            if (candidate->text() == "Save")
                save = candidate;
        }
        QVERIFY(box && save);
        const QString manifest = home.filePath("config/google-chrome/NativeMessagingHosts/io.github.tehsaad.haul.json");

        box->setChecked(true);
        save->click();
        QVERIFY(box->isChecked());
        QVERIFY(QFile::exists(manifest));

        box->setChecked(false);
        save->click();
        QVERIFY(!box->isChecked());
        QVERIFY(!QFile::exists(manifest));

        helper.remove();
        qputenv("HOME", oldHome);
        qputenv("XDG_CONFIG_HOME", oldConfig);
    }
#endif

    void addDialogOffersStreamQualities()
    {
        ui::AddDownloadDialog dialog(manager.get(), settings.get());
        dialog.setPreferredVariant("360p-800000");
        dialog.setUrl(server.url("/hls/master.m3u8?count=3&ui=1"));
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(hasVisibleLabelContaining(&dialog, QStringLiteral("Video stream found")), 10000);
        QComboBox *quality = nullptr;
        for (QComboBox *box : dialog.findChildren<QComboBox *>()) {
            if (box->findText("720p") >= 0)
                quality = box;
        }
        QVERIFY(quality && quality->isVisible());
        QCOMPARE(quality->currentText(), QString("360p")); // the choice made in the browser panel
        QVERIFY(hasVisibleLabelContaining(&dialog, QStringLiteral("About")));
        for (QPushButton *b : dialog.findChildren<QPushButton *>()) {
            if (b->text() == "Start now")
                b->click();
        }
        const qint64 id = dialog.createdTaskId();
        QVERIFY(id > 0);
        QCOMPARE(manager->task(id)->streamKind, StreamKind::Hls);
        QCOMPARE(manager->task(id)->streamVariant, QString("360p-800000"));
        QTRY_COMPARE_WITH_TIMEOUT(manager->task(id)->state, TaskState::Completed, 15000);
    }

    void addDialogRefusesEncryptedStream()
    {
        ui::AddDownloadDialog dialog(manager.get(), settings.get());
        dialog.setUrl(server.url("/hls/high.m3u8?enc=key"));
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(hasVisibleLabelContaining(&dialog, QStringLiteral("encrypted")), 10000);
        for (QPushButton *b : dialog.findChildren<QPushButton *>()) {
            if (b->text() == "Start now")
                QVERIFY(!b->isEnabled());
        }
    }

    void lightThemeApplies()
    {
        AppSettings s = settings->current();
        s.theme = QStringLiteral("light");
        QVERIFY(settings->save(s));
        QVERIFY(!ui::Theme::isDark());
        s.theme = QStringLiteral("dark");
        QVERIFY(settings->save(s));
        QVERIFY(ui::Theme::isDark());
    }
};

QTEST_MAIN(TestUi)
#include "tst_ui.moc"
