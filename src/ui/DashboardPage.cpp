#include "DashboardPage.h"

#include "DownloadTableModel.h"
#include "TaskActions.h"
#include "Theme.h"
#include "Widgets.h"
#include "app/DownloadManager.h"
#include "core/Formatters.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QStackedWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace haul::ui {

namespace {
constexpr int kSpeedHistoryLength = 60; // one sample per second
}

DashboardPage::DashboardPage(DownloadManager *manager, DownloadTableModel *model, TaskActions *actions, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_model(model)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("Page"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(18);

    // Header
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(makeLabel(QStringLiteral("Dashboard"), QStringLiteral("title"), this));
    auto *subtitle = makeLabel(QStringLiteral("Everything Haul is downloading, waiting for and has finished."),
                               QStringLiteral("subtitle"), this);
    subtitle->setWordWrap(true);
    titles->addWidget(subtitle);
    header->addLayout(titles, 1);
    auto *pauseAll = makeButton(QStringLiteral("Pause all"), QStringLiteral("pause"), QString(), this);
    auto *resumeAll = makeButton(QStringLiteral("Resume all"), QStringLiteral("play"), QString(), this);
    auto *add = makeButton(QStringLiteral("Add download"), QStringLiteral("plus"), QStringLiteral("primary"), this);
    pauseAll->setToolTip(QStringLiteral("Pause every running and waiting download"));
    resumeAll->setToolTip(QStringLiteral("Resume every paused download"));
    header->addWidget(pauseAll);
    header->addWidget(resumeAll);
    header->addWidget(add);
    root->addLayout(header);

    // Statistics
    auto *stats = new QHBoxLayout;
    stats->setSpacing(14);
    m_activeCard = new StatCard(QStringLiteral("Active"), QStringLiteral("download"), this);
    m_speedCard = new StatCard(QStringLiteral("Speed"), QStringLiteral("speed"), this);
    m_completedCard = new StatCard(QStringLiteral("Completed"), QStringLiteral("check"), this);
    m_failedCard = new StatCard(QStringLiteral("Failed"), QStringLiteral("alert"), this);
    m_dataCard = new StatCard(QStringLiteral("Downloaded"), QStringLiteral("data"), this);
    m_speedCard->sparkline();
    stats->addWidget(m_activeCard, 2);
    stats->addWidget(m_speedCard, 3);
    stats->addWidget(m_completedCard, 2);
    stats->addWidget(m_failedCard, 2);
    stats->addWidget(m_dataCard, 2);
    root->addLayout(stats);

    // Main area: table card + recent activity card
    auto *main = new QHBoxLayout;
    main->setSpacing(14);

    auto *tableCard = makeCard(this);
    auto *tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(0, 12, 0, 0);
    tableLayout->setSpacing(10);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(14, 0, 14, 0);
    toolbar->addWidget(makeLabel(QStringLiteral("Downloads"), QStringLiteral("h2"), tableCard));
    toolbar->addStretch();
    m_search = new QLineEdit(tableCard);
    m_search->setPlaceholderText(QStringLiteral("Search name or URL   (Ctrl+F)"));
    m_search->addAction(Theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(180);
    m_group = new QComboBox(tableCard);
    m_group->addItem(QStringLiteral("All downloads"), DownloadFilterModel::All);
    m_group->addItem(QStringLiteral("Downloading"), DownloadFilterModel::Active);
    m_group->addItem(QStringLiteral("Queued"), DownloadFilterModel::Waiting);
    m_group->addItem(QStringLiteral("Paused"), DownloadFilterModel::Paused);
    m_group->addItem(QStringLiteral("Completed"), DownloadFilterModel::Completed);
    m_group->addItem(QStringLiteral("Failed / cancelled"), DownloadFilterModel::Failed);
    toolbar->addWidget(m_search);
    toolbar->addWidget(m_group);
    tableLayout->addLayout(toolbar);

    m_filter = new DownloadFilterModel(this);
    m_filter->setSourceModel(m_model);

    m_table = new QTableView(tableCard);
    m_table->setModel(m_filter);
    m_table->setItemDelegate(new DownloadItemDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(DownloadTableModel::Added, Qt::DescendingOrder);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setMouseTracking(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(44);
    m_table->setIconSize(QSize(18, 18));
    QHeaderView *h = m_table->horizontalHeader();
    h->setHighlightSections(false);
    h->setSectionResizeMode(QHeaderView::Interactive);
    h->setSectionResizeMode(DownloadTableModel::Name, QHeaderView::Stretch);
    h->setMinimumSectionSize(60);
    h->resizeSection(DownloadTableModel::Status, 118);
    h->resizeSection(DownloadTableModel::Progress, 130);
    h->resizeSection(DownloadTableModel::Size, 140);
    h->resizeSection(DownloadTableModel::Speed, 88);
    h->resizeSection(DownloadTableModel::Eta, 76);
    m_table->setTextElideMode(Qt::ElideMiddle);
    h->setSectionHidden(DownloadTableModel::Category, true);
    h->setSectionHidden(DownloadTableModel::Added, true);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_empty = new EmptyState(QStringLiteral("download"), QStringLiteral("No downloads yet"),
                             QStringLiteral("Add a link to start. Haul resumes interrupted downloads, uses several "
                                            "connections when the server allows it, and keeps your history."),
                             tableCard);
    auto *emptyAdd = m_empty->addAction(QStringLiteral("Add your first download"), QStringLiteral("plus"), QStringLiteral("primary"));

    m_tableStack = new QStackedWidget(tableCard);
    m_tableStack->addWidget(m_table);
    m_tableStack->addWidget(m_empty);
    tableLayout->addWidget(m_tableStack, 1);
    main->addWidget(tableCard, 1);

    auto *recentCard = makeCard(this);
    recentCard->setFixedWidth(250);
    auto *recentLayout = new QVBoxLayout(recentCard);
    recentLayout->setContentsMargins(16, 14, 16, 14);
    recentLayout->addWidget(makeLabel(QStringLiteral("Recent activity"), QStringLiteral("h2"), recentCard));
    m_recent = new QListWidget(recentCard);
    m_recent->setFocusPolicy(Qt::NoFocus);
    m_recent->setSelectionMode(QAbstractItemView::NoSelection);
    m_recent->setIconSize(QSize(16, 16));
    m_recent->setWordWrap(true);
    recentLayout->addWidget(m_recent, 1);
    main->addWidget(recentCard);
    root->addLayout(main, 1);

    connect(add, &QPushButton::clicked, this, &DashboardPage::addRequested);
    connect(emptyAdd, &QPushButton::clicked, this, &DashboardPage::addRequested);
    connect(pauseAll, &QPushButton::clicked, m_manager, &DownloadManager::pauseAll);
    connect(resumeAll, &QPushButton::clicked, m_manager, &DownloadManager::resumeAll);
    connect(m_search, &QLineEdit::textChanged, m_filter, &DownloadFilterModel::setSearchText);
    connect(m_search, &QLineEdit::textChanged, this, &DashboardPage::updateEmptyState);
    connect(m_group, &QComboBox::currentIndexChanged, this, [this] {
        m_filter->setGroup(static_cast<DownloadFilterModel::Group>(m_group->currentData().toInt()));
        updateEmptyState();
    });
    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_table->indexAt(pos);
        if (!index.isValid())
            return;
        const qint64 id = index.data(DownloadTableModel::TaskIdRole).toLongLong();
        m_actions->buildMenu(id, this)->popup(m_table->viewport()->mapToGlobal(pos));
    });
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        emit m_actions->detailsRequested(index.data(DownloadTableModel::TaskIdRole).toLongLong());
    });

    connect(m_manager, &DownloadManager::statsChanged, this, &DashboardPage::refreshStats);
    connect(m_manager, &DownloadManager::taskAdded, this, &DashboardPage::updateEmptyState);
    connect(m_manager, &DownloadManager::taskRemoved, this, &DashboardPage::updateEmptyState);
    connect(m_manager, &DownloadManager::taskUpdated, this, [this] { m_recentDirty = true; });
    connect(m_manager, &DownloadManager::taskRemoved, this, [this] { m_recentDirty = true; });

    // Speed history and the activity list are sampled once per second, not on every packet.
    auto *sampler = new QTimer(this);
    connect(sampler, &QTimer::timeout, this, &DashboardPage::sampleSpeed);
    sampler->start(1000);

    refreshStats();
    refreshRecent();
    updateEmptyState();
}

