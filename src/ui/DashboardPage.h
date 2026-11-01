#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTableView;

namespace haul {
class DownloadManager;
}

namespace haul::ui {

class DownloadFilterModel;
class DownloadTableModel;
class EmptyState;
class Sparkline;
class TaskActions;

// Overview: live statistics, the main download table with search and filters, and recent activity.
class DashboardPage : public QWidget
{
    Q_OBJECT
public:
    DashboardPage(DownloadManager *manager, DownloadTableModel *model, TaskActions *actions, QWidget *parent = nullptr);

    void focusSearch();
    qint64 selectedTaskId() const;
    QTableView *table() const { return m_table; }

signals:
    void addRequested();

private:
    void refreshStats();
    void sampleSpeed();
    void refreshRecent();
    void updateEmptyState();

    DownloadManager *m_manager;
    DownloadTableModel *m_model;
    DownloadFilterModel *m_filter;
    TaskActions *m_actions;
    QLineEdit *m_search;
    QLabel *m_summary;
    Sparkline *m_speedGraph;
    QComboBox *m_group;
    QStackedWidget *m_tableStack;
    QTableView *m_table;
    EmptyState *m_empty;
    QListWidget *m_recent;
    QVector<double> m_speedHistory;
    bool m_recentDirty = true;
};

} // namespace haul::ui
