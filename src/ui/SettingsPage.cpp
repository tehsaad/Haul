#include "SettingsPage.h"

#include "Theme.h"
#include "Widgets.h"
#include "media/MediaMerger.h"
#include "platform/BrowserIntegration.h"
#include "platform/Platform.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QUrl>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace haul::ui {

namespace {

QGridLayout *addSection(QVBoxLayout *root, const QString &title, const QString &subtitle, QWidget *parent)
{
    auto *card = makeCard(parent);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(4);
    layout->addWidget(makeLabel(title, QStringLiteral("h2"), card));
    if (!subtitle.isEmpty())
        layout->addWidget(makeLabel(subtitle, QStringLiteral("muted"), card));
    layout->addSpacing(8);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(0, 1);
    grid->setColumnMinimumWidth(1, 320); // controls start at the same x position in every card
    layout->addLayout(grid);
    root->addWidget(card);
    return grid;
}

QSpinBox *makeSpin(int min, int max, const QString &suffix, QWidget *parent)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(min, max);
    spin->setSuffix(suffix);
    spin->setMinimumWidth(130);
    return spin;
}

} // namespace

SettingsPage::SettingsPage(SettingsRepository *settings, QWidget *parent)
    : QWidget(parent)
    , m_settings(settings)
{
    setObjectName(QStringLiteral("Page"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("Page"));
    scroll->setWidget(content);
    outer->addWidget(scroll);

    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(14);
    root->addWidget(makeLabel(QStringLiteral("Settings"), QStringLiteral("title"), content));

    // Downloads
    QGridLayout *g = addSection(root, QStringLiteral("Downloads"), QString(), content);
    g->addWidget(makeLabel(QStringLiteral("Default download folder"), QString(), content), 0, 0);
    auto *dirRow = new QHBoxLayout;
    m_directory = new QLineEdit(content);
    m_directory->setMinimumWidth(320);
    auto *browse = makeButton(QStringLiteral("Browse…"), QStringLiteral("folder"), QString(), content);
    dirRow->addWidget(m_directory);
    dirRow->addWidget(browse);
    g->addLayout(dirRow, 0, 1);
    g->addWidget(makeLabel(QStringLiteral("When adding a download"), QString(), content), 1, 0);
    m_defaultAction = new QComboBox(content);
    m_defaultAction->addItem(QStringLiteral("Start now (default button)"), true);
    m_defaultAction->addItem(QStringLiteral("Add to queue (default button)"), false);
    g->addWidget(m_defaultAction, 1, 1, Qt::AlignLeft);

    // Connections
    g = addSection(root, QStringLiteral("Connections & bandwidth"),
                   QStringLiteral("More connections only help when the server allows byte ranges and is not the bottleneck."),
                   content);
    g->addWidget(makeLabel(QStringLiteral("Maximum simultaneous downloads"), QString(), content), 0, 0);
    m_maxConcurrent = makeSpin(1, 10, QString(), content);
    g->addWidget(m_maxConcurrent, 0, 1, Qt::AlignLeft);
    g->addWidget(makeLabel(QStringLiteral("Maximum connections per download"), QString(), content), 1, 0);
    m_maxConnections = makeSpin(1, 16, QString(), content);
    g->addWidget(m_maxConnections, 1, 1, Qt::AlignLeft);
    g->addWidget(makeLabel(QStringLiteral("Bandwidth limit for all downloads (0 = unlimited)"), QString(), content), 2, 0);
    m_speedLimit = makeSpin(0, 10 * 1024 * 1024, QStringLiteral(" KB/s"), content);
    m_speedLimit->setSingleStep(128);
    g->addWidget(m_speedLimit, 2, 1, Qt::AlignLeft);

    // Retries
    g = addSection(root, QStringLiteral("Automatic retry"),
                   QStringLiteral("Only temporary problems are retried, never things like 404 or a full disk."),
                   content);
    m_autoRetry = new QCheckBox(QStringLiteral("Retry failed downloads automatically"), content);
    g->addWidget(m_autoRetry, 0, 0, 1, 2);
    g->addWidget(makeLabel(QStringLiteral("Maximum attempts"), QString(), content), 1, 0);
    m_maxRetries = makeSpin(0, 10, QString(), content);
    g->addWidget(m_maxRetries, 1, 1, Qt::AlignLeft);
    g->addWidget(makeLabel(QStringLiteral("First retry delay (doubles each time)"), QString(), content), 2, 0);
    m_retryDelay = makeSpin(1, 3600, QStringLiteral(" s"), content);
    g->addWidget(m_retryDelay, 2, 1, Qt::AlignLeft);

    // Behaviour & appearance
    g = addSection(root, QStringLiteral("Behaviour & appearance"), QString(), content);
    g->addWidget(makeLabel(QStringLiteral("Theme"), QString(), content), 0, 0);
    m_theme = new QComboBox(content);
    m_theme->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    m_theme->addItem(QStringLiteral("Light"), QStringLiteral("light"));
    g->addWidget(m_theme, 0, 1, Qt::AlignLeft);
    m_confirmDelete = new QCheckBox(QStringLiteral("Ask before removing a download"), content);
    g->addWidget(m_confirmDelete, 1, 0, 1, 2);
    m_startQueueOnLaunch = new QCheckBox(QStringLiteral("Start the download queue when Haul opens"), content);
    g->addWidget(m_startQueueOnLaunch, 2, 0, 1, 2);
    if (platform::autostartSupported()) {
        m_launchAtLogin = new QCheckBox(QStringLiteral("Launch Haul when I sign in"), content);
        g->addWidget(m_launchAtLogin, 3, 0, 1, 2);
    }

    // Video
    g = addSection(root, QStringLiteral("Video"), QString(), content);
    const QString ffmpeg = findFfmpeg();
    g->addWidget(makeLabel(ffmpeg.isEmpty()
                               ? QStringLiteral("FFmpeg isn't installed. Streams still download, but videos with separate picture and sound are saved as two files, and some stay .ts instead of .mp4.")
                               : QStringLiteral("FFmpeg found: %1").arg(QDir::toNativeSeparators(ffmpeg)),
                           QStringLiteral("muted"), content), 0, 0, 1, 2);

    // Browser
    g = addSection(root, QStringLiteral("Browser"),
                   QStringLiteral("Lets Haul's extension send links here. Haul asks before starting them."), content);
    m_browser = new QCheckBox(QStringLiteral("Allow the browser extension"), content);
    g->addWidget(m_browser, 0, 0, 1, 2);
    auto *openExtension = makeButton(QStringLiteral("Show extension folder"), QString(), QString(), content);
    openExtension->setToolTip(QStringLiteral("Load this folder from chrome://extensions (Developer mode → Load unpacked), or from about:debugging in Firefox."));
    g->addWidget(openExtension, 1, 0, Qt::AlignLeft);
    connect(openExtension, &QPushButton::clicked, this, [this] {
        const QString dir = platform::browserExtensionDirectory();
        if (dir.isEmpty()) {
            m_status->setText(QStringLiteral("The extension folder could not be found."));
            setRole(m_status, QStringLiteral("warning"));
        } else {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });

    // Actions
    auto *actions = new QHBoxLayout;
    m_status = makeLabel(QString(), QStringLiteral("muted"), content);
    m_status->setWordWrap(true);
    auto *defaults = makeButton(QStringLiteral("Restore defaults"), QString(), QStringLiteral("ghost"), content);
    auto *save = makeButton(QStringLiteral("Save"), QString(), QStringLiteral("primary"), content);
    actions->addWidget(m_status, 1);
    actions->addWidget(defaults);
    actions->addWidget(save);
    root->addLayout(actions);
    root->addStretch();

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Default download folder"), m_directory->text());
        if (!dir.isEmpty())
            m_directory->setText(QDir::toNativeSeparators(dir));
    });
    connect(m_autoRetry, &QCheckBox::toggled, m_maxRetries, &QWidget::setEnabled);
    connect(m_autoRetry, &QCheckBox::toggled, m_retryDelay, &QWidget::setEnabled);
    connect(save, &QPushButton::clicked, this, &SettingsPage::save);
    connect(defaults, &QPushButton::clicked, this, &SettingsPage::restoreDefaults);
    connect(m_settings, &SettingsRepository::changed, this, &SettingsPage::load);

    load(m_settings->current());
}

