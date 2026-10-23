#include "core/Version.h"
#include "imaging/ImageLoader.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <cstdio>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("aoi-LiteAnno"));
    QApplication::setApplicationVersion(QString::fromLatin1(liteanno::kAppVersion));
    // The interface is Chinese; so are Qt's own dialogs and buttons (file dialogs, OK/Cancel).
    // The single-file build carries them inside (":/i18n", src/ui/CMakeLists.txt); the folder
    // build has them in Qt's translations folder.
    QTranslator qtTranslator;
    const QLocale chinese(QLocale::Chinese, QLocale::China);
    const bool builtInTranslation =
        qtTranslator.load(chinese, QStringLiteral("qtbase"), QStringLiteral("_"), QStringLiteral(":/i18n"));
    if (builtInTranslation
        || qtTranslator.load(chinese, QStringLiteral("qtbase"), QStringLiteral("_"),
                             QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Used by the package workflow: the single-file build has Qt linked in and carries the
    // translation and the licence texts, so nothing has to be next to it.
    if (QApplication::arguments().contains(QStringLiteral("--check-single-file"))) {
#ifdef QT_STATIC
        const bool staticQt = true;
#else
        const bool staticQt = false;
#endif
        const bool licenses = !QDir(QStringLiteral(":/licenses")).isEmpty();
        const QStringList missing = liteanno::imaging::missingImageFormats();
        std::fprintf(stderr, "static Qt: %d, built-in translation: %d, licence texts: %d, missing image formats: %s\n",
                     staticQt, builtInTranslation, licenses, qPrintable(missing.join(QLatin1Char(' '))));
        return staticQt && builtInTranslation && licenses && missing.isEmpty() ? 0 : 1;
    }

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
