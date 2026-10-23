#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

#include <cmath>

namespace liteanno::ui {

namespace {

void addPng(QIcon& icon, const QString& file, QIcon::State state)
{
    icon.addFile(QStringLiteral(":/liteanno/icons/%1.png").arg(file), QSize(20, 20), QIcon::Normal, state);
    icon.addFile(QStringLiteral(":/liteanno/icons/%1@2x.png").arg(file), QSize(40, 40), QIcon::Normal, state);
}

} // namespace

void applyTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette = app.style()->standardPalette();
    palette.setColor(QPalette::Window, QColor(0xFF, 0xFF, 0xFF));
    palette.setColor(QPalette::WindowText, QColor(0x1F, 0x29, 0x33));
    palette.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
    palette.setColor(QPalette::AlternateBase, QColor(0xF3, 0xF5, 0xF8));
    palette.setColor(QPalette::Text, QColor(0x1F, 0x29, 0x33));
    palette.setColor(QPalette::Button, QColor(0xFF, 0xFF, 0xFF));
    palette.setColor(QPalette::ButtonText, QColor(0x1F, 0x29, 0x33));
    palette.setColor(QPalette::Highlight, QColor(0x1F, 0x6F, 0xEB));
    palette.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
    palette.setColor(QPalette::PlaceholderText, QColor(0x8A, 0x94, 0x9E));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0xA0, 0xA8, 0xB0));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0xA0, 0xA8, 0xB0));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xA0, 0xA8, 0xB0));
    app.setPalette(palette);
#ifdef Q_OS_WIN
    // The Chinese UI font of Windows, a little larger than Qt's 9 pt default.
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
#endif
    app.setStyleSheet(themeStyleSheet());
}

QString themeStyleSheet()
{
    QFile file(QStringLiteral(":/liteanno/theme/light.qss"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QIcon themeIcon(const QString& name)
{
    QIcon icon;
    addPng(icon, name, QIcon::Off);
    addPng(icon, name + QStringLiteral("-on"), QIcon::On);
    return icon;
}

QIcon themeIconVariant(const QString& file)
{
    QIcon icon;
    addPng(icon, file, QIcon::Off);
    return icon;
}

QIcon appIcon()
{
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/liteanno/icons/logo-%1.png").arg(size), QSize(size, size));
    return icon;
}

QColor readableTextOn(const QColor& background)
{
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const QColor rgb = background.toRgb();
    const double luminance =
        0.2126 * linear(rgb.redF()) + 0.7152 * linear(rgb.greenF()) + 0.0722 * linear(rgb.blueF());
    // Contrast with white (luminance 1) and with near-black #111111 (about 0.0056).
    const double withWhite = 1.05 / (luminance + 0.05);
    const double withBlack = (luminance + 0.05) / 0.0556;
    return withBlack >= withWhite ? QColor(0x11, 0x11, 0x11) : QColor(Qt::white);
}

} // namespace liteanno::ui
