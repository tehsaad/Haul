#pragma once

#include "core/DownloadTask.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QVector>

namespace haul {
class DownloadManager;
}

namespace haul::ui {

// Exposes the DownloadManager's tasks as rows. Every value is read live from the manager;
// the model stores nothing but the row order of task ids.
class DownloadTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Name, Status, Progress, Size, Speed, Eta, Category, Added, ColumnCount };
    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        StateRole,
        PercentRole,   // int, -1 when unknown
        SortRole,
        PriorityRole,
        QueuePositionRole,
        SearchTextRole
    };

    explicit DownloadTableModel(DownloadManager *manager, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    qint64 taskIdAt(int row) const;
    int rowOf(qint64 id) const;

private:
    void onAdded(qint64 id);
    void onUpdated(qint64 id);
    void onRemoved(qint64 id);

    DownloadManager *m_manager;
    QVector<qint64> m_ids;
};

// Filters by state group and search text, and sorts by the column's SortRole.
class DownloadFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    enum Group { All, Active, Waiting, Paused, Completed, Failed };

    explicit DownloadFilterModel(QObject *parent = nullptr);
    void setGroup(Group group);
    void setSearchText(const QString &text);
    // Queue view: sort waiting tasks in their real start order (priority, then position).
    void setQueueOrder(bool enabled);

    static bool stateInGroup(TaskState state, Group group);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    Group m_group = All;
    QString m_search;
    bool m_queueOrder = false;
};

// Paints the Status column as a coloured pill and the Progress column as a bar.
class DownloadItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace haul::ui
