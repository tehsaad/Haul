#pragma once

// Small reusable building blocks of the Haul design system.

#include "core/DownloadTask.h"

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVector>

class QVBoxLayout;

namespace haul::ui {

// Factory helpers so every page uses the same styles (see Theme::styleSheet).
QLabel *makeLabel(const QString &text, const QString &role = QString(), QWidget *parent = nullptr);
QPushButton *makeButton(const QString &text, const QString &iconName = QString(),
                        const QString &variant = QString(), QWidget *parent = nullptr);
QFrame *makeCard(QWidget *parent = nullptr);
QFrame *makeSeparator(QWidget *parent = nullptr);
void setRole(QWidget *widget, const QString &role);

// A tiny line chart of recent values (used for the live speed history).
class Sparkline : public QWidget
{
    Q_OBJECT
public:
    explicit Sparkline(QWidget *parent = nullptr);
    void setValues(const QVector<double> &values);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<double> m_values;
};

// Short message shown instead of an empty table.
class EmptyState : public QWidget
{
    Q_OBJECT
public:
    EmptyState(const QString &title, const QString &message,
               QWidget *parent = nullptr);
    QPushButton *addAction(const QString &text, const QString &iconName, const QString &variant);
    void setMessage(const QString &title, const QString &message);
    void setActionVisible(bool visible);

private:
    QLabel *m_title;
    QLabel *m_message;
    QVBoxLayout *m_layout;
    QVector<QPushButton *> m_actions;
};

// Coloured pill showing a download state.
class StatusBadge : public QLabel
{
    Q_OBJECT
public:
    explicit StatusBadge(QWidget *parent = nullptr);
    void setState(TaskState state, const QString &text = QString());
};

// Visual map of a segmented download: one bar per segment, filled as it progresses.
class SegmentBar : public QWidget
{
    Q_OBJECT
public:
    explicit SegmentBar(QWidget *parent = nullptr);
    void setSegments(const QVector<SegmentInfo> &segments, qint64 totalBytes);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<SegmentInfo> m_segments;
    qint64 m_total = 0;
};

// Stack of auto-hiding notifications in the bottom-right corner of its parent.
class ToastHost : public QObject
{
    Q_OBJECT
public:
    explicit ToastHost(QWidget *parent);
    void show(const QString &title, const QString &message, bool isError);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void relayout();
    QWidget *m_parent;
    QVector<QFrame *> m_toasts;
};

} // namespace haul::ui
