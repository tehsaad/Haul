#include "QueuePage.h"

#include "DownloadTableModel.h"
#include "TaskActions.h"
#include "Theme.h"
#include "Widgets.h"
#include "app/DownloadManager.h"
#include "core/Formatters.h"
#include "scheduler/Scheduler.h"
#include "storage/SettingsRepository.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QProgressBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTimeEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace haul::ui {

QueuePage::QueuePage(DownloadManager *manager, SettingsRepository *settings, Scheduler *scheduler,
                     DownloadTableModel *model, TaskActions *actions, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_settings(settings)
    , m_scheduler(scheduler)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("Page"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(16);

    // Header with queue status
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(makeLabel(QStringLiteral("Queue"), QStringLiteral("title"), this));
    m_queueStatus = makeLabel(QString(), QStringLiteral("subtitle"), this);
    titles->addWidget(m_queueStatus);
    header->addLayout(titles, 1);
    m_toggleQueue = makeButton(QStringLiteral("Start queue"), QStringLiteral("play"), QStringLiteral("primary"), this);
    header->addWidget(m_toggleQueue, 0, Qt::AlignTop);
    root->addLayout(header);
    m_queueProgress = new QProgressBar(this);
    m_queueProgress->setRange(0, 1000);
    root->addWidget(m_queueProgress);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);

    // Left: task lists
    auto *listCard = makeCard(this);
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(0, 8, 0, 0);
    listLayout->setSpacing(6);
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(12, 0, 12, 0);
    m_tabs = new QTabBar(listCard);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->addTab(QStringLiteral("Queued"));
    m_tabs->addTab(QStringLiteral("Active"));
    m_tabs->addTab(QStringLiteral("Paused"));
    m_tabs->addTab(QStringLiteral("Completed"));
    m_tabs->addTab(QStringLiteral("Failed"));
    tabRow->addWidget(m_tabs);
    tabRow->addStretch();
    listLayout->addLayout(tabRow);

    m_filter = new DownloadFilterModel(this);
    m_filter->setSourceModel(model);
    m_filter->setGroup(DownloadFilterModel::Waiting);
    m_filter->setQueueOrder(true);

    m_table = new QTableView(listCard);
    m_table->setModel(m_filter);
    m_table->setItemDelegate(new DownloadItemDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(44);
    QHeaderView *h = m_table->horizontalHeader();
    h->setHighlightSections(false);
    h->setSectionResizeMode(DownloadTableModel::Name, QHeaderView::Stretch);
    h->resizeSection(DownloadTableModel::Status, 130);
    h->resizeSection(DownloadTableModel::Progress, 160);
    h->resizeSection(DownloadTableModel::Size, 150);
    h->setSectionHidden(DownloadTableModel::Speed, true);
    h->setSectionHidden(DownloadTableModel::Eta, true);
    h->setSectionHidden(DownloadTableModel::Category, true);
    h->setSectionHidden(DownloadTableModel::Added, true);
    m_empty = new EmptyState(QString(), QString(), listCard);
    m_listStack = new QStackedWidget(listCard);
    m_listStack->addWidget(m_table);
    m_listStack->addWidget(m_empty);
    listLayout->addWidget(m_listStack, 1);

    auto *orderRow = new QHBoxLayout;
    orderRow->setContentsMargins(12, 6, 12, 12);
    orderRow->addWidget(makeLabel(QStringLiteral("Priority"), QStringLiteral("muted"), listCard));
    m_priority = new QComboBox(listCard);
    m_priority->addItem(QStringLiteral("Low"), static_cast<int>(Priority::Low));
    m_priority->addItem(QStringLiteral("Normal"), static_cast<int>(Priority::Normal));
    m_priority->addItem(QStringLiteral("High"), static_cast<int>(Priority::High));
    orderRow->addWidget(m_priority);
    orderRow->addStretch();
    m_moveUp = makeButton(QStringLiteral("Move up"), QStringLiteral("up"), QString(), listCard);
    m_moveDown = makeButton(QStringLiteral("Move down"), QStringLiteral("down"), QString(), listCard);
    orderRow->addWidget(m_moveUp);
    orderRow->addWidget(m_moveDown);
    listLayout->addLayout(orderRow);
    body->addWidget(listCard, 1);

    // Right: limits and schedule
    auto *side = new QVBoxLayout;
    side->setSpacing(14);

    auto *limitsCard = makeCard(this);
    limitsCard->setFixedWidth(320);
    auto *limits = new QGridLayout(limitsCard);
    limits->setContentsMargins(12, 10, 12, 10);
    limits->setVerticalSpacing(10);
    limits->addWidget(makeLabel(QStringLiteral("Limits"), QStringLiteral("h2"), limitsCard), 0, 0, 1, 2);
    limits->addWidget(makeLabel(QStringLiteral("Simultaneous downloads"), QString(), limitsCard), 1, 0);
    m_maxConcurrent = new QSpinBox(limitsCard);
    m_maxConcurrent->setRange(1, 10);
    limits->addWidget(m_maxConcurrent, 1, 1);
    limits->addWidget(makeLabel(QStringLiteral("Connections per download"), QString(), limitsCard), 2, 0);
    m_maxConnections = new QSpinBox(limitsCard);
    m_maxConnections->setRange(1, 16);
    limits->addWidget(m_maxConnections, 2, 1);
    m_limitsStatus = makeLabel(QStringLiteral("Changes apply to the next downloads that start."), QStringLiteral("muted"), limitsCard);
    m_limitsStatus->setWordWrap(true);
    limits->addWidget(m_limitsStatus, 3, 0, 1, 2);
    side->addWidget(limitsCard);

    auto *scheduleCard = makeCard(this);
    scheduleCard->setFixedWidth(320);
    auto *schedule = new QGridLayout(scheduleCard);
    schedule->setContentsMargins(16, 14, 16, 14);
    schedule->setVerticalSpacing(10);
    schedule->addWidget(makeLabel(QStringLiteral("Schedule"), QStringLiteral("h2"), scheduleCard), 0, 0, 1, 2);
    m_scheduleEnabled = new QCheckBox(QStringLiteral("Start daily at"), scheduleCard);
    m_startTime = new QTimeEdit(scheduleCard);
    m_startTime->setDisplayFormat(QStringLiteral("HH:mm"));
    m_stopEnabled = new QCheckBox(QStringLiteral("Stop daily at"), scheduleCard);
    m_stopTime = new QTimeEdit(scheduleCard);
    m_stopTime->setDisplayFormat(QStringLiteral("HH:mm"));
    schedule->addWidget(m_scheduleEnabled, 1, 0);
    schedule->addWidget(m_startTime, 1, 1);
    schedule->addWidget(m_stopEnabled, 2, 0);
    schedule->addWidget(m_stopTime, 2, 1);
    auto *saveSchedule = makeButton(QStringLiteral("Save schedule"), QStringLiteral("clock"), QString(), scheduleCard);
    schedule->addWidget(saveSchedule, 3, 0, 1, 2);
    m_scheduleError = makeLabel(QString(), QStringLiteral("error"), scheduleCard);
    m_scheduleError->setWordWrap(true);
    m_scheduleError->hide();
    schedule->addWidget(m_scheduleError, 4, 0, 1, 2);
    m_scheduleSummary = makeLabel(QString(), QStringLiteral("value"), scheduleCard);
    m_scheduleSummary->setWordWrap(true);
    schedule->addWidget(m_scheduleSummary, 5, 0, 1, 2);
    auto *note = makeLabel(QStringLiteral("Uses this computer's local time. The schedule only runs while Haul is open "
                                          "(there is no background service). If Haul opens after the start time but "
                                          "before the stop time, the queue starts right away; if the window has already "
                                          "ended, it waits for the next day."),
                           QStringLiteral("muted"), scheduleCard);
    note->setWordWrap(true);
    schedule->addWidget(note, 6, 0, 1, 2);
    side->addWidget(scheduleCard);
    side->addStretch();
    body->addLayout(side);
    root->addLayout(body, 1);

    loadSettings();

    connect(m_toggleQueue, &QPushButton::clicked, this, [this] {
        if (m_manager->isQueueRunning())
            m_manager->stopQueue();
        else
            m_manager->startQueue();
    });
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        static const DownloadFilterModel::Group groups[] = {
            DownloadFilterModel::Waiting, DownloadFilterModel::Active, DownloadFilterModel::Paused,
            DownloadFilterModel::Completed, DownloadFilterModel::Failed
        };
        m_filter->setGroup(groups[index]);
        refreshSelection();
        updateEmptyState();
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &QueuePage::refreshSelection);
    connect(m_priority, &QComboBox::activated, this, [this] {
        m_manager->setPriority(selectedId(), static_cast<Priority>(m_priority->currentData().toInt()));
    });
    connect(m_moveUp, &QPushButton::clicked, this, [this] { m_manager->moveUp(selectedId()); });
    connect(m_moveDown, &QPushButton::clicked, this, [this] { m_manager->moveDown(selectedId()); });
    connect(m_maxConcurrent, &QSpinBox::valueChanged, this, &QueuePage::saveLimits);
    connect(m_maxConnections, &QSpinBox::valueChanged, this, &QueuePage::saveLimits);
    connect(saveSchedule, &QPushButton::clicked, this, &QueuePage::saveSchedule);
    connect(m_scheduleEnabled, &QCheckBox::toggled, m_startTime, &QWidget::setEnabled);
    connect(m_stopEnabled, &QCheckBox::toggled, m_stopTime, &QWidget::setEnabled);
    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_table->indexAt(pos);
        if (index.isValid())
            m_actions->buildMenu(index.data(DownloadTableModel::TaskIdRole).toLongLong(), this)
                ->popup(m_table->viewport()->mapToGlobal(pos));
    });
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        emit m_actions->detailsRequested(index.data(DownloadTableModel::TaskIdRole).toLongLong());
    });

    connect(m_manager, &DownloadManager::queueStateChanged, this, &QueuePage::refreshQueueStatus);
    connect(m_manager, &DownloadManager::statsChanged, this, &QueuePage::refreshQueueStatus);
    connect(m_manager, &DownloadManager::taskUpdated, this, &QueuePage::refreshSelection);
    connect(m_settings, &SettingsRepository::changed, this, &QueuePage::loadSettings);

    for (auto signal : { &QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved })
        connect(m_filter, signal, this, &QueuePage::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::layoutChanged, this, &QueuePage::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::modelReset, this, &QueuePage::updateEmptyState);

    refreshQueueStatus();
    refreshSelection();
    updateEmptyState();
}

