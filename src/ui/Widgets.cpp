#include "Widgets.h"

#include "Theme.h"
#include "core/Formatters.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace haul::ui {

void setRole(QWidget *widget, const QString &role)
{
    widget->setProperty("role", role);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QLabel *makeLabel(const QString &text, const QString &role, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    if (!role.isEmpty())
        label->setProperty("role", role);
    return label;
}

QPushButton *makeButton(const QString &text, const QString &iconName, const QString &variant, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    if (!variant.isEmpty())
        button->setProperty("variant", variant);
    if (!iconName.isEmpty()) {
        const QColor tint = variant == QLatin1String("primary") ? Theme::current().onAccent
                          : variant == QLatin1String("danger") ? Theme::current().danger
                                                               : Theme::current().text;
        button->setIcon(Theme::icon(iconName, tint));
        button->setIconSize(QSize(16, 16));
    }
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QFrame *makeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("Card"));
    return card;
}

QFrame *makeSeparator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setObjectName(QStringLiteral("Separator"));
    return line;
}

// ------------------------------------------------------------------ Sparkline

Sparkline::Sparkline(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(34);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Sparkline::setValues(const QVector<double> &values)
{
    m_values = values;
    update();
}

void Sparkline::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(1, 3, -1, -1);
    const QColor accent = Theme::current().accent;

    if (m_values.size() < 2) {
        painter.setPen(QPen(Theme::current().border, 1.5, Qt::DashLine));
        painter.drawLine(QPointF(area.left(), area.bottom()), QPointF(area.right(), area.bottom()));
        return;
    }

    const double peak = std::max(1.0, *std::max_element(m_values.begin(), m_values.end()));
    QPainterPath line;
    for (int i = 0; i < m_values.size(); ++i) {
        const double x = area.left() + area.width() * i / (m_values.size() - 1);
        const double y = area.bottom() - area.height() * (m_values[i] / peak);
        if (i == 0)
            line.moveTo(x, y);
        else
            line.lineTo(x, y);
    }
    QPainterPath fill = line;
    fill.lineTo(area.right(), area.bottom());
    fill.lineTo(area.left(), area.bottom());
    fill.closeSubpath();

    QLinearGradient gradient(area.topLeft(), area.bottomLeft());
    QColor top = accent;
    top.setAlpha(90);
    QColor bottom = accent;
    bottom.setAlpha(0);
    gradient.setColorAt(0, top);
    gradient.setColorAt(1, bottom);
    painter.fillPath(fill, gradient);
    painter.setPen(QPen(accent, 1.8));
    painter.drawPath(line);
}

// ------------------------------------------------------------------ StatCard

EmptyState::EmptyState(const QString &title, const QString &message, QWidget *parent)
    : QWidget(parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setAlignment(Qt::AlignCenter);
    m_layout->setSpacing(6);

    m_title = makeLabel(title, QStringLiteral("h2"), this);
    m_title->setAlignment(Qt::AlignCenter);
    m_message = makeLabel(message, QStringLiteral("muted"), this);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setMaximumWidth(420);

    m_layout->addWidget(m_title);
    m_layout->addWidget(m_message, 0, Qt::AlignHCenter);
}

QPushButton *EmptyState::addAction(const QString &text, const QString &iconName, const QString &variant)
{
    auto *button = makeButton(text, iconName, variant, this);
    m_layout->addWidget(button, 0, Qt::AlignHCenter);
    m_actions.append(button);
    return button;
}

void EmptyState::setMessage(const QString &title, const QString &message)
{
    m_title->setText(title);
    m_message->setText(message);
}

void EmptyState::setActionVisible(bool visible)
{
    for (QPushButton *button : std::as_const(m_actions))
        button->setVisible(visible);
}

// ------------------------------------------------------------------ StatusBadge

StatusBadge::StatusBadge(QWidget *parent)
    : QLabel(parent)
{
    setAlignment(Qt::AlignCenter);
}

