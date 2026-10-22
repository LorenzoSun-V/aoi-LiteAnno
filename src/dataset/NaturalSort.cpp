#include "dataset/NaturalSort.h"

namespace liteanno::dataset {

namespace {

// -1, 0 or 1, comparing two runs of ASCII digits by value.
int compareDigits(QStringView a, QStringView b)
{
    while (a.size() > 1 && a.front() == QLatin1Char('0'))
        a = a.mid(1);
    while (b.size() > 1 && b.front() == QLatin1Char('0'))
        b = b.mid(1);
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

bool isDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

} // namespace

int compareFoldedNaturally(QStringView fa, QStringView fb)
{
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < fa.size() && j < fb.size()) {
        if (isDigit(fa[i]) && isDigit(fb[j])) {
            qsizetype ei = i;
            while (ei < fa.size() && isDigit(fa[ei]))
                ++ei;
            qsizetype ej = j;
            while (ej < fb.size() && isDigit(fb[ej]))
                ++ej;
            const int c = compareDigits(fa.mid(i, ei - i), fb.mid(j, ej - j));
            if (c != 0)
                return c;
            i = ei;
            j = ej;
            continue;
        }
        // '/' sorts first so a directory's files come before longer sibling names.
        const bool sa = fa[i] == QLatin1Char('/');
        const bool sb = fb[j] == QLatin1Char('/');
        if (sa != sb)
            return sa ? -1 : 1;
        if (fa[i] != fb[j])
            return fa[i] < fb[j] ? -1 : 1;
        ++i;
        ++j;
    }
    if ((i < fa.size()) != (j < fb.size()))
        return j < fb.size() ? -1 : 1;
    return 0;
}

bool naturalLess(const QString& a, const QString& b)
{
    const int c = compareFoldedNaturally(a.toCaseFolded(), b.toCaseFolded());
    return c != 0 ? c < 0 : a < b;
}

void sortNaturally(QStringList& names)
{
    sortNaturally(names, [](const QString& name) -> const QString& { return name; });
}

} // namespace liteanno::dataset