void DashboardPage::focusSearch()
{
    m_search->setFocus();
    m_search->selectAll();
}

qint64 DashboardPage::selectedTaskId() const
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    return rows.isEmpty() ? 0 : rows.first().data(DownloadTableModel::TaskIdRole).toLongLong();
}

void DashboardPage::refreshStats()
{
    const DownloadStats s = m_manager->stats();
    m_activeCard->setValue(QString::number(s.active),
                           QStringLiteral("%1 queued · %2 paused").arg(s.queued).arg(s.paused));

    const double peak = m_speedHistory.isEmpty() ? 0.0 : *std::max_element(m_speedHistory.begin(), m_speedHistory.end());
    m_speedCard->setValue(s.speed > 0 ? formatSpeed(s.speed) : QStringLiteral("0 B/s"),
                          peak > 0 ? QStringLiteral("Peak %1 in the last minute").arg(formatSpeed(peak))
                                   : QStringLiteral("Idle"));

    m_completedCard->setValue(QString::number(s.completed), QStringLiteral("of %1 total").arg(s.total));
    m_failedCard->setValue(QString::number(s.failed), s.failed > 0 ? QStringLiteral("Needs attention") : QStringLiteral("All good"));
    m_dataCard->setValue(formatBytes(s.downloadedBytes), QStringLiteral("On disk across all downloads"));
}

