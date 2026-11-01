#include "DownloadDetailsPage.h"

#include "TaskActions.h"
#include "Theme.h"
#include "Widgets.h"
#include "app/DownloadManager.h"
#include "core/Formatters.h"
#include "core/UrlValidator.h"

#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLocale>
#include <QProgressBar>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace haul::ui {

namespace {

QLabel *addField(QGridLayout *grid, int row, int column, const QString &caption, QWidget *parent)
{
    grid->addWidget(makeLabel(caption, QStringLiteral("caption"), parent), row * 2, column);
    QLabel *value = makeLabel(QStringLiteral("—"), QStringLiteral("value"), parent);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setWordWrap(true);
    grid->addWidget(value, row * 2 + 1, column);
    return value;
}

QString formatTime(const QDateTime &time)
{
    return time.isValid() ? QLocale().toString(time, QStringLiteral("d MMM yyyy, HH:mm:ss")) : QStringLiteral("—");
}

} // namespace

DownloadDetailsPage::DownloadDetailsPage(DownloadManager *manager, TaskActions *actions, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("Page"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this);
    outer->addWidget(m_stack);

    auto *empty = new EmptyState(QStringLiteral("No download selected"),
                                 QStringLiteral("Double-click a download in Downloads or Queue, or choose "
                                                "“Details” from its context menu."),
                                 this);
    m_stack->addWidget(empty);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("Page"));
    scroll->setWidget(content);
    m_stack->addWidget(scroll);

    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(16);

    // Header
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    titles->addWidget(makeLabel(QStringLiteral("Details"), QStringLiteral("caption"), content));
    m_name = makeLabel(QString(), QStringLiteral("title"), content);
    m_name->setWordWrap(true);
    m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(m_name);
    header->addLayout(titles, 1);
    m_badge = new StatusBadge(content);
    header->addWidget(m_badge, 0, Qt::AlignTop);
    root->addLayout(header);

    // Progress card
    auto *progressCard = makeCard(content);
    auto *progressLayout = new QVBoxLayout(progressCard);
    progressLayout->setContentsMargins(12, 10, 12, 10);
    progressLayout->setSpacing(10);
    m_progress = new QProgressBar(progressCard);
    m_progress->setRange(0, 1000);
    m_progressText = makeLabel(QString(), QStringLiteral("muted"), progressCard);
    progressLayout->addWidget(m_progress);
    progressLayout->addWidget(m_progressText);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    m_pause = makeButton(QStringLiteral("Pause"), QStringLiteral("pause"), QString(), progressCard);
    m_resume = makeButton(QStringLiteral("Resume"), QStringLiteral("play"), QStringLiteral("primary"), progressCard);
    m_retry = makeButton(QStringLiteral("Retry"), QStringLiteral("retry"), QString(), progressCard);
    m_cancel = makeButton(QStringLiteral("Cancel"), QStringLiteral("cancel"), QString(), progressCard);
    m_openFolder = makeButton(QStringLiteral("Open folder"), QStringLiteral("folder"), QStringLiteral("ghost"), progressCard);
    m_copyUrl = makeButton(QStringLiteral("Copy URL"), QStringLiteral("copy"), QStringLiteral("ghost"), progressCard);
    m_remove = makeButton(QStringLiteral("Remove"), QStringLiteral("trash"), QStringLiteral("danger"), progressCard);
    for (QPushButton *b : { m_pause, m_resume, m_retry, m_cancel })
        controls->addWidget(b);
    controls->addStretch();
    for (QPushButton *b : { m_openFolder, m_copyUrl, m_remove })
        controls->addWidget(b);
    progressLayout->addLayout(controls);
    root->addWidget(progressCard);

    // Error card (only visible when there is something to say)
    auto *errorCard = makeCard(content);
    auto *errorLayout = new QHBoxLayout(errorCard);
    errorLayout->setContentsMargins(12, 10, 12, 10);
    auto *errorIcon = new QLabel(errorCard);
    errorIcon->setPixmap(Theme::icon(QStringLiteral("alert"), Theme::current().warning).pixmap(20, 20));
    errorIcon->setAlignment(Qt::AlignTop);
    m_error = makeLabel(QString(), QString(), errorCard);
    m_error->setWordWrap(true);
    m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    errorLayout->addWidget(errorIcon);
    errorLayout->addWidget(m_error, 1);
    m_errorCard = errorCard;
    root->addWidget(errorCard);

    // Information grid
    auto *infoCard = makeCard(content);
    auto *grid = new QGridLayout(infoCard);
    grid->setContentsMargins(18, 16, 18, 16);
    grid->setHorizontalSpacing(28);
    grid->setVerticalSpacing(4);
    m_url = addField(grid, 0, 0, QStringLiteral("URL"), infoCard);
    m_destination = addField(grid, 0, 1, QStringLiteral("Saved to"), infoCard);
    m_downloaded = addField(grid, 1, 0, QStringLiteral("Downloaded"), infoCard);
    m_total = addField(grid, 1, 1, QStringLiteral("Total size"), infoCard);
    m_speed = addField(grid, 2, 0, QStringLiteral("Current speed"), infoCard);
    m_eta = addField(grid, 2, 1, QStringLiteral("Time left"), infoCard);
    m_started = addField(grid, 3, 0, QStringLiteral("Started"), infoCard);
    m_completed = addField(grid, 3, 1, QStringLiteral("Completed"), infoCard);
    m_retries = addField(grid, 4, 0, QStringLiteral("Automatic retries"), infoCard);
    m_connections = addField(grid, 4, 1, QStringLiteral("Connections / format"), infoCard);
    m_checksum = addField(grid, 5, 0, QStringLiteral("Integrity"), infoCard);
    for (int r = 0; r < 12; r += 2)
        grid->setRowMinimumHeight(r, 22);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    root->addWidget(infoCard);

    // Segments
    auto *segmentCard = makeCard(content);
    auto *segmentLayout = new QVBoxLayout(segmentCard);
    segmentLayout->setContentsMargins(18, 16, 18, 16);
    segmentLayout->addWidget(makeLabel(QStringLiteral("Segments"), QStringLiteral("h2"), segmentCard));
    m_segmentText = makeLabel(QString(), QStringLiteral("muted"), segmentCard);
    m_segmentText->setWordWrap(true);
    m_segmentBar = new SegmentBar(segmentCard);
    segmentLayout->addWidget(m_segmentText);
    segmentLayout->addWidget(m_segmentBar);
    m_segmentCard = segmentCard;
    root->addWidget(segmentCard);
    root->addStretch();

    connect(m_pause, &QPushButton::clicked, this, [this] { m_manager->pause(m_id); });
    connect(m_resume, &QPushButton::clicked, this, [this] { m_manager->resume(m_id); });
    connect(m_retry, &QPushButton::clicked, this, [this] { m_manager->retry(m_id); });
    connect(m_cancel, &QPushButton::clicked, this, [this] { m_manager->cancel(m_id); });
    connect(m_openFolder, &QPushButton::clicked, this, [this] { m_actions->openFolder(m_id); });
    connect(m_copyUrl, &QPushButton::clicked, this, [this] { m_actions->copyUrl(m_id); });
    connect(m_remove, &QPushButton::clicked, this, [this] { m_actions->removeWithConfirmation(m_id); });

    connect(m_manager, &DownloadManager::taskUpdated, this, [this](qint64 id) {
        if (id == m_id && isVisible())
            refresh();
    });
    connect(m_manager, &DownloadManager::taskRemoved, this, [this](qint64 id) {
        if (id == m_id)
            showTask(0);
    });
}

