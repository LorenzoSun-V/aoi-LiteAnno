#pragma once

// The questions the window asks the user. Tests replace them so nothing modal blocks.

#include "application/RecoveryService.h"

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

struct Prompts {
    // The class of a new shape, or nullopt when the user cancelled.
    std::function<std::optional<QString>(const QStringList& classes, const QString& suggested)> chooseLabel;
    std::function<std::optional<QString>()> skipReason;
    std::function<ExternalChangeChoice(const QString& imagePath)> externalChange;
    // `current` is the annotation file as it is now; empty when it does not exist.
    std::function<RecoveryChoice(const application::RecoveryCandidate& candidate, const QByteArray& current)> recovery;
    std::function<void(const QString& title, const QString& text)> message;

    // Dialogs on `parent`.
    static Prompts dialogs(QWidget* parent);
};

} // namespace liteanno::ui
