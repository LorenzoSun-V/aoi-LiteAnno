#pragma once

// Shared by the reliable-save tests and the helper process they start.

#include "application/SaveCoordinator.h"
#include "core/ImageInfo.h"
#include "core/Shape.h"

namespace liteanno::test {

inline const QString kReliableSaveImage = QStringLiteral("line1/0002.jpg");

inline application::ProjectLocation reliableSaveLocation(const QString& dataRoot)
{
    return {QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"), QStringLiteral("0123456789abcdef0123456789abcdef"),
            dataRoot};
}

inline ImageInfo reliableSaveImageInfo()
{
    return ImageInfo{QStringLiteral("0002.jpg"), 640, 480, 1};
}

inline Shape reliableSaveShape()
{
    Shape shape;
    shape.type = ShapeType::Rectangle;
    shape.label = QStringLiteral("scratch");
    shape.points = {QPointF(12.5, 20.0), QPointF(64.0, 80.25)};
    return shape;
}

} // namespace liteanno::test
