#pragma once

#include "core/ShapeType.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace liteanno {

// One entry of "classes" (format spec section 6).
struct ClassDef {
    int id = 0;
    QString name;
    QString color;  // empty when not set
};

enum class ImageLabelMode {
    Single,  // exactly one option selected (format spec 5.3)
    Multi,
};

// "imageLabels"; absent when options is empty.
struct ImageLabelConfig {
    ImageLabelMode mode = ImageLabelMode::Single;
    QStringList options;
};

// One attribute of a class in "attributes".
struct AttributeDef {
    QString name;
    QStringList options;
    bool required = false;
};

// liteanno/project.json (format spec section 6).
struct ProjectConfig {
    QString formatVersion;
    QString projectId;
    QString name;
    QString createdAt;
    QList<ShapeType> shapeTypes;  // empty: the project labels no shapes
    QList<ClassDef> classes;      // empty: any label is accepted
    ImageLabelConfig imageLabels;
    QHash<QString, QList<AttributeDef>> attributes;  // by class name
    int batchSize = 1000;
};

} // namespace liteanno
