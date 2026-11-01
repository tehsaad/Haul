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
    p.window = QColor("#1F1E1C");
    p.sidebar = QColor("#191816");
    p.surface = QColor("#242321");
    p.surfaceAlt = QColor("#2E2C29");
    p.border = QColor("#393633");
    p.text = QColor("#E6E2DA");
    p.textMuted = QColor("#9B958B");
    p.accent = QColor("#CF8A42");
    p.accentHover = QColor("#DB9A55");
    p.accentPressed = QColor("#B97835");
    p.onAccent = QColor("#1B1A18");
    p.success = QColor("#8DAE76");
    p.warning = QColor("#CDAA4C");
    p.danger = QColor("#D06F5C");
    p.info = QColor("#7FA3B3");
    p.track = QColor("#35322E");
    return p;
}

Palette Theme::light()
{
    Palette p;
    p.window = QColor("#F4F1EB");
    p.sidebar = QColor("#EBE7DE");
    p.surface = QColor("#FBF9F5");
    p.surfaceAlt = QColor("#EFEBE3");
    p.border = QColor("#D8D2C6");
    p.text = QColor("#2A2824");
    p.textMuted = QColor("#6F685D");
    p.accent = QColor("#A85F1E");
    p.accentHover = QColor("#B96E2C");
    p.accentPressed = QColor("#924F15");
    p.onAccent = QColor("#FFFFFF");
    p.success = QColor("#4F7A3F");
    p.warning = QColor("#8F7212");
    p.danger = QColor("#A8402F");
    p.info = QColor("#3F6F85");
    p.track = QColor("#E2DDD3");
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
QMainWindow, QDialog, #Page { background: @window; }
QToolTip { background: @surfaceAlt; color: @text; border: 1px solid @border; padding: 4px 6px; }

/* ---------- side list ---------- */
#Sidebar { background: @sidebar; border-right: 1px solid @border; }
#BrandName { font-size: 12pt; font-weight: 600; }

/* ---------- bars ---------- */
#PageBar { background: @window; border-bottom: 1px solid @border; }
QStatusBar { background: @sidebar; border-top: 1px solid @border; color: @textMuted; }
QStatusBar::item { border: none; }
QLabel#StatusSpeed { color: @text; font-weight: 600; }

/* ---------- typography ---------- */
QLabel[role="title"] { font-size: 13pt; font-weight: 600; }
QLabel[role="subtitle"] { color: @textMuted; }
QLabel[role="h2"] { font-weight: 600; }
QLabel[role="muted"] { color: @textMuted; }
QLabel[role="caption"] { color: @textMuted; }
QLabel[role="statValue"] { font-size: 13pt; font-weight: 600; }
QLabel[role="error"] { color: @danger; }
QLabel[role="warning"] { color: @warning; }
QLabel[role="success"] { color: @success; }
QLabel[role="mono"] { font-family: monospace; }

/* ---------- grouped sections ---------- */
QFrame#Card { background: @surface; border: 1px solid @border; border-radius: 3px; }
QFrame#Separator { background: @border; max-height: 1px; min-height: 1px; border: none; }

/* ---------- buttons ---------- */
QPushButton {
    background: @surfaceAlt; border: 1px solid @border; border-radius: 3px;
    padding: 4px 10px; min-height: 18px;
}
QPushButton:hover { border-color: @textMuted; }
QPushButton:pressed { background: @border; }
QPushButton:disabled { color: @textMuted; background: transparent; }
QPushButton:focus { border-color: @accent; }
QPushButton[variant="primary"] { background: @accent; border: 1px solid @accentPressed; color: @onAccent; font-weight: 600; }
QPushButton[variant="primary"]:hover { background: @accentHover; }
QPushButton[variant="primary"]:pressed { background: @accentPressed; }
QPushButton[variant="primary"]:disabled { background: @track; border-color: @track; color: @textMuted; }
QPushButton[variant="ghost"] { background: transparent; border: 1px solid transparent; }
QPushButton[variant="ghost"]:hover { background: @surfaceAlt; border-color: @border; }
QPushButton[variant="danger"] { color: @danger; }
QPushButton[variant="danger"]:hover { border-color: @danger; }
QPushButton#NavButton {
    background: transparent; border: none; border-left: 2px solid transparent; border-radius: 0;
    padding: 6px 10px; text-align: left; color: @textMuted;
}
QPushButton#NavButton:hover { color: @text; }
QPushButton#NavButton:checked { background: @surfaceAlt; border-left: 2px solid @accent; color: @text; }
QToolButton#Disclosure { background: transparent; border: none; color: @textMuted; padding: 2px 0; }
QToolButton#Disclosure:hover { color: @text; }