void SettingsPage::load(const AppSettings &s)
{
    m_directory->setText(QDir::toNativeSeparators(s.downloadDirectory));
    m_maxConcurrent->setValue(s.maxConcurrentDownloads);
    m_maxConnections->setValue(s.maxConnectionsPerDownload);
    m_speedLimit->setValue(s.speedLimitKiB);
    m_autoRetry->setChecked(s.autoRetry);
    m_maxRetries->setValue(s.maxRetries);
    m_retryDelay->setValue(s.retryDelaySeconds);
    m_maxRetries->setEnabled(s.autoRetry);
    m_retryDelay->setEnabled(s.autoRetry);
    m_defaultAction->setCurrentIndex(s.startImmediately ? 0 : 1);
    m_theme->setCurrentIndex(m_theme->findData(s.theme));
    m_confirmDelete->setChecked(s.confirmBeforeDelete);
    m_startQueueOnLaunch->setChecked(s.startQueueOnLaunch);
    if (m_launchAtLogin)
        m_launchAtLogin->setChecked(platform::isAutostartEnabled());
    m_browser->setChecked(platform::isBrowserIntegrationEnabled());
}

AppSettings SettingsPage::collect() const
{
    AppSettings s = m_settings->current();
    s.downloadDirectory = QDir::cleanPath(QDir::fromNativeSeparators(m_directory->text().trimmed()));
    s.maxConcurrentDownloads = m_maxConcurrent->value();
    s.maxConnectionsPerDownload = m_maxConnections->value();
    s.speedLimitKiB = m_speedLimit->value();
    s.autoRetry = m_autoRetry->isChecked();
    s.maxRetries = m_maxRetries->value();
    s.retryDelaySeconds = m_retryDelay->value();
    s.startImmediately = m_defaultAction->currentData().toBool();
    s.theme = m_theme->currentData().toString();
    s.confirmBeforeDelete = m_confirmDelete->isChecked();
    s.startQueueOnLaunch = m_startQueueOnLaunch->isChecked();
    s.launchAtLogin = m_launchAtLogin && m_launchAtLogin->isChecked();
    return s;
}

