#pragma once

// The questions the window asks the user. Tests replace them so nothing modal blocks.

#include "application/RecoveryService.h"
#include "application/ImportService.h"
#include "formats/Exporters.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QWidget;

namespace liteanno::ui {

enum class ExternalChangeChoice {
    Reload,     // "重新加载": drop the edits, read the file again
    Overwrite,  // "用我的版本覆盖"
    Later,      // keep the edits in the recovery record
};

enum class RecoveryChoice {
    Restore,  // write the recorded edit to the file
    Discard,  // drop the recorded edit
    Later,
};

// What the export dialog starts with.
struct ExportQuestion {
    int batchNumber = 0;          // the batch shown, 0 when all images are
    bool plainFolder = false;     // a folder without a project file: no batches, nothing confirmed
    bool projectClasses = false;  // the project file numbers its classes
    QString classesFile;          // preset: classes.txt of the data folder, when there is one
};

// What "导出…" asks for.
struct ExportChoice {
    formats::ExportFormat format = formats::ExportFormat::YoloDetection;
    bool wholeProject = true;  // else the batch shown
    QString outputDir;
    bool copyImages = false;
    QString classesFile;  // empty: the project file's classes
};

// What the import dialog starts with.
struct ImportQuestion {
    int batchNumber = 0;       // the batch shown, 0 when all images are
    bool plainFolder = false;  // a folder without a project file: no batches
};

// What "导入…" asks for.
struct ImportChoice {
    formats::ImportFormat format = formats::ImportFormat::YoloDetection;
    bool wholeProject = true;  // else the batch shown
    QString labels;            // the label folder; for COCO the JSON file
    QString classesFile;       // YOLO; empty: classes.txt in the label folder, else the project's
    QString mappingFile;       // masks; empty: mask_colors.json beside the masks
    application::ImportMode mode = application::ImportMode::OnlyNew;
};

struct Prompts {
    // The class of a new shape, or nullopt when the user cancelled. `classes` are offered;
    // with `freeText` any other name may be typed, else one of them must be chosen.
    std::function<std::optional<QString>(const QStringList& classes, const QString& suggested, bool freeText)> chooseLabel;
    std::function<std::optional<QString>()> skipReason;
    std::function<ExternalChangeChoice(const QString& imagePath)> externalChange;
    // `current` is the annotation file as it is now; empty when it does not exist.
    std::function<RecoveryChoice(const application::RecoveryCandidate& candidate, const QByteArray& current)> recovery;
    std::function<void(const QString& title, const QString& text)> message;
    // Closing would lose edits that are neither in the file nor in a recovery record. True
    // when the user gives them up; the default answer keeps the project open.
    std::function<bool(const QString& text)> confirmDiscard;
    // One of `batches` ("第 3 批 …"), `current` preselected; nullopt when cancelled.
    std::function<std::optional<int>(const QStringList& batches, int current)> chooseBatch;
    // The export settings.
    std::function<std::optional<ExportChoice>(const ExportQuestion& question)> exportOptions;
    // The import settings.
    std::function<std::optional<ImportChoice>(const ImportQuestion& question)> importOptions;
    // Some images hold objects the format cannot take (`text` lists them). True: export the
    // others; the default answer exports nothing.
    std::function<bool(const QString& text)> skipUnconvertible;
    // Recovery records that cannot be used (`text` lists them). True: move them to the
    // "unusable" folder, kept as they are; the default answer leaves them.
    std::function<bool(const QString& text)> setAsideUnusable;

    // Dialogs on `parent`.
    static Prompts dialogs(QWidget* parent);
};

} // namespace liteanno::ui
