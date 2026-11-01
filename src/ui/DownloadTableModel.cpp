#include "DownloadTableModel.h"

#include "Theme.h"
#include "app/DownloadManager.h"
#include "core/FileNameUtils.h"
#include "core/Formatters.h"

#include <QFileInfo>
#include <QLocale>
#include <QPainter>
#include <QApplication>
#include <QPainterPath>
#include <QStyle>

#include <algorithm>
#include <limits>

namespace haul::ui {

DownloadTableModel::DownloadTableModel(DownloadManager *manager, QObject *parent)
    : QAbstractTableModel(parent)
    , m_manager(manager)
{
    for (const DownloadTask &task : manager->tasks())
        m_ids.append(task.id);
    connect(manager, &DownloadManager::taskAdded, this, &DownloadTableModel::onAdded);
    connect(manager, &DownloadManager::taskUpdated, this, &DownloadTableModel::onUpdated);
    connect(manager, &DownloadManager::taskRemoved, this, &DownloadTableModel::onRemoved);
}

int DownloadTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_ids.size();
}

int DownloadTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

qint64 DownloadTableModel::taskIdAt(int row) const
{
    return row >= 0 && row < m_ids.size() ? m_ids.at(row) : 0;
}

int DownloadTableModel::rowOf(qint64 id) const
{
    return m_ids.indexOf(id);
}

void DownloadTableModel::onAdded(qint64 id)
{
    if (m_ids.contains(id))
        return;
    beginInsertRows({}, m_ids.size(), m_ids.size());
    m_ids.append(id);
    endInsertRows();
}

void DownloadTableModel::onUpdated(qint64 id)
{
    const int row = rowOf(id);
    if (row >= 0)
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void DownloadTableModel::onRemoved(qint64 id)
{
    const int row = rowOf(id);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_ids.removeAt(row);
    endRemoveRows();
}

QVariant DownloadTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return QStringLiteral("Name");
    case Status: return QStringLiteral("Status");
    case Progress: return QStringLiteral("Progress");
    case Size: return QStringLiteral("Size");
    case Speed: return QStringLiteral("Speed");
    case Eta: return QStringLiteral("Time left");
    case Category: return QStringLiteral("Category");
    case Added: return QStringLiteral("Added");
    default: return {};
    }
}

static QString statusText(const DownloadTask &t)
{
    if (t.state == TaskState::Queued && t.retryAt.isValid())
        return QStringLiteral("Retrying");
    if (t.state == TaskState::Paused && t.interrupted)
        return QStringLiteral("Interrupted");
    if (t.state == TaskState::Completed && t.checksumStatus == ChecksumStatus::Verified)
        return QStringLiteral("Verified");
    if (t.state == TaskState::Completed && t.checksumStatus == ChecksumStatus::Mismatch)
        return QStringLiteral("Bad checksum");
    return stateLabel(t.state);
}

QVariant DownloadTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const DownloadTask *t = m_manager->task(taskIdAt(index.row()));
    if (!t)
        return {};

    const qint64 eta = t->state == TaskState::Downloading
        ? estimateRemainingSeconds(t->downloadedBytes, t->totalBytes, t->speed) : -1;

    switch (role) {
    case TaskIdRole: return t->id;
    case StateRole: return static_cast<int>(t->state);
    case PercentRole: return t->progressPercent();
    case PriorityRole: return static_cast<int>(t->priority);
    case QueuePositionRole: return t->queuePosition;
    case SearchTextRole: return t->fileName + QLatin1Char(' ') + t->url;
    case Qt::ToolTipRole:
        if (index.column() == Status && !t->errorMessage.isEmpty())
            return t->errorMessage;
        if (index.column() == Name)
            return t->finalPath();
        return {};
    case Qt::DecorationRole:
        if (index.column() == Name) {
            QString icon = QStringLiteral("download");
            if (t->state == TaskState::Completed)
                icon = QStringLiteral("check");
            else if (t->state == TaskState::Failed)
                icon = QStringLiteral("alert");
            return Theme::icon(icon, Theme::stateColor(t->state));
        }
        return {};
    case Qt::TextAlignmentRole:
        if (index.column() == Size || index.column() == Speed || index.column() == Eta)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ForegroundRole:
        if (index.column() != Name && index.column() != Status)
            return Theme::current().textMuted;
        return {};
    case SortRole:
        switch (index.column()) {
        case Name: return t->fileName.toLower();
        case Status: return static_cast<int>(t->state);
        case Progress: return t->progressPercent();
        case Size: return t->totalBytes;
        case Speed: return t->speed;
        case Eta: return eta < 0 ? std::numeric_limits<qint64>::max() : eta;
        case Category: return categoryForFileName(t->fileName);
        case Added: return t->createdAt;
        default: return {};
        }
    case Qt::DisplayRole:
        switch (index.column()) {
        case Name: return t->fileName;
        case Status: return statusText(*t);
        case Progress: {
            const int p = t->progressPercent();
            return p < 0 ? QStringLiteral("—") : QStringLiteral("%1%").arg(p);
        }
        case Size:
            if (t->totalBytes < 0)
                return t->downloadedBytes > 0 ? formatBytes(t->downloadedBytes) + QStringLiteral(" / ?") : QStringLiteral("Unknown");
            if (t->state == TaskState::Completed)
                return formatBytes(t->totalBytes);
            return formatBytes(t->downloadedBytes) + QStringLiteral(" / ") + formatBytes(t->totalBytes);
        case Speed:
            return t->state == TaskState::Downloading ? formatSpeed(t->speed) : QString();
        case Eta:
            return t->state == TaskState::Downloading ? formatDuration(eta) : QString();
        case Category: return categoryForFileName(t->fileName);
        case Added: return QLocale().toString(t->createdAt, QLocale::ShortFormat);
        default: return {};
        }
    default:
        return {};
    }
}

