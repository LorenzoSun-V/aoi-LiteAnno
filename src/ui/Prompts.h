#pragma once

// The questions the window asks the user. Tests replace them so nothing modal blocks.

#include "application/RecoveryService.h"
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

// What "导出…" asks for.
struct ExportChoice {
    formats::ExportFormat format = formats::ExportFormat::YoloDetection;
    bool wholeProject = true;  // else the batch shown
    QString outputDir;
    bool copyImages = false;
};

struct Prompts {
    // The class of a new shape, or nullopt when the user cancelled.
    std::function<std::optional<QString>(const QStringList& classes, const QString& suggested)> chooseLabel;
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
    // The export settings; `batchNumber` is the batch shown, 0 when all images are.
    std::function<std::optional<ExportChoice>(int batchNumber)> exportOptions;
    // Some images hold objects the format cannot take (`text` lists them). True: export the
    // others; the default answer exports nothing.
    std::function<bool(const QString& text)> skipUnconvertible;

    // Dialogs on `parent`.
    static Prompts dialogs(QWidget* parent);
};

} // namespace liteanno::ui