void QueuePage::updateEmptyState()
{
    static const char *const titles[] = { "Nothing is waiting", "Nothing is downloading", "Nothing is paused",
                                          "No completed downloads yet", "No failed downloads" };
    static const char *const messages[] = {
        "Downloads added with “Add to queue” wait here until you press Start queue or the schedule starts it.",
        "Running downloads appear here with their live progress.",
        "Paused downloads keep their partial data and can be resumed later.",
        "Finished downloads are listed here.",
        "Downloads that failed or were cancelled appear here so you can retry them.",
    };
    const int tab = std::clamp(m_tabs->currentIndex(), 0, 4);
    m_empty->setMessage(QString::fromUtf8(titles[tab]), QString::fromUtf8(messages[tab]));
    m_listStack->setCurrentWidget(m_filter->rowCount() == 0 ? static_cast<QWidget *>(m_empty) : m_table);
}

qint64 QueuePage::selectedId() const
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    return rows.isEmpty() ? 0 : rows.first().data(DownloadTableModel::TaskIdRole).toLongLong();
}

void QueuePage::refreshQueueStatus()
{
    const bool running = m_manager->isQueueRunning();
    const DownloadStats s = m_manager->stats();
    m_toggleQueue->setText(running ? QStringLiteral("Stop queue") : QStringLiteral("Start queue"));
    m_toggleQueue->setIcon(Theme::icon(running ? QStringLiteral("stop") : QStringLiteral("play"), Theme::current().onAccent));
    m_queueStatus->setText(QStringLiteral("Queue %1 · %2 downloading · %3 waiting · limit %4 at a time")
                               .arg(running ? QStringLiteral("running") : QStringLiteral("stopped"))
                               .arg(s.active).arg(s.queued).arg(m_settings->current().maxConcurrentDownloads));

    // Overall progress of the queue: active and waiting downloads whose size is known.
    qint64 done = 0;
    qint64 total = 0;
    for (const DownloadTask &t : m_manager->tasks()) {
        if ((t.state == TaskState::Queued || t.state == TaskState::Downloading) && t.totalBytes > 0) {
            done += std::min(t.downloadedBytes, t.totalBytes);
            total += t.totalBytes;
        }
    }
    m_queueProgress->setVisible(total > 0);
    if (total > 0)
        m_queueProgress->setValue(static_cast<int>(1000.0 * done / total));
    m_queueProgress->setToolTip(total > 0 ? QStringLiteral("%1 of %2").arg(formatBytes(done), formatBytes(total)) : QString());
}

