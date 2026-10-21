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

bool naturalLess(const QString& a, const QString& b)
{
    const QString fa = a.toCaseFolded();
    const QString fb = b.toCaseFolded();
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
            const int c = compareDigits(QStringView(fa).mid(i, ei - i), QStringView(fb).mid(j, ej - j));
            if (c != 0)
                return c < 0;
            i = ei;
            j = ej;
            continue;
        }
        // '/' sorts first so a directory's files come before longer sibling names.
        const bool sa = fa[i] == QLatin1Char('/');
        const bool sb = fb[j] == QLatin1Char('/');
        if (sa != sb)
            return sa;
        if (fa[i] != fb[j])
            return fa[i] < fb[j];
        ++i;
        ++j;
    }
    if ((i < fa.size()) != (j < fb.size()))
        return j < fb.size();
    return a < b;
}

} // namespace liteanno::dataset
