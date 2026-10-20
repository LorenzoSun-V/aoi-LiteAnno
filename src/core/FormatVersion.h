#pragma once

namespace liteanno {

// Disk format version written by this build (format spec section 1). Files whose major
// version is higher are opened read-only.
inline constexpr int kFormatVersionMajor = 1;
inline constexpr int kFormatVersionMinor = 0;
inline constexpr char kFormatVersion[] = "1.0";

} // namespace liteanno
