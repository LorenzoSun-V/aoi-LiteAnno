#pragma once

#include <QIcon>
#include <QString>

class QApplication;

namespace liteanno::ui {

// The look of the program: the Fusion style, a light palette and the style sheet
// theme/light.qss. Widgets set object names and the properties "role" and "tone" for it.
void applyTheme(QApplication& app);
QString themeStyleSheet();

// An icon from icons/png: `name` in the text colour, `name`-on (the accent colour) when a
// checkable button is on.
QIcon themeIcon(const QString& name);
// One colour variant only, e.g. "check-white".
QIcon themeIconVariant(const QString& file);

} // namespace liteanno::ui
