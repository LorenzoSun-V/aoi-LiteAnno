#include "core/Version.h"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("aoi-LiteAnno"));
    QApplication::setApplicationVersion(QString::fromLatin1(liteanno::kAppVersion));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("aoi-LiteAnno %1").arg(QApplication::applicationVersion()));
    auto* placeholder = new QLabel(QStringLiteral(u"工程骨架：标注界面尚未实现"));
    placeholder->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(placeholder);
    window.resize(960, 640);
    window.show();

    // Used by CI to check that the application starts.
    if (QApplication::arguments().contains(QStringLiteral("--smoke-test")))
        QTimer::singleShot(0, &app, &QApplication::quit);

    return app.exec();
}
