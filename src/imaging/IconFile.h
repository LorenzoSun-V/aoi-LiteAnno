#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>

namespace liteanno::imaging {

// The logo made square: a wider or taller picture is centred on a transparent square.
QImage squareLogo(const QImage& logo);

// The sizes Windows asks an .ico for (title bar, task bar, Explorer views).
QList<int> windowsIconSizes();

// A Windows .ico of `logo` (made square) at windowsIconSizes(), each size a PNG entry
// (Windows Vista and later read those). Empty if `logo` is null.
QByteArray windowsIcon(const QImage& logo);

} // namespace liteanno::imaging
