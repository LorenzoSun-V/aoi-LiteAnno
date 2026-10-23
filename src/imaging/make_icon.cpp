// Build tool: liteanno-make-icon <logo.png> <out.ico>. Makes the Windows icon of both
// programs from the one logo file (LITEANNO_LOGO, branding/README.md).
#include "imaging/IconFile.h"

#include <QFile>
#include <QImage>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: liteanno-make-icon <logo.png> <out.ico>\n");
        return 2;
    }
    const QString input = QString::fromLocal8Bit(argv[1]);
    const QImage logo(input);
    if (logo.isNull()) {
        std::fprintf(stderr, "liteanno-make-icon: cannot read %s as an image\n", argv[1]);
        return 1;
    }
    const QByteArray icon = liteanno::imaging::windowsIcon(logo);
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly) || out.write(icon) != icon.size() || !out.flush()) {
        std::fprintf(stderr, "liteanno-make-icon: cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
