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

} // namespace liteanno::formats