/* ---------- inputs ---------- */
QLineEdit, QSpinBox, QTimeEdit, QComboBox, QPlainTextEdit {
    background: @surface; border: 1px solid @border; border-radius: 3px;
    padding: 3px 6px; selection-background-color: @accent; selection-color: @onAccent;
}
QLineEdit:focus, QSpinBox:focus, QTimeEdit:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QSpinBox:disabled, QTimeEdit:disabled, QComboBox:disabled { color: @textMuted; }
QLineEdit[invalid="true"] { border-color: @danger; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: @surface; border: 1px solid @border; selection-background-color: @surfaceAlt; selection-color: @text; }
QSpinBox::up-button, QSpinBox::down-button, QTimeEdit::up-button, QTimeEdit::down-button { width: 14px; border: none; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 13px; height: 13px; border-radius: 2px; border: 1px solid @textMuted; background: @surface; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/icons/check-white.svg); }
QCheckBox:disabled { color: @textMuted; }

/* ---------- tables ---------- */
QTableView {
    background: @surface; border: none; gridline-color: transparent;
    selection-background-color: @surfaceAlt; selection-color: @text; alternate-background-color: @surface;
}
QTableView::item { padding: 0 6px; border-bottom: 1px solid @surfaceAlt; }
QTableView::item:selected { background: @surfaceAlt; }
QHeaderView { background: @window; }
QHeaderView::section {
    background: @window; color: @textMuted; border: none;
    border-bottom: 1px solid @border; border-right: 1px solid @border; padding: 3px 6px;
}
QTableCornerButton::section { background: @window; border: none; }
QListWidget { background: transparent; border: none; }
QListWidget::item { padding: 1px 0; }

/* ---------- tabs ---------- */
QTabBar::tab {
    background: transparent; color: @textMuted; padding: 5px 10px;
    border: none; border-bottom: 2px solid transparent;
}
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QTabBar::tab:hover { color: @text; }

/* ---------- progress ---------- */
QProgressBar { background: @track; border: none; border-radius: 1px; max-height: 6px; min-height: 6px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @accent; border-radius: 1px; }
QProgressBar[state="completed"]::chunk { background: @success; }
QProgressBar[state="failed"]::chunk { background: @danger; }
QProgressBar[state="paused"]::chunk { background: @warning; }

/* ---------- menus ---------- */
QMenu { background: @surface; border: 1px solid @border; padding: 3px 0; }
QMenu::item { padding: 4px 22px 4px 10px; }
QMenu::item:selected { background: @surfaceAlt; }
QMenu::item:disabled { color: @textMuted; }
QMenu::separator { height: 1px; background: @border; margin: 3px 0; }
QMenu::icon { padding-left: 6px; }

/* ---------- scrollbars ---------- */
QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 9px; }
QScrollBar::handle:vertical { background: @border; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @textMuted; }
QScrollBar:horizontal { background: transparent; height: 9px; }
QScrollBar::handle:horizontal { background: @border; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---------- notifications ---------- */
QFrame#Toast { background: @surfaceAlt; border: 1px solid @border; border-radius: 3px; }
QFrame#Toast[kind="error"] { border-left: 2px solid @danger; }
QFrame#Toast[kind="info"] { border-left: 2px solid @accent; }
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