void QueuePage::refreshSelection()
{
    const DownloadTask *t = m_manager->task(selectedId());
    const bool waiting = t && t->state == TaskState::Queued;
    m_priority->setEnabled(t && t->state != TaskState::Completed);
    m_moveUp->setEnabled(waiting);
    m_moveDown->setEnabled(waiting);
    if (t)
        m_priority->setCurrentIndex(m_priority->findData(static_cast<int>(t->priority)));
}

void QueuePage::loadSettings()
{
    const AppSettings &s = m_settings->current();
    const QSignalBlocker b1(m_maxConcurrent);
    const QSignalBlocker b2(m_maxConnections);
    m_maxConcurrent->setValue(s.maxConcurrentDownloads);
    m_maxConnections->setValue(s.maxConnectionsPerDownload);
    m_scheduleEnabled->setChecked(s.schedule.enabled);
    m_startTime->setTime(s.schedule.startTime);
    m_startTime->setEnabled(s.schedule.enabled);
    m_stopEnabled->setChecked(s.schedule.stopEnabled);
    m_stopTime->setTime(s.schedule.stopTime);
    m_stopTime->setEnabled(s.schedule.stopEnabled);
    m_scheduleSummary->setText(m_scheduler->describe());
    refreshQueueStatus();
}

void QueuePage::saveLimits()
{
    AppSettings s = m_settings->current();
    s.maxConcurrentDownloads = m_maxConcurrent->value();
    s.maxConnectionsPerDownload = m_maxConnections->value();
    QStringList errors;
    if (!m_settings->save(s, &errors))
        m_limitsStatus->setText(errors.join(QLatin1Char('\n')));
    else
        m_limitsStatus->setText(QStringLiteral("Saved. New limits apply to downloads as they start."));
}

void QueuePage::saveSchedule()
{
    AppSettings s = m_settings->current();
    s.schedule.enabled = m_scheduleEnabled->isChecked();
    s.schedule.startTime = m_startTime->time();
    s.schedule.stopEnabled = m_stopEnabled->isChecked();
    s.schedule.stopTime = m_stopTime->time();
    QStringList errors;
    if (!m_settings->save(s, &errors)) {
        m_scheduleError->setText(errors.join(QLatin1Char('\n')));
        m_scheduleError->show();
        return;
    }
    m_scheduleError->hide();
    m_scheduleSummary->setText(m_scheduler->describe());
}

} // namespace haul::ui
