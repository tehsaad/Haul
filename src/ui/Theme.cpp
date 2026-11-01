#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>

// The icons live in a static library, so their resource must be registered explicitly.
static void initHaulResources()
{
    Q_INIT_RESOURCE(haul);
}

namespace haul::ui {

namespace {
Palette g_current = Theme::dark();
bool g_dark = true;
QHash<QString, QIcon> g_iconCache;
} // namespace

Palette Theme::dark()
{
    Palette p;
    p.window = QColor("#111318");
    p.sidebar = QColor("#0C0E12");
    p.surface = QColor("#171A21");
    p.surfaceAlt = QColor("#1F232C");
    p.border = QColor("#2A2F3A");
    p.text = QColor("#E7E9EE");
    p.textMuted = QColor("#8D92A3");
    p.accent = QColor("#7C6CF2");
    p.accentHover = QColor("#8F81F5");
    p.accentPressed = QColor("#6A59E8");
    p.onAccent = QColor("#FFFFFF");
    p.success = QColor("#3FB68B");
    p.warning = QColor("#E5A54B");
    p.danger = QColor("#E5626A");
    p.info = QColor("#5B9BF0");
    p.track = QColor("#262B35");
    return p;
}

Palette Theme::light()
{
    Palette p;
    p.window = QColor("#F4F5F8");
    p.sidebar = QColor("#FFFFFF");
    p.surface = QColor("#FFFFFF");
    p.surfaceAlt = QColor("#F0F1F5");
    p.border = QColor("#E0E3EA");
    p.text = QColor("#1B1E25");
    p.textMuted = QColor("#626978");
    p.accent = QColor("#6A59E8");
    p.accentHover = QColor("#7C6CF2");
    p.accentPressed = QColor("#5847D6");
    p.onAccent = QColor("#FFFFFF");
    p.success = QColor("#22946B");
    p.warning = QColor("#C07F1F");
    p.danger = QColor("#D14751");
    p.info = QColor("#3478D6");
    p.track = QColor("#E6E8EE");
    return p;
}

const Palette &Theme::current()
{
    return g_current;
}

bool Theme::isDark()
{
    return g_dark;
}

QColor Theme::stateColor(TaskState state)
{
    const Palette &p = g_current;
    switch (state) {
    case TaskState::Downloading: return p.accent;
    case TaskState::Queued: return p.info;
    case TaskState::Paused: return p.warning;
    case TaskState::Completed: return p.success;
    case TaskState::Failed: return p.danger;
    case TaskState::Cancelled: return p.textMuted;
    }
    return p.textMuted;
}

QIcon Theme::icon(const QString &name, const QColor &color)
{
    const QColor tint = color.isValid() ? color : g_current.textMuted;
    const QString key = name + tint.name(QColor::HexArgb);
    if (g_iconCache.contains(key))
        return g_iconCache.value(key);

    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly))
        return QIcon();
    QByteArray svg = file.readAll();
    svg.replace("currentColor", tint.name(QColor::HexRgb).toLatin1());

    QSvgRenderer renderer(svg);
    QIcon icon;
    for (int size : { 16, 20, 24, 32, 48 }) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter);
        painter.end();
        icon.addPixmap(pixmap);
    }
    g_iconCache.insert(key, icon);
    return icon;
}

void Theme::apply(QApplication &app, const QString &name)
{
    initHaulResources();
    g_dark = name != QLatin1String("light");
    g_current = g_dark ? dark() : light();
    g_iconCache.clear();
    const Palette &p = g_current;

    QPalette qp;
    qp.setColor(QPalette::Window, p.window);
    qp.setColor(QPalette::WindowText, p.text);
    qp.setColor(QPalette::Base, p.surface);
    qp.setColor(QPalette::AlternateBase, p.surfaceAlt);
    qp.setColor(QPalette::Text, p.text);
    qp.setColor(QPalette::Button, p.surfaceAlt);
    qp.setColor(QPalette::ButtonText, p.text);
    qp.setColor(QPalette::Highlight, p.accent);
    qp.setColor(QPalette::HighlightedText, p.onAccent);
    qp.setColor(QPalette::ToolTipBase, p.surfaceAlt);
    qp.setColor(QPalette::ToolTipText, p.text);
    qp.setColor(QPalette::PlaceholderText, p.textMuted);
    qp.setColor(QPalette::Link, p.accent);
    qp.setColor(QPalette::Disabled, QPalette::Text, p.textMuted);
    qp.setColor(QPalette::Disabled, QPalette::ButtonText, p.textMuted);
    qp.setColor(QPalette::Disabled, QPalette::WindowText, p.textMuted);
    app.setPalette(qp);
    app.setStyleSheet(styleSheet(p));
}

