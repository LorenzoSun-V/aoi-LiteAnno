// A separate process for the reliable-save tests: holds a project lock, or stops itself
// in the middle of a save as a forced kill or a power cut would.
//
//   liteanno_test_helper hold-lock <lock file>
//   liteanno_test_helper kill-during-save <local data root> <data root> <stage>
//
// <stage> is "after-record" or "after-annotation". The save is of image line1/0002.jpg
// (640x480) with project and binding ids from tests/common/ReliableSaveTestData.h.

#include "application/ProjectLock.h"
#include "application/SaveCoordinator.h"
#include "common/ReliableSaveTestData.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QThread>

#include <cstdio>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <csignal>
#endif

using namespace liteanno;

namespace {

[[noreturn]] void killSelf()
{
    std::fflush(nullptr);
#ifdef Q_OS_WIN
    TerminateProcess(GetCurrentProcess(), 9);
#else
    std::raise(SIGKILL);
#endif
    std::_Exit(9);  // not reached
}

int holdLock(const QString& path)
{
    application::ProjectLock lock(path);
    if (lock.tryLock() != application::ProjectLock::Status::Locked)
        return 2;
    std::puts("locked");
    std::fflush(stdout);
    for (;;)
        QThread::sleep(1);
}

int killDuringSave(const QString& localRoot, const QString& dataRoot, const QString& stageName)
{
    const application::SaveStage stage = stageName == QLatin1String("after-record")
                                             ? application::SaveStage::RecoveryRecordWritten
                                             : application::SaveStage::AnnotationWritten;
    application::SaveCoordinator coordinator(application::LocalDataPaths(localRoot),
                                             test::reliableSaveLocation(dataRoot));
    formats::LoadedAnnotation annotation =
        formats::loadAnnotationFile(coordinator.annotationFilePath(test::kReliableSaveImage), test::reliableSaveImageInfo());
    if (annotation.file.isReadOnly() || annotation.file.document().addShape(test::reliableSaveShape()) < 0)
        return 2;
    coordinator.stageHook = [stage](application::SaveStage reached) {
        if (reached == stage)
            killSelf();
    };
    coordinator.save(test::kReliableSaveImage, annotation, 7);
    return 3;  // the hook did not stop the process
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args[1] == QLatin1String("hold-lock"))
        return holdLock(args[2]);
    if (args.size() == 5 && args[1] == QLatin1String("kill-during-save"))
        return killDuringSave(args[2], args[3], args[4]);
    QTextStream(stderr) << "usage: liteanno_test_helper hold-lock <file> | kill-during-save <local> <data> <stage>\n";
    return 1;
}
