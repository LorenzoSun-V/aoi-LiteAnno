#include "ui/Texts.h"

namespace liteanno::ui {

QString statusText(AnnotationStatus status)
{
    return annotationStatusText(status);
}

QString shapeTypeText(ShapeType type)
{
    switch (type) {
    case ShapeType::Rectangle:
        return QStringLiteral(u"矩形");
    case ShapeType::Polygon:
        return QStringLiteral(u"多边形");
    case ShapeType::Rotation:
        return QStringLiteral(u"旋转框");
    case ShapeType::Point:
        return QStringLiteral(u"点");
    case ShapeType::Line:
        return QStringLiteral(u"线段");
    case ShapeType::LineStrip:
        return QStringLiteral(u"折线");
    case ShapeType::Circle:
        return QStringLiteral(u"圆");
    case ShapeType::Quadrilateral:
        return QStringLiteral(u"四边形");
    case ShapeType::Cuboid:
        return QStringLiteral(u"立方体");
    }
    return QString();
}

QString blockerText(const ConfirmBlocker& blocker)
{
    const QString number = QString::number(blocker.shapeIndex + 1);
    switch (blocker.code) {
    case ConfirmBlockerCode::UnrecognizedObject:
        return QStringLiteral(u"第 %1 个对象无法识别或只能保留，不能确认").arg(number);
    case ConfirmBlockerCode::UnknownClass:
        return QStringLiteral(u"第 %1 个形状的类别“%2”不在类别列表中").arg(number, blocker.detail);
    case ConfirmBlockerCode::MissingAttribute:
        return QStringLiteral(u"第 %1 个形状缺少必填属性“%2”").arg(number, blocker.detail);
    case ConfirmBlockerCode::InvalidGeometry:
        return QStringLiteral(u"第 %1 个形状几何无效（面积为 0 或点数不足）").arg(number);
    case ConfirmBlockerCode::ImageSizeMismatch:
        return QStringLiteral(u"标注文件中的图片尺寸与实际不一致");
    case ConfirmBlockerCode::ImageLabelNotSelected:
        return QStringLiteral(u"请选择一个图片级标签");
    case ConfirmBlockerCode::ImageLabelAmbiguous:
        return QStringLiteral(u"图片级标签只能选一个");
    }
    return QString();
}

QString readOnlyText(formats::ReadOnlyReason reason)
{
    using formats::ReadOnlyReason;
    switch (reason) {
    case ReadOnlyReason::None:
        return QString();
    case ReadOnlyReason::Unreadable:
        return QStringLiteral(u"标注文件无法读取，只能查看。");
    case ReadOnlyReason::JsonError:
        return QStringLiteral(u"标注文件损坏，只能查看，程序不会覆盖它。请交给工程师处理。");
    case ReadOnlyReason::UnsupportedFormatVersion:
        return QStringLiteral(u"标注文件由更新版本的程序写入，只能查看。");
    case ReadOnlyReason::ExifOrientationUnconfirmed:
        return QStringLiteral(u"这是带 EXIF 方向信息的旧标注，坐标方向待确认，只能查看。请交给工程师处理。");
    case ReadOnlyReason::ImageSizeMismatch:
        return QStringLiteral(u"标注文件中的图片尺寸与实际图片不一致，只能查看。可以按当前图片尺寸更正（只改尺寸，不缩放坐标）。");
    }
    return QString();
}

} // namespace liteanno::ui
