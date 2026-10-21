#pragma once

#include <QString>

namespace liteanno::dataset {

// Orders paths the way people count: "img2" before "img10". Letters compare without case;
// digit runs compare by value. Ties fall back to the plain characters so the order is total.
bool naturalLess(const QString& a, const QString& b);

} // namespace liteanno::dataset
