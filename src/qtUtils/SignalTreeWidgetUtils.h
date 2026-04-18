#ifndef SEGMENTPUZZLER_SIGNALTREEWIDGETUTILS_H
#define SEGMENTPUZZLER_SIGNALTREEWIDGETUTILS_H

#include <QTreeWidgetItem>
#include <stdexcept>

namespace signal_tree {

enum class RowKind : int {
    Root = 0,
    Color,
    Norm,
    Alpha,
    DataType
};

constexpr int SignalIndexRole = Qt::UserRole;
constexpr int RowKindRole = Qt::UserRole + 1;

inline QTreeWidgetItem *topLevelSignalItem(QTreeWidgetItem *item) {
    return item != nullptr && item->parent() != nullptr ? item->parent() : item;
}

inline RowKind rowKind(QTreeWidgetItem *item) {
    if (item == nullptr) {
        throw std::logic_error("signal tree item not found");
    }

    const QVariant kindData = item->data(0, RowKindRole);
    if (!kindData.isValid()) {
        return item->parent() == nullptr ? RowKind::Root : RowKind::DataType;
    }

    return static_cast<RowKind>(kindData.toInt());
}

inline size_t signalIndex(QTreeWidgetItem *item) {
    QTreeWidgetItem *rootItem = topLevelSignalItem(item);
    if (rootItem == nullptr) {
        throw std::logic_error("signal tree item not found");
    }

    const QVariant signalIndexData = rootItem->data(0, SignalIndexRole);
    if (!signalIndexData.isValid()) {
        throw std::logic_error("signal index not found");
    }

    return static_cast<size_t>(signalIndexData.toULongLong());
}

} // namespace signal_tree

#endif // SEGMENTPUZZLER_SIGNALTREEWIDGETUTILS_H