void StatusBadge::setState(TaskState state, const QString &text)
{
    const QColor color = Theme::stateColor(state);
    QColor background = color;
    background.setAlpha(40);
    setText(text.isEmpty() ? stateLabel(state) : text);
    setStyleSheet(QStringLiteral("QLabel { color: %1; background: rgba(%2,%3,%4,%5); border-radius: 10px;"
                                 " padding: 3px 10px; font-weight: 600; font-size: 9pt; }")
                      .arg(color.name()).arg(background.red()).arg(background.green())
                      .arg(background.blue()).arg(background.alpha()));
}

// ------------------------------------------------------------------ SegmentBar

SegmentBar::SegmentBar(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(28);
}

QSize SegmentBar::sizeHint() const
{
    return QSize(400, 28);
}

void SegmentBar::setSegments(const QVector<SegmentInfo> &segments, qint64 totalBytes)
{
    m_segments = segments;
    m_total = totalBytes;
    update();
}

void SegmentBar::paintEvent(QPaintEvent *)
{
    if (m_segments.isEmpty() || m_total <= 0)
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Palette &p = Theme::current();
    const QRectF area = QRectF(rect()).adjusted(0, 4, 0, -4);
    const double gap = 3.0;
    const double usable = area.width() - gap * (m_segments.size() - 1);

    double x = area.left();
    for (const SegmentInfo &s : m_segments) {
        const double width = std::max(4.0, usable * double(s.length()) / double(m_total));
        const QRectF box(x, area.top(), width, area.height());
        QPainterPath path;
        path.addRoundedRect(box, 4, 4);
        painter.fillPath(path, p.track);
        const double fraction = s.length() > 0 ? double(std::min(s.downloaded, s.length())) / double(s.length()) : 0.0;
        if (fraction > 0) {
            QPainterPath done;
            done.addRoundedRect(QRectF(box.left(), box.top(), box.width() * fraction, box.height()), 4, 4);
            painter.fillPath(done, s.isComplete() ? p.success : p.accent);
        }
        x += width + gap;
    }
}

// ------------------------------------------------------------------ ToastHost

ToastHost::ToastHost(QWidget *parent)
    : QObject(parent)
    , m_parent(parent)
{
    parent->installEventFilter(this);
}

void ToastHost::show(const QString &title, const QString &message, bool isError)
{
    auto *toast = new QFrame(m_parent);
    toast->setObjectName(QStringLiteral("Toast"));
    toast->setProperty("kind", isError ? "error" : "info");
    toast->setFixedWidth(340);
    auto *layout = new QHBoxLayout(toast);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);

    auto *icon = new QLabel(toast);
    icon->setPixmap(Theme::icon(isError ? QStringLiteral("alert") : QStringLiteral("check"),
                                isError ? Theme::current().danger : Theme::current().success).pixmap(20, 20));
    icon->setAlignment(Qt::AlignTop);
    layout->addWidget(icon);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(2);
    auto *titleLabel = new QLabel(title, toast);
    titleLabel->setObjectName(QStringLiteral("ToastTitle"));
    auto *messageLabel = makeLabel(message, QStringLiteral("muted"), toast);
    messageLabel->setWordWrap(true);
    texts->addWidget(titleLabel);
    texts->addWidget(messageLabel);
    layout->addLayout(texts, 1);

    toast->adjustSize();
    toast->show();
    toast->raise();
    m_toasts.append(toast);
    while (m_toasts.size() > 4)
        delete m_toasts.takeFirst();
    relayout();

    QTimer::singleShot(isError ? 7000 : 4500, toast, [this, toast] {
        m_toasts.removeAll(toast);
        toast->deleteLater();
        relayout();
    });
}

bool ToastHost::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_parent && event->type() == QEvent::Resize)
        relayout();
    return QObject::eventFilter(watched, event);
}

void ToastHost::relayout()
{
    int y = m_parent->height() - 20;
    for (int i = m_toasts.size() - 1; i >= 0; --i) {
        QFrame *toast = m_toasts[i];
        y -= toast->height();
        toast->move(m_parent->width() - toast->width() - 20, y);
        toast->raise();
        y -= 10;
    }
}

} // namespace haul::ui
