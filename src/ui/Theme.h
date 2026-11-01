#pragma once

#include "core/TaskState.h"

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace haul::ui {

// Design tokens: every colour used by the interface comes from here.
struct Palette {
    QColor window;     // page background
    QColor sidebar;
    QColor surface;    // cards, tables, dialogs
    QColor surfaceAlt; // inputs, hovered rows
    QColor border;
    QColor text;
    QColor textMuted;
    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor onAccent;
    QColor success;
    QColor warning;
    QColor danger;
    QColor info;
    QColor track;      // empty part of progress bars
};

class Theme
{
public:
    static Palette dark();
    static Palette light();

    // Applies the palette and the stylesheet to the whole application.
    static void apply(QApplication &app, const QString &name);
    static const Palette &current();
    static bool isDark();

    static QString styleSheet(const Palette &p);

    // Line icon from :/icons/<name>.svg, tinted with `color` (defaults to the muted text colour).
    static QIcon icon(const QString &name, const QColor &color = QColor());

    // Badge colour for a download state.
    static QColor stateColor(TaskState state);
};

} // namespace haul::ui
