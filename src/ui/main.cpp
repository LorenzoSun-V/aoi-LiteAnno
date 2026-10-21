#include "core/Version.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("aoi-LiteAnno"));
    QApplication::setApplicationVersion(QString::fromLatin1(liteanno::kAppVersion));

    liteanno::ui::MainWindow window;
    window.show();

    // Used by CI to check that the application starts.
    const QStringList args = QApplication::arguments();
    if (args.contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &app, &QApplication::quit);
    } else if (args.size() > 1 && !args.last().startsWith(QLatin1Char('-'))) {
        // A data root given on the command line, e.g. by dropping a folder on the program.
        const QString root = args.last();
        QTimer::singleShot(0, &window, [&window, root] { window.openProject(root); });
    }
    return app.exec();
}
