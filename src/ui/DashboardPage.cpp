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
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Toolbar
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PageBar"));
    bar->setAttribute(Qt::WA_StyledBackground);
    auto *toolbar = new QHBoxLayout(bar);
    toolbar->setContentsMargins(10, 7, 10, 7);
    toolbar->setSpacing(6);
    auto *add = makeButton(QStringLiteral("Add…"), QStringLiteral("plus"), QStringLiteral("primary"), bar);
    add->setToolTip(QStringLiteral("Ctrl+N"));
    auto *resumeAll = makeButton(QStringLiteral("Resume all"), QStringLiteral("play"), QString(), bar);
    auto *pauseAll = makeButton(QStringLiteral("Pause all"), QStringLiteral("pause"), QString(), bar);
    m_search = new QLineEdit(bar);
    m_search->setPlaceholderText(QStringLiteral("Search"));
    m_search->setToolTip(QStringLiteral("Ctrl+F"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(220);
    m_group = new QComboBox(bar);
    m_group->addItem(QStringLiteral("All"), DownloadFilterModel::All);
    m_group->addItem(QStringLiteral("Downloading"), DownloadFilterModel::Active);
    m_group->addItem(QStringLiteral("Queued"), DownloadFilterModel::Waiting);
    m_group->addItem(QStringLiteral("Paused"), DownloadFilterModel::Paused);
    m_group->addItem(QStringLiteral("Finished"), DownloadFilterModel::Completed);
    m_group->addItem(QStringLiteral("Failed"), DownloadFilterModel::Failed);
    toolbar->addWidget(add);
    toolbar->addWidget(resumeAll);
    toolbar->addWidget(pauseAll);
    toolbar->addStretch();
    toolbar->addWidget(m_search, 1);
    toolbar->addWidget(m_group);
    root->addWidget(bar);

    auto *tableCard = this;

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
    m_table->verticalHeader()->setDefaultSectionSize(28);
    m_table->setIconSize(QSize(16, 16));
    QHeaderView *h = m_table->horizontalHeader();
    h->setHighlightSections(false);
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->setSectionResizeMode(QHeaderView::Interactive);
    h->setSectionResizeMode(DownloadTableModel::Name, QHeaderView::Stretch);
    h->setMinimumSectionSize(60);
    h->resizeSection(DownloadTableModel::Status, 104);
    h->resizeSection(DownloadTableModel::Progress, 110);
    h->resizeSection(DownloadTableModel::Size, 140);
    h->resizeSection(DownloadTableModel::Speed, 88);
    h->resizeSection(DownloadTableModel::Eta, 76);
    m_table->setTextElideMode(Qt::ElideMiddle);
    h->setSectionHidden(DownloadTableModel::Category, true);
    h->setSectionHidden(DownloadTableModel::Added, true);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_empty = new EmptyState(QString(), QString(), tableCard);
    auto *emptyAdd = m_empty->addAction(QStringLiteral("Add a link"), QString(), QString());

    m_tableStack = new QStackedWidget(tableCard);
    m_tableStack->addWidget(m_table);
    m_tableStack->addWidget(m_empty);
    root->addWidget(m_tableStack, 1);

    // Totals and recent activity, kept out of the way under the list.
    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("PageBar"));
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(10, 6, 10, 6);
    footerLayout->setSpacing(3);
    auto *totals = new QHBoxLayout;
    totals->setSpacing(10);
    m_summary = makeLabel(QString(), QStringLiteral("muted"), footer);
    m_speedGraph = new Sparkline(footer);
    m_speedGraph->setFixedSize(120, 18);
    totals->addWidget(m_summary, 1);
    totals->addWidget(m_speedGraph);
    footerLayout->addLayout(totals);
    m_recent = new QListWidget(footer);
    m_recent->setFocusPolicy(Qt::NoFocus);
    m_recent->setSelectionMode(QAbstractItemView::NoSelection);
    m_recent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recent->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recent->setTextElideMode(Qt::ElideMiddle);
    m_recent->setFixedHeight(60);
    footerLayout->addWidget(m_recent);
    root->addWidget(footer);

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
    QStringList parts;
    if (s.active > 0)
        parts << QStringLiteral("%1 downloading").arg(s.active);
    if (s.queued > 0)
        parts << QStringLiteral("%1 queued").arg(s.queued);
    if (s.paused > 0)
        parts << QStringLiteral("%1 paused").arg(s.paused);
    if (s.completed > 0)
        parts << QStringLiteral("%1 finished").arg(s.completed);
    if (s.failed > 0)
        parts << QStringLiteral("%1 failed").arg(s.failed);
    if (s.downloadedBytes > 0)
        parts << QStringLiteral("%1 in total").arg(formatBytes(s.downloadedBytes));
    m_summary->setText(parts.join(QStringLiteral(", ")));
}

void DashboardPage::sampleSpeed()
{
    m_speedHistory.append(m_manager->stats().speed);
    while (m_speedHistory.size() > kSpeedHistoryLength)
        m_speedHistory.removeFirst();
    m_speedGraph->setValues(m_speedHistory);
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
    };
    QVector<Event> events;
    for (const DownloadTask &t : m_manager->tasks()) {
        if (t.completedAt.isValid())
            events.append({ t.completedAt, QStringLiteral("Finished %1").arg(t.fileName) });
        else if (t.state == TaskState::Failed)
            events.append({ t.startedAt.isValid() ? t.startedAt : t.createdAt, QStringLiteral("Failed %1").arg(t.fileName) });
        else if (t.startedAt.isValid())
            events.append({ t.startedAt, QStringLiteral("Started %1").arg(t.fileName) });
        else
            events.append({ t.createdAt, QStringLiteral("Added %1").arg(t.fileName) });
    }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.when > b.when; });

    m_recent->clear();
    for (int i = 0; i < events.size() && i < 3; ++i) {
        auto *item = new QListWidgetItem(QLocale().toString(events[i].when, QStringLiteral("HH:mm")) + QStringLiteral("  ") + events[i].text);
        item->setForeground(Theme::current().textMuted);
        m_recent->addItem(item);
    }
    m_recent->setVisible(!events.isEmpty());
}

void DashboardPage::updateEmptyState()
{
    const bool noTasks = m_model->rowCount() == 0;
    const bool noMatches = !noTasks && m_filter->rowCount() == 0;
    if (noTasks)
        m_empty->setMessage(QStringLiteral("Nothing here yet"), QStringLiteral("Paste a link with Ctrl+V to get started."));
    else if (noMatches)
        m_empty->setMessage(QStringLiteral("No matches"), QString());
    m_empty->setActionVisible(noTasks);
    m_tableStack->setCurrentWidget(noTasks || noMatches ? static_cast<QWidget *>(m_empty) : m_table);
}

} // namespace haul::ui
