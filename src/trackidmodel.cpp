#include "trackidmodel.h"

#include <algorithm>
#include <unordered_set>

int TrackIdModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(ids_.size());
}

QVariant TrackIdModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || std::size_t(index.row()) >= ids_.size()) return {};
    if (role == IdRole || role == Qt::DisplayRole) return ids_[std::size_t(index.row())];
    return {};
}

QHash<int, QByteArray> TrackIdModel::roleNames() const
{
    // Named so delegates keep their `required property var modelData`.
    return {{IdRole, "modelData"}};
}

void TrackIdModel::setIds(const std::vector<int>& wanted)
{
    if (wanted == ids_) return;
    // 1. Remove ids that are gone, last first so earlier rows keep their indices.
    const std::unordered_set<int> keep(wanted.begin(), wanted.end());
    for (int row = int(ids_.size()) - 1; row >= 0; --row) {
        if (keep.count(ids_[std::size_t(row)])) continue;
        int first = row;
        while (first > 0 && !keep.count(ids_[std::size_t(first - 1)])) --first;
        beginRemoveRows({}, first, row);
        ids_.erase(ids_.begin() + first, ids_.begin() + row + 1);
        endRemoveRows();
        row = first;
    }
    // 2. Walk the wanted order: move existing ids into place, insert new ones.
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (i < ids_.size() && ids_[i] == wanted[i]) continue;
        const auto found = std::find(ids_.begin() + std::ptrdiff_t(i), ids_.end(), wanted[i]);
        if (found != ids_.end()) {
            const int from = int(found - ids_.begin());
            beginMoveRows({}, from, from, {}, int(i));
            const int id = *found;
            ids_.erase(found);
            ids_.insert(ids_.begin() + std::ptrdiff_t(i), id);
            endMoveRows();
        } else {
            beginInsertRows({}, int(i), int(i));
            ids_.insert(ids_.begin() + std::ptrdiff_t(i), wanted[i]);
            endInsertRows();
        }
    }
}
