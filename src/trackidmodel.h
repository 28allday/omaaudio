#pragma once

#include <QAbstractListModel>
#include <vector>

// The session's track ids as a list model that reports exactly what changed —
// inserted, removed and moved rows — so a view of tracks (lanes, mixer strips,
// the file list) creates one delegate when one track is added instead of
// rebuilding every one. A plain list model would reset the whole view on each change.
class TrackIdModel : public QAbstractListModel {
    Q_OBJECT
public:
    using QAbstractListModel::QAbstractListModel;
    enum Roles { IdRole = Qt::UserRole + 1 };
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // Brings the model to `ids` with the fewest row operations.
    void setIds(const std::vector<int>& ids);
    [[nodiscard]] const std::vector<int>& ids() const noexcept { return ids_; }
private:
    std::vector<int> ids_;
};
