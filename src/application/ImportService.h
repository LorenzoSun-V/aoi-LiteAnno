#pragma once

#include "application/ExportService.h"
#include "formats/Importers.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace liteanno::application {

// What happens to an image that already has an annotation file (architecture 11.2).
enum class ImportMode {
    OnlyNew,  // leave it alone; import only images without one
    Append,   // add the imported shapes after its own
    Replace,  // remove its shapes and put the imported ones in their place
};
QString importModeName(ImportMode mode);  // "new", "append", "replace"
std::optional<ImportMode> importModeFromName(const QString& name);

struct ImportRequest {
    formats::ImportFormat format = formats::ImportFormat::YoloDetection;
    QString labelDir;
    QStringList batches;  // list stems ("b000003"); empty: the whole project
    ImportMode mode = ImportMode::OnlyNew;
    bool skipUnconvertible = false;  // "跳过这些图片继续导入"
};

enum class ImportResult {
    Imported,
    NoLabelFile,    // nothing to import for this image
    HasAnnotation,  // has an annotation file and the mode leaves it alone
    NotWritten,     // missing, read-only, name collision, unsaved window edits, write failed
    Unconvertible,  // the label file cannot be imported
};
const char* importResultName(ImportResult result);

struct ImportItem {
    QString image;
    ImportResult result = ImportResult::NoLabelFile;
    QString labelFile;  // absolute, when one was found
    QString reason;
    int shapes = 0;  // imported shapes
};

struct ImportReport {
    // No annotation file was changed unless this is true.
    bool written = false;
    QString error;
    bool cancelled = false;
    QList<ImportItem> items;

    int count(ImportResult result) const;
    QList<ImportItem> unconvertible() const;
};

// Turns label files into annotation files (architecture 11.2): reads the label file of every
// image of the scope, refuses when any cannot be imported (unless told to skip those images),
// then writes the changed annotation files one by one with the checks of a normal save.
// Imported and changed images are waiting for confirmation afterwards.
class ImportService
{
public:
    // Called with the images handled so far; returning false cancels. A cancel while writing
    // keeps the files written so far.
    using Progress = std::function<bool(int done, int total)>;

    ImportService(const ProjectSession& project, ImageInfoReader readImageInfo);

    ImportReport run(const ImportRequest& request, const Progress& progress = {}) const;

private:
    const ProjectSession& m_project;
    ImageInfoReader m_readImageInfo;
};

} // namespace liteanno::application
