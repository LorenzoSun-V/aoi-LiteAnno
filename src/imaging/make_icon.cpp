// Build tool: liteanno-make-icon <logo.png> <out.ico>. Makes the Windows icon of both
// programs from the one logo file (LITEANNO_LOGO, branding/README.md).
#include "imaging/IconFile.h"

#include <QCoreApplication>
#include <QFile>
#include <QImage>

#include <cstdio>

int main(int argc, char** argv)
{
    // The paths come from QCoreApplication: on Windows it reads the wide command line, so a
    // logo or build folder with characters outside the code page still arrives whole.
    QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != 3) {
        std::fprintf(stderr, "usage: liteanno-make-icon <logo.png> <out.ico>\n");
        return 2;
    }
    const QImage logo(arguments[1]);
    if (logo.isNull()) {
        std::fprintf(stderr, "liteanno-make-icon: cannot read %s as an image\n", qUtf8Printable(arguments[1]));
        return 1;
    }
    const QByteArray icon = liteanno::imaging::windowsIcon(logo);
    QFile out(arguments[2]);
    if (!out.open(QIODevice::WriteOnly) || out.write(icon) != icon.size() || !out.flush()) {
        std::fprintf(stderr, "liteanno-make-icon: cannot write %s\n", qUtf8Printable(arguments[2]));
        return 1;
    }
    return 0;
}
