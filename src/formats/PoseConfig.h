#pragma once

// The pose configuration X-AnyLabeling's YOLO and COCO pose converters take (LabelConverter
// pose_cfg_file): the keypoint names of each class, in order, and whether the files hold a
// visibility flag per keypoint.
//
//   has_visible: true
//   classes:
//     person:
//       - nose
//       - left_eye
//     car: [front, back]
//
// Upstream reads it with yaml.safe_load; this reads the part of YAML such files use: the two
// keys, block or flow lists of names, plain or quoted scalars, comments. Anything else is
// refused with its line number rather than read differently.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include <utility>

namespace liteanno::formats {

struct PoseConfig {
    QList<std::pair<QString, QStringList>> classes;  // in file order: class number 0, 1, ...
    bool hasVisible = true;
    QString error;  // empty when the file was read

    int classIndex(const QString& name) const;
    const QStringList* keypoints(const QString& name) const;
    int maxKeypoints() const;
};

PoseConfig parsePoseConfig(const QByteArray& bytes);
PoseConfig readPoseConfig(const QString& path);
// The file as parsePoseConfig reads it back, block lists, names quoted when they need it.
QByteArray poseConfigFile(const PoseConfig& config);

// Class names as ultralytics writes them, with the same part of YAML: the names of class
// numbers 0, 1, ... in order, or an error with its line number.
struct ClassNamesFile {
    QStringList names;
    QString error;  // empty when the names were read
};

// A dataset yaml (data.yaml): "names:" as a list ([a, b] or "- a" lines) or as "number: name"
// lines with the numbers 0, 1, ... once each, in any order; "nc", when given, must count them.
// Other keys (path, train, val, download, ...) are passed over.
ClassNamesFile parseDatasetYaml(const QByteArray& bytes);

// The lines of a classes.txt when they are "number: name" lines, as copied from the names of
// a data.yaml: read as those names. nullopt when no line is; an error when only some are.
std::optional<ClassNamesFile> numberedClassLines(const QStringList& lines);

} // namespace liteanno::formats