// ------------------------------------------------------------------ filter

DownloadFilterModel::DownloadFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(DownloadTableModel::SortRole);
    setDynamicSortFilter(true);
}

bool DownloadFilterModel::stateInGroup(TaskState state, Group group)
{
    switch (group) {
    case All: return true;
    case Active: return state == TaskState::Downloading;
    case Waiting: return state == TaskState::Queued;
    case Paused: return state == TaskState::Paused;
    case Completed: return state == TaskState::Completed;
    case Failed: return state == TaskState::Failed || state == TaskState::Cancelled;
    }
    return true;
}

void DownloadFilterModel::setGroup(Group group)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_group = group;
    endFilterChange();
#else
    m_group = group;
    invalidateFilter();
#endif
}

void DownloadFilterModel::setSearchText(const QString &text)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_search = text.trimmed();
    endFilterChange();
#else
    m_search = text.trimmed();
    invalidateFilter();
#endif
}

void DownloadFilterModel::setQueueOrder(bool enabled)
{
    m_queueOrder = enabled;
    invalidate();
    if (enabled)
        sort(0, Qt::AscendingOrder);
}

bool DownloadFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    const auto state = static_cast<TaskState>(index.data(DownloadTableModel::StateRole).toInt());
    if (!stateInGroup(state, m_group))
        return false;
    if (m_search.isEmpty())
        return true;
    return index.data(DownloadTableModel::SearchTextRole).toString().contains(m_search, Qt::CaseInsensitive);
}

bool DownloadFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    if (m_queueOrder) {
        const int lp = left.data(DownloadTableModel::PriorityRole).toInt();
        const int rp = right.data(DownloadTableModel::PriorityRole).toInt();
        if (lp != rp)
            return lp > rp;
        const int lpos = left.data(DownloadTableModel::QueuePositionRole).toInt();
        const int rpos = right.data(DownloadTableModel::QueuePositionRole).toInt();
        if (lpos != rpos)
            return lpos < rpos;
        return left.data(DownloadTableModel::TaskIdRole).toLongLong() < right.data(DownloadTableModel::TaskIdRole).toLongLong();
    }
    return QSortFilterProxyModel::lessThan(left, right);
}

// ------------------------------------------------------------------ delegate

QSize DownloadItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), 44));
    return size;
}

void DownloadItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int column = index.column();
    if (column != DownloadTableModel::Status && column != DownloadTableModel::Progress) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Background (selection/hover) via the style, then our custom content.
    QStyleOptionViewItem bg = option;
    initStyleOption(&bg, index);
    bg.text.clear();
    bg.icon = QIcon();
    const QStyle *style = option.widget ? option.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &bg, painter, option.widget);

    const Palette &p = Theme::current();
    const auto state = static_cast<TaskState>(index.data(DownloadTableModel::StateRole).toInt());
    const QColor color = Theme::stateColor(state);
    painter->save();

    if (column == DownloadTableModel::Status) {
        const QString text = index.data(Qt::DisplayRole).toString();
        painter->setFont(option.font);
        painter->setPen(color);
        painter->drawText(option.rect.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    } else {
        const int percent = index.data(DownloadTableModel::PercentRole).toInt();
        const QRect r = option.rect.adjusted(10, 0, -10, 0);
        const QString label = percent < 0 ? QStringLiteral("—") : QStringLiteral("%1%").arg(percent);
        QFont font = option.font;
        font.setPointSizeF(font.pointSizeF() * 0.9);
        painter->setFont(font);
        const int labelWidth = QFontMetrics(font).horizontalAdvance(QStringLiteral("100%")) + 6;
        const QRectF track(r.left(), r.center().y() - 3, std::max(10, r.width() - labelWidth), 6);
        QPainterPath trackPath;
        trackPath.addRect(track);
        painter->fillPath(trackPath, p.track);

        if (percent > 0) {
            QPainterPath fillPath;
            fillPath.addRect(QRectF(track.left(), track.top(), track.width() * percent / 100.0, track.height()));
            painter->fillPath(fillPath, state == TaskState::Downloading ? p.accent : color);
        } else if (percent < 0 && state == TaskState::Downloading) {
            // Unknown size: show an honest "activity" stripe instead of a fake percentage.
            QColor faded = p.accent;
            faded.setAlpha(120);
            QPainterPath fillPath;
            fillPath.addRect(QRectF(track.left(), track.top(), track.width() * 0.25, track.height()));
            painter->fillPath(fillPath, faded);
        }
        painter->setPen(p.textMuted);
        painter->drawText(QRectF(track.right() + 6, r.top(), labelWidth, r.height()), Qt::AlignVCenter | Qt::AlignRight, label);
    }
    painter->restore();
}

} // namespace haul::ui
