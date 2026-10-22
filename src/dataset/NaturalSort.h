#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <vector>

namespace liteanno::dataset {

// Orders paths the way people count: "img2" before "img10". Letters compare without case;
// digit runs compare by value. Ties fall back to the plain characters so the order is total.
bool naturalLess(const QString& a, const QString& b);

// The same order for long lists: case folding is done once per name, not per comparison.
void sortNaturally(QStringList& names);
template <typename T, typename Name>
void sortNaturally(QList<T>& items, Name name);

// Compares names already case folded; 0 when only the plain characters can tell them apart.
int compareFoldedNaturally(QStringView a, QStringView b);

template <typename T, typename Name>
void sortNaturally(QList<T>& items, Name name)
{
    struct Key {
        QString folded;
        qsizetype index;
    };
    std::vector<Key> keys;
    keys.reserve(size_t(items.size()));
    for (qsizetype i = 0; i < items.size(); ++i)
        keys.push_back({name(items[i]).toCaseFolded(), i});
    std::sort(keys.begin(), keys.end(), [&](const Key& a, const Key& b) {
        const int c = compareFoldedNaturally(a.folded, b.folded);
        return c != 0 ? c < 0 : name(items[a.index]) < name(items[b.index]);
    });
    QList<T> sorted;
    sorted.reserve(items.size());
    for (const Key& key : keys)
        sorted.append(std::move(items[key.index]));
    items = std::move(sorted);
}

} // namespace liteanno::dataset