QString Theme::styleSheet(const Palette &p)
{
    QString css = QStringLiteral(R"(
* { outline: none; }
QWidget { color: @text; font-size: 10pt; }
QMainWindow, QDialog, #Page { background: @window; }
QToolTip { background: @surfaceAlt; color: @text; border: 1px solid @border; padding: 6px 8px; border-radius: 6px; }

/* ---------- sidebar ---------- */
#Sidebar { background: @sidebar; border-right: 1px solid @border; }
#BrandName { font-size: 15pt; font-weight: 700; }
#BrandTag { color: @textMuted; font-size: 8.5pt; }
QToolButton#NavButton {
    background: transparent; border: none; border-radius: 8px;
    padding: 9px 12px; text-align: left; color: @textMuted; font-weight: 500;
}
QToolButton#NavButton:hover { background: @surfaceAlt; color: @text; }
QToolButton#NavButton:checked { background: @accentSoft; color: @text; font-weight: 600; }
#SidebarFooter { color: @textMuted; font-size: 9pt; }
#SidebarSpeed { font-size: 13pt; font-weight: 700; }

/* ---------- typography ---------- */
QLabel[role="title"] { font-size: 18pt; font-weight: 700; }
QLabel[role="subtitle"] { color: @textMuted; }
QLabel[role="h2"] { font-size: 11.5pt; font-weight: 600; }
QLabel[role="muted"] { color: @textMuted; }
QLabel[role="caption"] { color: @textMuted; font-size: 8.5pt; font-weight: 600; letter-spacing: 0.5px; }
QLabel[role="value"] { font-weight: 500; }
QLabel[role="statValue"] { font-size: 19pt; font-weight: 700; }
QLabel[role="error"] { color: @danger; }
QLabel[role="warning"] { color: @warning; }
QLabel[role="success"] { color: @success; }
QLabel[role="mono"] { font-family: monospace; }

/* ---------- cards ---------- */
QFrame#Card { background: @surface; border: 1px solid @border; border-radius: 12px; }
QFrame#Separator { background: @border; max-height: 1px; min-height: 1px; border: none; }

/* ---------- buttons ---------- */
QPushButton {
    background: @surfaceAlt; border: 1px solid @border; border-radius: 8px;
    padding: 7px 14px; font-weight: 500; min-height: 18px;
}
QPushButton:hover { border-color: @textMuted; }
QPushButton:pressed { background: @border; }
QPushButton:disabled { color: @textMuted; background: @surface; border-color: @border; }
QPushButton:focus { border-color: @accent; }
QPushButton[variant="primary"] { background: @accent; border: 1px solid @accent; color: @onAccent; font-weight: 600; }
QPushButton[variant="primary"]:hover { background: @accentHover; border-color: @accentHover; }
QPushButton[variant="primary"]:pressed { background: @accentPressed; }
QPushButton[variant="primary"]:disabled { background: @track; border-color: @track; color: @textMuted; }
QPushButton[variant="ghost"] { background: transparent; border: 1px solid transparent; }
QPushButton[variant="ghost"]:hover { background: @surfaceAlt; border-color: @border; }
QPushButton[variant="danger"] { color: @danger; }
QPushButton[variant="danger"]:hover { border-color: @danger; }

/* ---------- inputs ---------- */
QLineEdit, QSpinBox, QTimeEdit, QComboBox, QPlainTextEdit {
    background: @surfaceAlt; border: 1px solid @border; border-radius: 8px;
    padding: 6px 10px; selection-background-color: @accent; selection-color: @onAccent;
}
QLineEdit:focus, QSpinBox:focus, QTimeEdit:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QSpinBox:disabled, QTimeEdit:disabled, QComboBox:disabled { color: @textMuted; }
QLineEdit[invalid="true"] { border-color: @danger; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: @surface; border: 1px solid @border; selection-background-color: @accentSoft; selection-color: @text; padding: 4px; }
QSpinBox::up-button, QSpinBox::down-button, QTimeEdit::up-button, QTimeEdit::down-button { width: 16px; border: none; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px; border: 1px solid @textMuted; background: @surfaceAlt; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/icons/check-white.svg); }
QCheckBox:disabled { color: @textMuted; }