void DownloadDetailsPage::showTask(qint64 id)
{
    m_id = m_manager->task(id) ? id : 0;
    refresh();
}

void DownloadDetailsPage::refresh()
{
    const DownloadTask *t = m_manager->task(m_id);
    if (!t) {
        m_stack->setCurrentIndex(0);
        return;
    }
    m_stack->setCurrentIndex(1);
    const TaskState s = t->state;

    m_name->setText(t->fileName);
    QString badgeText;
    if (s == TaskState::Paused && t->interrupted)
        badgeText = QStringLiteral("Interrupted");
    else if (s == TaskState::Queued && t->retryAt.isValid())
        badgeText = QStringLiteral("Retrying");
    m_badge->setState(s, badgeText);

    const int percent = t->progressPercent();
    if (t->totalBytes > 0) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(static_cast<int>(1000.0 * std::min(t->downloadedBytes, t->totalBytes) / t->totalBytes));
    } else if (s == TaskState::Downloading) {
        m_progress->setRange(0, 0); // size unknown: busy indicator instead of a made-up percentage
    } else {
        m_progress->setRange(0, 1000);
        m_progress->setValue(s == TaskState::Completed ? 1000 : 0);
    }
    const QString barState = s == TaskState::Completed ? QStringLiteral("completed")
                           : s == TaskState::Failed    ? QStringLiteral("failed")
                           : s == TaskState::Paused    ? QStringLiteral("paused")
                                                       : QString();
    if (m_progress->property("state").toString() != barState) {
        m_progress->setProperty("state", barState);
        m_progress->style()->unpolish(m_progress);
        m_progress->style()->polish(m_progress);
    }
    m_progressText->setText(percent >= 0
        ? QStringLiteral("%1% · %2 of %3").arg(percent).arg(formatBytes(t->downloadedBytes), formatBytes(t->totalBytes))
        : QStringLiteral("%1 downloaded · total size unknown").arg(formatBytes(t->downloadedBytes)));

    m_url->setText(t->url);
    m_url->setToolTip(redactUrl(QUrl(t->url)));
    m_destination->setText(QDir::toNativeSeparators(t->finalPath()));
    m_downloaded->setText(formatBytes(t->downloadedBytes));
    m_total->setText(t->totalBytes >= 0 ? formatBytes(t->totalBytes) : QStringLiteral("Unknown (not reported by the server)"));

    if (s == TaskState::Downloading) {
        m_speed->setText(t->speed > 0 ? formatSpeed(t->speed) : QStringLiteral("Measuring…"));
        const qint64 eta = estimateRemainingSeconds(t->downloadedBytes, t->totalBytes, t->speed);
        m_eta->setText(eta >= 0 ? formatDuration(eta)
                                : (t->totalBytes < 0 ? QStringLiteral("Unknown: the size is not known")
                                                     : QStringLiteral("Calculating…")));
    } else {
        m_speed->setText(QStringLiteral("—"));
        m_eta->setText(QStringLiteral("—"));
    }
    m_started->setText(formatTime(t->startedAt));
    m_completed->setText(formatTime(t->completedAt));
    m_retries->setText(QString::number(t->retryCount));

    if (t->isStream()) {
        QStringList parts { t->streamKind == StreamKind::Hls ? QStringLiteral("HLS") : QStringLiteral("DASH") };
        if (!t->streamLabel.isEmpty())
            parts << t->streamLabel;
        if (t->streamPiecesTotal > 0)
            parts << QStringLiteral("%1 of %2 pieces").arg(t->streamPiecesDone).arg(t->streamPiecesTotal);
        m_connections->setText(parts.join(QStringLiteral(" · ")));
    } else if (!t->segments.isEmpty())
        m_connections->setText(QStringLiteral("%1 (segmented)").arg(t->segments.size()));
    else if (s == TaskState::Downloading || s == TaskState::Completed)
        m_connections->setText(t->acceptRanges ? QStringLiteral("1") : QStringLiteral("1 (server does not support ranges)"));
    else
        m_connections->setText(QStringLiteral("—"));

    switch (t->checksumStatus) {
    case ChecksumStatus::Verified:
        m_checksum->setText(QStringLiteral("SHA-256 verified against the checksum you supplied"));
        setRole(m_checksum, QStringLiteral("success"));
        break;
    case ChecksumStatus::Mismatch:
        m_checksum->setText(QStringLiteral("SHA-256 MISMATCH: do not trust this file"));
        setRole(m_checksum, QStringLiteral("error"));
        break;
    case ChecksumStatus::NotChecked:
        m_checksum->setText(t->expectedSha256.isEmpty()
                                ? QStringLiteral("Not verified (no trusted checksum supplied)")
                                : QStringLiteral("Will be verified when the download finishes"));
        setRole(m_checksum, QStringLiteral("value"));
        break;
    }

    m_errorCard->setVisible(!t->errorMessage.isEmpty());
    m_error->setText(t->errorMessage);

    // Segment details only for downloads that really use several connections.
    m_segmentCard->setVisible(!t->segments.isEmpty());
    if (!t->segments.isEmpty()) {
        QStringList parts;
        for (const SegmentInfo &seg : t->segments) {
            parts << QStringLiteral("#%1 %2/%3").arg(seg.index + 1)
                         .arg(formatBytes(std::min(seg.downloaded, seg.length())), formatBytes(seg.length()));
        }
        m_segmentText->setText(parts.join(QStringLiteral("   ·   ")));
        m_segmentBar->setSegments(t->segments, t->totalBytes);
    }

    m_pause->setEnabled(s == TaskState::Downloading || s == TaskState::Queued);
    m_resume->setEnabled(s == TaskState::Paused);
    m_retry->setEnabled(s == TaskState::Failed || s == TaskState::Cancelled || (s == TaskState::Queued && t->retryAt.isValid()));
    m_cancel->setEnabled(!isFinishedState(s));
}

} // namespace haul::ui
