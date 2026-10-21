#pragma once

#include <QString>

namespace liteanno {

// The value of "liteanno.status" (format spec 4.3).
enum class SkipState {
    None,     // no status
    Skipped,  // "skipped"
    Unknown,  // any other value; kept as it is unless an operation sets the review state
};

// Confirmation and skip state of one image (format spec 5.2).
struct ReviewState {
    bool checked = false;  // effective value: a non-boolean "checked" reads as false (13.3)
    SkipState skip = SkipState::None;
    QString skipReason;
};

} // namespace liteanno