/* ---------- tables ---------- */
QTableView {
    background: @surface; border: none; gridline-color: transparent;
    selection-background-color: @accentSoft; selection-color: @text; alternate-background-color: @surface;
}
QTableView::item { padding: 0 8px; border-bottom: 1px solid @border; }
QTableView::item:hover { background: @surfaceAlt; }
QTableView::item:selected { background: @accentSoft; }
QHeaderView { background: @surface; }
QHeaderView::section {
    background: @surface; color: @textMuted; border: none; border-bottom: 1px solid @border;
    padding: 8px; font-size: 8.5pt; font-weight: 600; text-transform: uppercase;
}
QTableCornerButton::section { background: @surface; border: none; }
QListWidget { background: transparent; border: none; }
QListWidget::item { padding: 6px 2px; border-bottom: 1px solid @border; }

/* ---------- tabs ---------- */
QTabBar::tab {
    background: transparent; color: @textMuted; padding: 8px 14px; margin-right: 4px;
    border: none; border-bottom: 2px solid transparent; font-weight: 500;
}
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QTabBar::tab:hover { color: @text; }

/* ---------- progress ---------- */
QProgressBar { background: @track; border: none; border-radius: 4px; max-height: 8px; min-height: 8px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @accent; border-radius: 4px; }
QProgressBar[state="completed"]::chunk { background: @success; }
QProgressBar[state="failed"]::chunk { background: @danger; }
QProgressBar[state="paused"]::chunk { background: @warning; }

/* ---------- menus ---------- */
QMenu { background: @surface; border: 1px solid @border; border-radius: 8px; padding: 6px; }
QMenu::item { padding: 7px 26px 7px 12px; border-radius: 6px; }
QMenu::item:selected { background: @surfaceAlt; }
QMenu::item:disabled { color: @textMuted; }
QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }
QMenu::icon { padding-left: 6px; }

/* ---------- scrollbars ---------- */
QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @border; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @textMuted; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @border; border-radius: 4px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---------- toast ---------- */
QFrame#Toast { background: @surfaceAlt; border: 1px solid @border; border-radius: 10px; }
QFrame#Toast[kind="error"] { border-left: 3px solid @danger; }
QFrame#Toast[kind="info"] { border-left: 3px solid @accent; }
QLabel#ToastTitle { font-weight: 600; }

QMessageBox { background: @surface; }
)");

    QColor accentSoft = p.accent;
    accentSoft.setAlpha(g_dark ? 46 : 30);
    const QString soft = QStringLiteral("rgba(%1,%2,%3,%4)").arg(accentSoft.red()).arg(accentSoft.green())
                             .arg(accentSoft.blue()).arg(accentSoft.alpha());

    // Longest names first so "@surfaceAlt" is not replaced as "@surface" + "Alt".
    const QList<QPair<QString, QString>> tokens = {
        { "@accentPressed", p.accentPressed.name() }, { "@accentHover", p.accentHover.name() },
        { "@accentSoft", soft }, { "@accent", p.accent.name() }, { "@onAccent", p.onAccent.name() },
        { "@surfaceAlt", p.surfaceAlt.name() }, { "@surface", p.surface.name() },
        { "@textMuted", p.textMuted.name() }, { "@text", p.text.name() },
        { "@window", p.window.name() }, { "@sidebar", p.sidebar.name() }, { "@border", p.border.name() },
        { "@success", p.success.name() }, { "@warning", p.warning.name() }, { "@danger", p.danger.name() },
        { "@track", p.track.name() },
    };
    for (const auto &token : tokens)
        css.replace(token.first, token.second);
    return css;
}

} // namespace haul::ui
