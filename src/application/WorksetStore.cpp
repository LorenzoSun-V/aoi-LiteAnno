#include "application/WorksetStore.h"

#include "core/FormatVersion.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>

namespace liteanno::application {

namespace {

const QRegularExpression& listName()
{
    static const QRegularExpression pattern(QStringLiteral("^(b(\\d+))\\.list\\.json$"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

QString randomBatchId()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QString id;
    for (quint32 word : words)
        id += QStringLiteral("%1").arg(word, 8, 16, QLatin1Char('0'));
    return id;
}

QString stemOf(int number)
{
    return QStringLiteral("b%1").arg(number, 6, 10, QLatin1Char('0'));
}

} // namespace

WorksetStore::WorksetStore(QString root, QString projectId)
    : m_root(std::move(root))
    , m_projectId(std::move(projectId))
    , m_directory(m_root + QLatin1String("/liteanno/worksets"))
{
}

QString WorksetStore::timestamp(const QDateTime& time)
{
    return time.toOffsetFromUtc(time.offsetFromUtc()).toString(Qt::ISODate);
}

QString WorksetStore::listPath(const QString& stem) const
{
    return m_directory + QLatin1Char('/') + stem + QLatin1String(".list.json");
}

QString WorksetStore::progressPath(const QString& stem) const
{
    return m_directory + QLatin1Char('/') + stem + QLatin1String(".progress.json");
}

bool WorksetStore::load()
{
    m_batches.clear();
    m_assigned.clear();
    m_loaded = false;
    QDir dir(m_directory);
    if (!dir.exists()) {
        m_loaded = true;  // no batch yet
        return true;
    }
    if (!dir.isReadable())
        return false;
    const QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QString& name : names) {
        const QRegularExpressionMatch match = listName().match(name);
        if (!match.hasMatch())
            continue;
        Batch batch;
        batch.stem = match.captured(1);
        batch.number = match.captured(2).toInt();
        QFile file(listPath(batch.stem));
        if (!file.open(QIODevice::ReadOnly)) {
            // Unreadable now: what it lists is unknown, so nothing may be given out.
            return false;
        }
        const QByteArray bytes = file.readAll();
        formats::WorksetListResult parsed = formats::parseWorksetList(bytes);
        if (parsed.list && parsed.list->projectId != m_projectId)
            parsed = {std::nullopt, QStringLiteral("projectId %1 is not this project's").arg(parsed.list->projectId)};
        if (parsed.list) {
            batch.number = std::max(batch.number, parsed.list->number);
            for (const QString& item : std::as_const(parsed.list->items))
                ++m_assigned[item];
            batch.list = std::move(parsed.list);
        } else {
            batch.error = parsed.error;
            batch.damagedItems = formats::readItemsOfDamagedList(bytes);
            for (const QString& item : std::as_const(batch.damagedItems))
                ++m_assigned[item];
        }
        m_batches.append(std::move(batch));
    }
    std::stable_sort(m_batches.begin(), m_batches.end(), [](const Batch& a, const Batch& b) {
        return a.number != b.number ? a.number < b.number : a.stem < b.stem;
    });
    m_loaded = true;
    return true;
}

const WorksetStore::Batch* WorksetStore::find(const QString& stem) const
{
    for (const Batch& batch : m_batches) {
        if (batch.stem == stem)
            return &batch;
    }
    return nullptr;
}

QStringList WorksetStore::duplicates() const
{
    QStringList paths;
    for (auto it = m_assigned.cbegin(); it != m_assigned.cend(); ++it) {
        if (it.value() > 1)
            paths.append(it.key());
    }
    paths.sort();
    return paths;
}

WorksetStore::Created WorksetStore::createNext(const QStringList& candidates, int batchSize, const QDateTime& now)
{
    if (!load())
        return {nullptr, QStringLiteral(u"无法读取批次文件夹 %1").arg(QDir::toNativeSeparators(m_directory))};
    QStringList items;
    const int size = std::max(1, batchSize);
    for (const QString& path : candidates) {
        if (items.size() >= size)
            break;
        if (!m_assigned.contains(path))
            items.append(path);
    }
    if (items.isEmpty())
        return {};
    if (!QDir().mkpath(m_directory))
        return {nullptr, QStringLiteral(u"无法创建批次文件夹 %1").arg(QDir::toNativeSeparators(m_directory))};

    formats::WorksetList list;
    list.formatVersion = QString::fromLatin1(kFormatVersion);
    list.projectId = m_projectId;
    list.batchId = randomBatchId();
    list.createdAt = timestamp(now);
    list.items = items;
    list.itemsDigest = formats::computeItemsDigest(items);
    int number = 0;
    for (const Batch& batch : std::as_const(m_batches))
        number = std::max(number, batch.number);
    // The temporary file is complete on disk before it gets the list's name; the rename
    // fails rather than replace a list another program created meanwhile.
    for (int attempt = 0; attempt < 100; ++attempt) {
        ++number;
        list.number = number;
        const QString stem = stemOf(number);
        const QString target = listPath(stem);
        if (QFileInfo::exists(target))
            continue;
        const QString temporary = target + QLatin1Char('.') + randomBatchId().left(8) + QLatin1String(".tmp");
        const formats::SaveResult written =
            formats::writeFileAtomically(temporary, formats::worksetListToBytes(list));
        if (written.status != formats::SaveStatus::Saved)
            return {nullptr, written.error};
        const bool renamed = QFile::rename(temporary, target);
        if (!renamed) {
            QFile::remove(temporary);
            if (QFileInfo::exists(target))
                continue;
            return {nullptr, QStringLiteral(u"无法写入批次清单 %1").arg(QDir::toNativeSeparators(target))};
        }
        Batch batch;
        batch.stem = stem;
        batch.number = number;
        batch.list = list;
        for (const QString& item : std::as_const(items))
            ++m_assigned[item];
        m_batches.append(std::move(batch));
        const Batch& created = m_batches.last();
        formats::WorksetProgress progress;
        progress.batchId = list.batchId;
        progress.listDigest = list.itemsDigest;
        progress.events.append({QStringLiteral("started"), list.createdAt});
        writeProgress(created, progress);  // a lost progress file only loses the position
        return {&created, QString()};
    }
    return {nullptr, QStringLiteral(u"没有可用的批次编号")};
}

std::optional<formats::WorksetProgress> WorksetStore::progress(const Batch& batch) const
{
    if (!batch.list)
        return std::nullopt;
    QFile file(progressPath(batch.stem));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    std::optional<formats::WorksetProgress> progress = formats::parseWorksetProgress(file.readAll());
    if (!progress || progress->batchId != batch.list->batchId || progress->listDigest != batch.list->itemsDigest)
        return std::nullopt;
    return progress;
}

formats::SaveResult WorksetStore::writeProgress(const Batch& batch, const formats::WorksetProgress& progress) const
{
    formats::SaveOptions options;
    options.maxAttempts = 3;
    return formats::writeFileAtomically(progressPath(batch.stem), formats::worksetProgressToBytes(progress), options);
}

const WorksetStore::Batch* WorksetStore::lastWorkedOn() const
{
    const Batch* newest = nullptr;
    QDateTime newestTime;
    const Batch* first = nullptr;
    for (const Batch& batch : m_batches) {
        if (!batch.list)
            continue;
        if (!first)
            first = &batch;
        if (!progress(batch))
            continue;
        const QDateTime modified = QFileInfo(progressPath(batch.stem)).lastModified();
        if (!newest || modified > newestTime || (modified == newestTime && batch.number > newest->number)) {
            newest = &batch;
            newestTime = modified;
        }
    }
    return newest ? newest : first;
}

} // namespace liteanno::application