void DashboardPage::sampleSpeed()
{
    m_speedHistory.append(m_manager->stats().speed);
    while (m_speedHistory.size() > kSpeedHistoryLength)
        m_speedHistory.removeFirst();
    m_speedCard->sparkline()->setValues(m_speedHistory);
    refreshStats();
    if (m_recentDirty)
        refreshRecent();
}

void DashboardPage::refreshRecent()
{
    m_recentDirty = false;
    struct Event {
        QDateTime when;
        QString text;
        TaskState state;
    };
    QVector<Event> events;
    for (const DownloadTask &t : m_manager->tasks()) {
        if (t.completedAt.isValid())
            events.append({ t.completedAt, QStringLiteral("Finished %1").arg(t.fileName), t.state });
        else if (t.state == TaskState::Failed)
            events.append({ t.startedAt.isValid() ? t.startedAt : t.createdAt, QStringLiteral("Failed %1").arg(t.fileName), t.state });
        else if (t.startedAt.isValid())
            events.append({ t.startedAt, QStringLiteral("Started %1").arg(t.fileName), t.state });
        else
            events.append({ t.createdAt, QStringLiteral("Added %1").arg(t.fileName), t.state });
    }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.when > b.when; });

    m_recent->clear();
    if (events.isEmpty()) {
        auto *item = new QListWidgetItem(QStringLiteral("Nothing yet. Activity appears here as downloads start and finish."));
        item->setForeground(Theme::current().textMuted);
        m_recent->addItem(item);
        return;
    }
    for (int i = 0; i < events.size() && i < 8; ++i) {
        const Event &e = events[i];
        const QString icon = e.state == TaskState::Completed ? QStringLiteral("check")
                           : e.state == TaskState::Failed   ? QStringLiteral("alert")
                                                            : QStringLiteral("clock");
        auto *item = new QListWidgetItem(Theme::icon(icon, Theme::stateColor(e.state)),
                                         e.text + QStringLiteral("\n") + QLocale().toString(e.when, QStringLiteral("d MMM, HH:mm")));
        m_recent->addItem(item);
    }
}

void DashboardPage::updateEmptyState()
{
    const bool noTasks = m_model->rowCount() == 0;
    const bool noMatches = !noTasks && m_filter->rowCount() == 0;
    if (noTasks) {
        m_empty->setMessage(QStringLiteral("No downloads yet"),
                            QStringLiteral("Add a link to start. Haul resumes interrupted downloads, uses several "
                                           "connections when the server allows it, and keeps your history."));
    } else if (noMatches) {
        m_empty->setMessage(QStringLiteral("Nothing matches"), QStringLiteral("Try a different search or filter."));
    }
    m_tableStack->setCurrentWidget(noTasks || noMatches ? static_cast<QWidget *>(m_empty) : m_table);
}

} // namespace haul::ui
