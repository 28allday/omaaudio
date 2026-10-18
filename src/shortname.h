#pragma once

#include <QString>

#include <algorithm>
#include <cmath>

// A name inside a sentence the interface shows (a recipe's "Run on" line, a
// batch summary), cut in the middle so both ends stay: a long file or track name
// never stretches the line or the panel. The same rule as Theme.shortName in QML.
inline QString shortName(const QString& name, int max = 40)
{
    const int limit = std::max(8, max);
    if (name.size() <= limit) return name;
    const int head = int(std::ceil((limit - 1) * 0.6));
    return name.left(head).trimmed() + QChar(0x2026) + name.right(limit - 1 - head).trimmed();
}