void SettingsPage::save()
{
    const AppSettings s = collect();
    const bool allowBrowser = m_browser->isChecked(); // saving reloads the page, so read this first
    QStringList errors;
    if (!m_settings->save(s, &errors)) {
        m_status->setText(errors.join(QLatin1Char('\n')));
        setRole(m_status, QStringLiteral("error"));
        return;
    }
    if (m_launchAtLogin && platform::isAutostartEnabled() != s.launchAtLogin
        && !platform::setAutostartEnabled(s.launchAtLogin)) {
        m_status->setText(QStringLiteral("Settings saved, but the login item could not be changed."));
        setRole(m_status, QStringLiteral("warning"));
        return;
    }
    QString browserError;
    const bool wasAllowed = platform::isBrowserIntegrationEnabled();
    const bool changed = wasAllowed != allowBrowser;
    if (changed && !platform::setBrowserIntegrationEnabled(allowBrowser, &browserError)) {
        m_browser->setChecked(platform::isBrowserIntegrationEnabled());
        m_status->setText(browserError);
        setRole(m_status, QStringLiteral("warning"));
        return;
    }
    m_browser->setChecked(allowBrowser);
    m_status->setText(!changed ? QStringLiteral("Saved.")
                      : allowBrowser ? QStringLiteral("Saved. The browser extension can now send downloads to Haul.")
                                     : QStringLiteral("Saved. The browser extension can no longer reach Haul."));
    setRole(m_status, QStringLiteral("success"));
}

void SettingsPage::restoreDefaults()
{
    load(SettingsRepository::defaults());
    m_status->setText(QStringLiteral("Defaults loaded. Press Save to keep them."));
    setRole(m_status, QStringLiteral("muted"));
}

} // namespace haul::ui
