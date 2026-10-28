#include "formats/PoseConfig.h"

#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <optional>

namespace liteanno::formats {

namespace {

struct Line {
    int number = 0;  // from 1
    int indent = 0;
    QString text;  // without the indentation and the comment
};

// The part of a line before its comment: "#" at the start or after white space, outside
// quotes.
QString withoutComment(const QString& line)
{
    QChar quote;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"'))
            quote = c;
        else if (c == QLatin1Char('#') && (i == 0 || line[i - 1].isSpace()))
            return line.left(i);
    }
    return line;
}

// What YAML would read as something other than a string (yaml.safe_load, YAML 1.1).
bool readsAsOtherThanText(const QString& text)
{
    static const QRegularExpression other(
        QStringLiteral(R"(^(~|null|Null|NULL|true|True|TRUE|false|False|FALSE|yes|Yes|YES|no|No|NO|on|On|ON|off|Off|OFF|y|Y|n|N)$)"
                       R"(|^[-+]?(0|[1-9][0-9_]*|0[0-7_]+|0x[0-9a-fA-F_]+|0b[01_]+)$)"
                       R"(|^[-+]?([0-9][0-9_]*)?\.[0-9_]*([eE][-+][0-9]+)?$)"
                       R"(|^[-+]?\.(inf|Inf|INF)$|^\.(nan|NaN|NAN)$)"
                       R"(|^[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+(\.[0-9_]*)?$)"));
    return other.match(text).hasMatch();
}

// One scalar: plain or quoted. nullopt with `error` set when it is not a plain name.
std::optional<QString> scalar(const QString& raw, QString& error)
{
    const QString text = raw.trimmed();
    if (text.isEmpty()) {
        error = QStringLiteral(u"名称是空的");
        return std::nullopt;
    }
    if (text.startsWith(QLatin1Char('\''))) {
        if (text.size() < 2 || !text.endsWith(QLatin1Char('\''))) {
            error = QStringLiteral(u"单引号没有配对");
            return std::nullopt;
        }
        QString inner = text.mid(1, text.size() - 2);
        if (inner.replace(QStringLiteral("''"), QString()).contains(QLatin1Char('\''))) {
            error = QStringLiteral(u"单引号没有配对");
            return std::nullopt;
        }
        return text.mid(1, text.size() - 2).replace(QStringLiteral("''"), QStringLiteral("'"));
    }
    if (text.startsWith(QLatin1Char('"'))) {
        if (text.size() < 2 || !text.endsWith(QLatin1Char('"'))) {
            error = QStringLiteral(u"双引号没有配对");
            return std::nullopt;
        }
        QString result;
        const QString inner = text.mid(1, text.size() - 2);
        for (qsizetype i = 0; i < inner.size(); ++i) {
            const QChar c = inner[i];
            if (c == QLatin1Char('"')) {
                error = QStringLiteral(u"双引号没有配对");
                return std::nullopt;
            }
            if (c == QLatin1Char('\\')) {
                if (i + 1 < inner.size() && (inner[i + 1] == QLatin1Char('"') || inner[i + 1] == QLatin1Char('\\'))) {
                    result += inner[++i];
                    continue;
                }
                error = QStringLiteral(u"双引号里只能用 \\\" 和 \\\\ 两种转义");
                return std::nullopt;
            }
            result += c;
        }
        return result;
    }
    static const QString special = QStringLiteral("[]{}&*!|>%@`,'\"");
    if (special.contains(text[0]) || text.startsWith(QLatin1String("- ")) || text == QLatin1String("-")
        || text.startsWith(QLatin1String("? ")) || text.contains(QLatin1String(": ")) || text.contains(QLatin1String(" #")) || text.endsWith(QLatin1Char(':'))
        || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\t'))) {
        error = QStringLiteral(u"“%1”需要加引号").arg(text);
        return std::nullopt;
    }
    if (readsAsOtherThanText(text)) {
        error = QStringLiteral(u"“%1”会被读成数字或布尔值，作为名称请加引号").arg(text);
        return std::nullopt;
    }
    return text;
}

// "[a, b, 'c']": names separated by commas.
std::optional<QStringList> flowList(const QString& text, QString& error)
{
    if (!text.startsWith(QLatin1Char('[')) || !text.endsWith(QLatin1Char(']'))) {
        error = QStringLiteral(u"列表应写成 [a, b] 或每行一个“- 名称”");
        return std::nullopt;
    }
    const QString inner = text.mid(1, text.size() - 2).trimmed();
    QStringList names;
    if (inner.isEmpty())
        return names;
    QString current;
    QChar quote;
    QStringList parts;
    for (const QChar c : inner) {
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            current += c;
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            current += c;
        } else if (c == QLatin1Char(',')) {
            parts.append(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.append(current);
    if (parts.size() > 1 && parts.last().trimmed().isEmpty())
        parts.removeLast();  // "[a, b,]"
    for (const QString& part : parts) {
        const std::optional<QString> name = scalar(part, error);
        if (!name)
            return std::nullopt;
        names.append(*name);
    }
    return names;
}

// "key: value" or "key:"; the key as written and the value, trimmed.
bool splitKey(const QString& text, QString& key, QString& value)
{
    QChar quote;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"'))
            quote = c;
        else if (c == QLatin1Char(':') && (i + 1 == text.size() || text[i + 1] == QLatin1Char(' '))) {
            key = text.left(i);
            value = text.mid(i + 1).trimmed();
            return true;
        }
    }
    return false;
}

// The lines of a YAML file without comments, blank lines and a leading "---". nullopt with
// `error` set (its line number in it) for text that is not UTF-8 or indented with tabs.
std::optional<QList<Line>> yamlLines(const QByteArray& bytes, QString& error)
{
    QByteArray data = bytes;
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = decoder(data);
    if (decoder.hasError()) {
        error = QStringLiteral(u"不是 UTF-8 编码的文本文件");
        return std::nullopt;
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QList<Line> lines;
    const QStringList raw = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QString content = withoutComment(raw[i]);
        if (content.trimmed().isEmpty())
            continue;
        if (content.trimmed() == QLatin1String("---") && lines.isEmpty())
            continue;  // the document start
        int indent = 0;
        while (indent < content.size() && content[indent] == QLatin1Char(' '))
            ++indent;
        if (indent < content.size() && content[indent] == QLatin1Char('\t')) {
            error = QStringLiteral(u"第 %1 行：缩进不能用制表符").arg(i + 1);
            return std::nullopt;
        }
        lines.append({int(i) + 1, indent, content.mid(indent).trimmed()});
    }
    return lines;
}

// A class number as a YAML key: digits only (ultralytics reads the keys of names as ints).
std::optional<int> classNumber(const QString& key)
{
    static const QRegularExpression digits(QStringLiteral("^(0|[1-9][0-9]{0,8})$"));
    if (!digits.match(key).hasMatch())
        return std::nullopt;
    return key.toInt();
}

QString atLine(int line, const QString& reason)
{
    return QStringLiteral(u"第 %1 行：%2").arg(line).arg(reason);
}

bool isItem(const Line& line)
{
    return line.text == QLatin1String("-") || line.text.startsWith(QLatin1String("- "));
}

// The class names in `lines` from `i` on, while they are indented at least `indent`: "- name"
// items, or "number: name" with each of the numbers 0, 1, ... once, in any order. With
// `itemsOnly`, only "- name" lines (a list at the indentation of its key). Moves `i` past them.
std::optional<QStringList> namesBlock(const QList<Line>& lines, qsizetype& i, int indent, bool itemsOnly, QString& error)
{
    QStringList list;
    QMap<int, QString> numbered;
    QMap<int, int> numberLine;
    int itemIndent = -1;
    while (i < lines.size() && lines[i].indent >= indent && (!itemsOnly || (lines[i].indent == indent && isItem(lines[i])))) {
        const Line& line = lines[i];
        if (itemIndent < 0)
            itemIndent = line.indent;
        if (line.indent != itemIndent) {
            error = atLine(line.number, QStringLiteral(u"缩进不对"));
            return std::nullopt;
        }
        const bool item = isItem(line);
        if (item ? !numbered.isEmpty() : !list.isEmpty()) {
            error = atLine(line.number, QStringLiteral(u"“- 名称”和“编号: 名称”不能混用"));
            return std::nullopt;
        }
        QString value = line.text.mid(1);
        std::optional<int> number;
        if (!item) {
            QString key;
            if (splitKey(line.text, key, value))
                number = classNumber(key.trimmed());
            if (!number) {
                error = atLine(line.number, QStringLiteral(u"应是“编号: 名称”（编号 0、1、2…）或“- 名称”"));
                return std::nullopt;
            }
            if (numbered.contains(*number)) {
                error = atLine(line.number, QStringLiteral(u"编号 %1 写了两次").arg(*number));
                return std::nullopt;
            }
        }
        QString reason;
        const std::optional<QString> name = scalar(value, reason);
        if (!name) {
            error = atLine(line.number, reason);
            return std::nullopt;
        }
        if (number) {
            numbered.insert(*number, *name);
            numberLine.insert(*number, line.number);
        } else {
            list.append(*name);
        }
        ++i;
    }
    if (!numbered.isEmpty()) {
        int expected = 0;
        for (auto it = numbered.cbegin(); it != numbered.cend(); ++it, ++expected) {
            if (it.key() != expected) {
                error = atLine(numberLine.value(it.key()), QStringLiteral(u"缺少编号 %1：编号应从 0 起连续").arg(expected));
                return std::nullopt;
            }
            list.append(it.value());
        }
    }
    return list;
}

// The checks a classes.txt gets as well: names, none twice.
std::optional<QStringList> distinctNames(const QStringList& names, QString& error)
{
    if (names.isEmpty()) {
        error = QStringLiteral(u"文件里没有类别");
        return std::nullopt;
    }
    QSet<QString> seen;
    for (const QString& name : names) {
        // Quotes can hold an empty or blank name: a classes.txt written with it could not be
        // read back.
        if (name.trimmed().isEmpty()) {
            error = QStringLiteral(u"有类别名是空的");
            return std::nullopt;
        }
        if (seen.contains(name)) {
            error = QStringLiteral(u"类别“%1”写了两次").arg(name);
            return std::nullopt;
        }
        seen.insert(name);
    }
    return names;
}

PoseConfig failed(int line, const QString& reason)
{
    PoseConfig config;
    config.error = line > 0 ? QStringLiteral(u"第 %1 行：%2").arg(line).arg(reason) : reason;
    return config;
}

QString quotedIfNeeded(const QString& name)
{
    QString error;
    const std::optional<QString> plain = scalar(name, error);
    if (plain && *plain == name)
        return name;
    QString escaped = name;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

} // namespace

int PoseConfig::classIndex(const QString& name) const
{
    for (qsizetype i = 0; i < classes.size(); ++i) {
        if (classes[i].first == name)
            return int(i);
    }
    return -1;
}

const QStringList* PoseConfig::keypoints(const QString& name) const
{
    const int index = classIndex(name);
    return index < 0 ? nullptr : &classes[index].second;
}

int PoseConfig::maxKeypoints() const
{
    int count = 0;
    for (const auto& entry : classes)
        count = std::max(count, int(entry.second.size()));
    return count;
}

PoseConfig parsePoseConfig(const QByteArray& bytes)
{
    QString error;
    const std::optional<QList<Line>> read = yamlLines(bytes, error);
    if (!read)
        return failed(0, error);
    const QList<Line>& lines = *read;

    PoseConfig config;
    bool sawClasses = false;
    QSet<QString> classNames;
    for (qsizetype i = 0; i < lines.size();) {
        const Line& line = lines[i];
        if (line.indent != 0)
            return failed(line.number, QStringLiteral(u"缩进不对"));
        QString key;
        QString value;
        if (!splitKey(line.text, key, value))
            return failed(line.number, QStringLiteral(u"应是“名称: 值”"));
        key = key.trimmed();
        ++i;
        if (key == QLatin1String("has_visible")) {
            static const QStringList yes = {QStringLiteral("true"), QStringLiteral("True"), QStringLiteral("TRUE")};
            static const QStringList no = {QStringLiteral("false"), QStringLiteral("False"), QStringLiteral("FALSE")};
            if (yes.contains(value))
                config.hasVisible = true;
            else if (no.contains(value))
                config.hasVisible = false;
            else
                return failed(line.number, QStringLiteral(u"has_visible 应是 true 或 false"));
            continue;
        }
        if (key != QLatin1String("classes")) {
            // Upstream reads only these two keys; others and what is under them are passed over.
            while (i < lines.size() && lines[i].indent > 0)
                ++i;
            continue;
        }
        if (sawClasses)
            return failed(line.number, QStringLiteral(u"classes 写了两次"));
        sawClasses = true;
        if (!value.isEmpty())
            return failed(line.number, QStringLiteral(u"classes 下面应是每行一个“类别: 关键点列表”"));
        int classIndent = -1;
        while (i < lines.size() && lines[i].indent > 0) {
            const Line& entry = lines[i];
            if (classIndent < 0)
                classIndent = entry.indent;
            if (entry.indent != classIndent)
                return failed(entry.number, QStringLiteral(u"缩进不对"));
            QString name;
            QString list;
            if (entry.text.startsWith(QLatin1Char('-')) || !splitKey(entry.text, name, list))
                return failed(entry.number, QStringLiteral(u"应是“类别: 关键点列表”"));
            QString error;
            const std::optional<QString> className = scalar(name, error);
            if (!className)
                return failed(entry.number, error);
            if (classNames.contains(*className))
                return failed(entry.number, QStringLiteral(u"类别“%1”写了两次").arg(*className));
            classNames.insert(*className);
            ++i;
            QStringList keypoints;
            if (!list.isEmpty()) {
                const std::optional<QStringList> names = flowList(list, error);
                if (!names)
                    return failed(entry.number, error);
                keypoints = *names;
            } else {
                // "- name" lines, indented more than the class or at its indentation.
                int itemIndent = -1;
                while (i < lines.size() && lines[i].indent >= classIndent && lines[i].text.startsWith(QLatin1Char('-'))) {
                    const Line& item = lines[i];
                    if (itemIndent < 0)
                        itemIndent = item.indent;
                    if (item.indent != itemIndent)
                        return failed(item.number, QStringLiteral(u"缩进不对"));
                    if (item.text != QLatin1String("-") && !item.text.startsWith(QLatin1String("- ")))
                        return failed(item.number, QStringLiteral(u"列表项应写成“- 名称”"));
                    const std::optional<QString> keypoint = scalar(item.text.mid(1), error);
                    if (!keypoint)
                        return failed(item.number, error);
                    keypoints.append(*keypoint);
                    ++i;
                }
                if (i < lines.size() && lines[i].indent > classIndent)
                    return failed(lines[i].number, QStringLiteral(u"应是“- 关键点名称”"));
            }
            if (keypoints.isEmpty())
                return failed(entry.number, QStringLiteral(u"类别“%1”没有关键点").arg(*className));
            QSet<QString> seen;
            for (const QString& keypoint : std::as_const(keypoints)) {
                if (seen.contains(keypoint))
                    return failed(entry.number, QStringLiteral(u"类别“%1”的关键点“%2”写了两次").arg(*className, keypoint));
                seen.insert(keypoint);
            }
            config.classes.append({*className, keypoints});
        }
    }
    if (config.classes.isEmpty())
        return failed(0, QStringLiteral(u"没有 classes，或 classes 下面没有类别"));
    return config;
}

PoseConfig readPoseConfig(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failed(0, QStringLiteral(u"无法读取：%1").arg(file.errorString()));
    return parsePoseConfig(file.readAll());
}

QByteArray poseConfigFile(const PoseConfig& config)
{
    QString text = config.hasVisible ? QStringLiteral("has_visible: true\n") : QStringLiteral("has_visible: false\n");
    text += QStringLiteral("classes:\n");
    for (const auto& [name, keypoints] : config.classes) {
        text += QStringLiteral("  ") + quotedIfNeeded(name) + QStringLiteral(":\n");
        for (const QString& keypoint : keypoints)
            text += QStringLiteral("    - ") + quotedIfNeeded(keypoint) + QLatin1Char('\n');
    }
    return text.toUtf8();
}

ClassNamesFile parseDatasetYaml(const QByteArray& bytes)
{
    ClassNamesFile result;
    const std::optional<QList<Line>> read = yamlLines(bytes, result.error);
    if (!read)
        return result;
    const QList<Line>& lines = *read;
    std::optional<QStringList> names;
    std::optional<int> nc;
    int ncLine = 0;
    for (qsizetype i = 0; i < lines.size();) {
        const Line& line = lines[i];
        QString key;
        QString value;
        if (line.indent != 0 || !splitKey(line.text, key, value)) {
            result.error = atLine(line.number, QStringLiteral(u"应是“名称: 值”"));
            return result;
        }
        key = key.trimmed();
        ++i;
        if (key == QLatin1String("nc")) {
            nc = classNumber(value);
            ncLine = line.number;
            if (!nc) {
                result.error = atLine(line.number, QStringLiteral(u"nc 应是类别个数"));
                return result;
            }
        } else if (key == QLatin1String("names")) {
            if (names) {
                result.error = atLine(line.number, QStringLiteral(u"names 写了两次"));
                return result;
            }
            if (!value.isEmpty()) {
                if (value.startsWith(QLatin1Char('{'))) {
                    result.error = atLine(line.number, QStringLiteral(u"names 请写成 [a, b]，或下面每行一个“编号: 名称”"));
                    return result;
                }
                QString reason;
                names = flowList(value, reason);
                if (!names) {
                    result.error = atLine(line.number, reason);
                    return result;
                }
            } else {
                // Indented under the key, or a "- name" list at its indentation.
                const bool sameIndent = i < lines.size() && lines[i].indent == 0 && isItem(lines[i]);
                names = namesBlock(lines, i, sameIndent ? 0 : 1, sameIndent, result.error);
                if (!names)
                    return result;
            }
        }
        // Other keys (path, train, val, download, ...) and what is under them are passed over,
        // also a "- item" list at the key's indentation (train: with a list of folders).
        while (i < lines.size() && (lines[i].indent > 0 || isItem(lines[i])))
            ++i;
    }
    if (!names) {
        result.error = QStringLiteral(u"文件里没有 names（类别名称）");
        return result;
    }
    if (!distinctNames(*names, result.error))
        return result;
    if (nc && *nc != names->size()) {
        result.error = atLine(ncLine, QStringLiteral(u"nc 是 %1，names 里有 %2 个类别").arg(*nc).arg(names->size()));
        return result;
    }
    result.names = *names;
    return result;
}

std::optional<ClassNamesFile> numberedClassLines(const QStringList& lines)
{
    static const QRegularExpression numbered(QStringLiteral(R"(^\s*[0-9]+\s*:(\s|$))"));
    qsizetype count = 0;
    for (const QString& line : lines)
        count += numbered.match(line).hasMatch() ? 1 : 0;
    if (count == 0)
        return std::nullopt;
    ClassNamesFile result;
    if (count != lines.size()) {
        for (qsizetype i = 0; i < lines.size(); ++i) {
            if (!numbered.match(lines[i]).hasMatch()) {
                result.error = atLine(int(i) + 1, QStringLiteral(u"有的行写了“编号: 名称”，这一行没有；请每行都写编号，或都不写"));
                return result;
            }
        }
    }
    QList<Line> parsed;
    for (qsizetype i = 0; i < lines.size(); ++i)
        parsed.append({int(i) + 1, 0, lines[i].trimmed()});
    qsizetype i = 0;
    const std::optional<QStringList> names = namesBlock(parsed, i, 0, false, result.error);
    if (names && distinctNames(*names, result.error))
        result.names = *names;
    return result;
}

} // namespace liteanno::formats
