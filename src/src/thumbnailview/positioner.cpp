/*
    SPDX-FileCopyrightText: 2014 Eike Hein <hein@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later

    SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "positioner.h"
#include "thumbnailmodel.h"
#include <QDebug>

#include <QTimer>
#include <QPoint>
#include <QUrl>

#include <cstdlib>

Positioner::Positioner(QObject *parent)
    : QAbstractItemModel(parent)
    , m_enabled(false)
    , m_thumbnialModel(nullptr)
    , m_perStripe(0)
    , m_ignoreNextTransaction(false)
    , m_deferApplyPositions(false)
    , m_updatePositionsTimer(new QTimer(this))
{
    qDebug() << "Initializing Positioner";
    m_updatePositionsTimer->setSingleShot(true);
    m_updatePositionsTimer->setInterval(0);
    connect(m_updatePositionsTimer, &QTimer::timeout, this, &Positioner::updatePositions);
}

Positioner::~Positioner()
{
    qDebug() << "Destroying Positioner";
}

bool Positioner::enabled() const
{
    return m_enabled;
}

void Positioner::setEnabled(bool enabled)
{
    if (m_enabled != enabled) {
        qDebug() << "Setting enabled from" << m_enabled << "to" << enabled;
        m_enabled = enabled;

        beginResetModel();

        if (enabled && m_thumbnialModel) {
            initMaps();
        }

        endResetModel();

        Q_EMIT enabledChanged();

        if (!enabled) {
            m_updatePositionsTimer->start();
        }
    }
}

ThumbnailModel *Positioner::thumbnailModel() const
{
    return m_thumbnialModel;
}

void Positioner::setThumbnailModel(ThumbnailModel *thumbnailModel)
{
    if (m_thumbnialModel != thumbnailModel) {
        qDebug() << "Setting thumbnail model from" << m_thumbnialModel << "to" << thumbnailModel;
        beginResetModel();

        if (m_thumbnialModel) {
            disconnectSignals(m_thumbnialModel);
        }

        m_thumbnialModel = thumbnailModel;

        if (m_thumbnialModel) {
            connectSignals(m_thumbnialModel);

            if (m_enabled) {
                initMaps();
            }
        }

        endResetModel();

        Q_EMIT sortModelChanged();
    }
}

int Positioner::perStripe() const
{
    return m_perStripe;
}

void Positioner::setPerStripe(int perStripe)
{
    if (m_perStripe != perStripe) {
        qDebug() << "Setting per stripe from" << m_perStripe << "to" << perStripe;
        m_perStripe = perStripe;

        Q_EMIT perStripeChanged();

        if (m_enabled && perStripe > 0 && !m_proxyToSource.isEmpty()) {
            applyPositions();
        }
    }
}

QStringList Positioner::positions() const
{
    return m_positions;
}

void Positioner::setPositions(const QStringList &positions)
{
    if (m_positions != positions) {
        qDebug() << "Setting positions, count:" << positions.size();
        m_positions = positions;

        Q_EMIT positionsChanged();

        // Defer applying positions until listing completes.
        if (m_thumbnialModel->status() == ThumbnailModel::Listing) {
            qDebug() << "Deferring position application - model is listing";
            m_deferApplyPositions = true;
        } else {
            applyPositions();
        }
    }
}

int Positioner::map(int row) const
{
    if (m_enabled && m_thumbnialModel) {
        return m_proxyToSource.value(row, -1);
    }

    return row;
}

QVariantList Positioner::maps(QVariantList rows) const
{
    QVariantList varList;
    varList.clear();
    if (m_enabled && m_thumbnialModel) {
        int iRow = -1;
        for (const auto &var : rows) {
            iRow = var.toInt();
            if (iRow < 0)
                continue;
            varList.push_back(m_proxyToSource.value(iRow, -1));
        }
        qDebug() << "Mapping" << rows.size() << "rows to" << varList.size() << "mapped rows";
    }

    return varList;
}

int Positioner::nearestItem(int currentIndex, Qt::ArrowType direction)
{
    if (!m_enabled || currentIndex >= rowCount()) {
        qDebug() << "Cannot find nearest item - disabled or invalid index:" << currentIndex;
        return -1;
    }

    if (currentIndex < 0) {
        qDebug() << "Current index < 0, returning first row";
        return firstRow();
    }

    int hDirection = 0;
    int vDirection = 0;

    switch (direction) {
    case Qt::LeftArrow:
        hDirection = -1;
        break;
    case Qt::RightArrow:
        hDirection = 1;
        break;
    case Qt::UpArrow:
        vDirection = -1;
        break;
    case Qt::DownArrow:
        vDirection = 1;
        break;
    default:
        return -1;
    }

    QList<int> rows(m_proxyToSource.keys());
    std::sort(rows.begin(), rows.end());

    int nearestItem = -1;
    const QPoint currentPos(currentIndex % m_perStripe, currentIndex / m_perStripe);
    int lastDistance = -1;
    int distance = 0;

    for (const int row : rows) {
        const QPoint pos(row % m_perStripe, row / m_perStripe);

        if (row == currentIndex) {
            continue;
        }

        if (hDirection == 0) {
            if (vDirection * pos.y() > vDirection * currentPos.y()) {
                distance = (pos - currentPos).manhattanLength();

                if (nearestItem == -1 || distance < lastDistance || (distance == lastDistance && pos.x() == currentPos.x())) {
                    nearestItem = row;
                    lastDistance = distance;
                }
            }
        } else if (vDirection == 0) {
            if (hDirection * pos.x() > hDirection * currentPos.x()) {
                distance = (pos - currentPos).manhattanLength();

                if (nearestItem == -1 || distance < lastDistance || (distance == lastDistance && pos.y() == currentPos.y())) {
                    nearestItem = row;
                    lastDistance = distance;
                }
            }
        }
    }

    qDebug() << "Found nearest item:" << nearestItem << "from current:" << currentIndex << "direction:" << direction;
    return nearestItem;
}

bool Positioner::isBlank(int row) const
{
    return false;
}

int Positioner::indexForUrl(const QUrl &url) const
{
    if (!m_thumbnialModel) {
        qDebug() << "Cannot find index for URL - no thumbnail model";
        return -1;
    }

    const QString &name = url.fileName();
    qDebug() << "Finding index for URL:" << name;

    int sourceIndex = -1;

    // TODO Optimize.
    for (int i = 0; i < m_thumbnialModel->rowCount(); ++i) {
        if (m_thumbnialModel->data(m_thumbnialModel->index(i, 0), Roles::FileNameRole).toString() == name) {
            sourceIndex = i;
            break;
        }
    }

    return m_sourceToProxy.value(sourceIndex, -1);
}

void Positioner::setRangeSelected(int anchor, int to)
{
    if (!m_thumbnialModel) {
        qDebug() << "Cannot set range selection - no thumbnail model";
        return;
    }

    qDebug() << "Setting range selection from" << anchor << "to" << to;
    if (m_enabled) {
        QVariantList indices;

        for (int i = qMin(anchor, to); i <= qMax(anchor, to); ++i) {
            if (m_proxyToSource.contains(i)) {
                indices.append(m_proxyToSource.value(i));
            }
        }

        if (!indices.isEmpty()) {
            m_thumbnialModel->updateSelection(indices, false);
        }
    } else {
        m_thumbnialModel->setRangeSelected(anchor, to);
    }
}

QHash<int, QByteArray> Positioner::roleNames() const
{
    if (!m_thumbnialModel)
        return QHash<int, QByteArray>();
    return m_thumbnialModel->roleNames();
}

QModelIndex Positioner::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return QModelIndex();
    }

    return createIndex(row, column);
}

QModelIndex Positioner::parent(const QModelIndex &index) const
{
    if (m_thumbnialModel) {
        m_thumbnialModel->parent(index);
    }

    return QModelIndex();
}

QVariant Positioner::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    if (m_thumbnialModel) {
        if (m_enabled) {
            if (m_proxyToSource.contains(index.row())) {
                return m_thumbnialModel->data(m_thumbnialModel->index(m_proxyToSource.value(index.row()), 0), role);
            } else if (role == Roles::BlankRole) {
                return true;
            }
        } else {
            return m_thumbnialModel->data(m_thumbnialModel->index(index.row(), 0), role);
        }
    }

    return QVariant();
}

int Positioner::rowCount(const QModelIndex &parent) const
{
    if (m_thumbnialModel) {
        if (m_enabled) {
            if (parent.isValid()) {
                return 0;
            } else {
                return lastRow() + 1;
            }
        } else {
            return m_thumbnialModel->rowCount(parent);
        }
    }

    return 0;
}

int Positioner::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)

    if (m_thumbnialModel) {
        return 1;
    }

    return 0;
}

void Positioner::reset()
{
    qDebug() << "Resetting positioner";
    beginResetModel();

    initMaps();

    endResetModel();

    m_positions = QStringList();
    Q_EMIT positionsChanged();
}

int Positioner::move(const QVariantList &moves)
{
    // Don't allow moves while listing.
    if (m_thumbnialModel->status() == ThumbnailModel::Listing) {
        qDebug() << "Deferring move - model is listing";
        m_deferMovePositions.append(moves);
        return -1;
    }

    qDebug() << "Processing" << moves.size() << "moves";
    QVector<int> fromIndices;
    QVector<int> toIndices;
    QVariantList sourceRows;

    for (int i = 0; i < moves.count(); ++i) {
        const int isFrom = (i % 2 == 0);
        const int v = moves[i].toInt();

        if (isFrom) {
            if (m_proxyToSource.contains(v)) {
                sourceRows.append(m_proxyToSource.value(v));
            } else {
                sourceRows.append(-1);
            }
        }

        (isFrom ? fromIndices : toIndices).append(v);
    }

    const int oldCount = rowCount();

    for (int i = 0; i < fromIndices.count(); ++i) {
        const int from = fromIndices[i];
        int to = toIndices[i];
        const int sourceRow = sourceRows[i].toInt();

        if (sourceRow == -1 || from == to) {
            continue;
        }

        if (to == -1) {
            to = firstFreeRow();

            if (to == -1) {
                to = lastRow() + 1;
            }
        }

        if (!fromIndices.contains(to) && !isBlank(to)) {
            /* find the next blank space
             * we won't be happy if we're moving two icons to the same place
             */
            while ((!isBlank(to) && from != to) || toIndices.contains(to)) {
                to++;
            }
        }

        toIndices[i] = to;

        if (!toIndices.contains(from)) {
            m_proxyToSource.remove(from);
        }

        updateMaps(to, sourceRow);

        const QModelIndex &fromIdx = index(from, 0);
        Q_EMIT dataChanged(fromIdx, fromIdx);

        if (to < oldCount) {
            const QModelIndex &toIdx = index(to, 0);
            Q_EMIT dataChanged(toIdx, toIdx);
        }
    }

    const int newCount = rowCount();

    if (newCount > oldCount) {
        if (m_beginInsertRowsCalled) {
            endInsertRows();
            m_beginInsertRowsCalled = false;
        }
        beginInsertRows(QModelIndex(), oldCount, newCount - 1);
        endInsertRows();
    }

    if (newCount < oldCount) {
        beginRemoveRows(QModelIndex(), newCount, oldCount - 1);
        endRemoveRows();
    }

    m_thumbnialModel->updateSelection(sourceRows, true);

    m_updatePositionsTimer->start();

    qDebug() << "Move completed, new count:" << newCount << "old count:" << oldCount;
    return toIndices.constFirst();
}

void Positioner::updatePositions()
{
    QStringList positions;

    if (m_enabled && !m_proxyToSource.isEmpty() && m_perStripe > 0) {
        qDebug() << "Updating positions for" << m_proxyToSource.size() << "items";
        positions.append(QString::number((1 + ((rowCount() - 1) / m_perStripe))));
        positions.append(QString::number(m_perStripe));

        QHashIterator<int, int> it(m_proxyToSource);

        while (it.hasNext()) {
            it.next();

            const QString &name = m_thumbnialModel->data(m_thumbnialModel->index(it.value(), 0), Roles::UrlRole).toString();

            if (name.isEmpty()) {
                qWarning() << "Empty name found while updating positions";
                return;
            }

            positions.append(name);
            positions.append(QString::number(qMax(0, it.key() / m_perStripe)));
            positions.append(QString::number(qMax(0, it.key() % m_perStripe)));
        }
    }

    if (positions != m_positions) {
        qDebug() << "Positions updated, new count:" << positions.size();
        m_positions = positions;

        Q_EMIT positionsChanged();
    }
}

void Positioner::sourceStatusChanged()
{
    if (m_deferApplyPositions && m_thumbnialModel->status() != ThumbnailModel::Listing) {
        qDebug() << "Applying deferred positions";
        applyPositions();
    }

    if (m_deferMovePositions.count() && m_thumbnialModel->status() != ThumbnailModel::Listing) {
        qDebug() << "Processing" << m_deferMovePositions.count() << "deferred moves";
        move(m_deferMovePositions);
        m_deferMovePositions.clear();
    }
}

void Positioner::sourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles)
{
    if (m_enabled) {
        qDebug() << "Source data changed from row" << topLeft.row() << "to" << bottomRight.row();
        int start = topLeft.row();
        int end = bottomRight.row();

        for (int i = start; i <= end; ++i) {
            if (m_sourceToProxy.contains(i)) {
                const QModelIndex &idx = index(m_sourceToProxy.value(i), 0);

                Q_EMIT dataChanged(idx, idx);
            }
        }
    } else {
        Q_EMIT dataChanged(topLeft, bottomRight, roles);
    }
}

void Positioner::sourceModelAboutToBeReset()
{
    beginResetModel();
}

void Positioner::sourceModelReset()
{
    if (m_enabled) {
        initMaps();
    }

    endResetModel();
}

void Positioner::sourceRowsAboutToBeInserted(const QModelIndex &parent, int start, int end)
{
    if (m_enabled) {
        qDebug() << "Source rows about to be inserted from" << start << "to" << end;
        // Don't insert yet if we're waiting for listing to complete to apply
        // initial positions;
        if (m_deferApplyPositions) {
            return;
        } else if (m_proxyToSource.isEmpty()) {
            beginInsertRows(parent, start, end);
            m_beginInsertRowsCalled = true;

            initMaps(end + 1);

            return;
        }

        // When new rows are inserted, they might go in the beginning or in the middle.
        // In this case we must update first the existing proxy->source and source->proxy
        // mapping, otherwise the proxy items will point to the wrong source item.
        int count = end - start + 1;
        m_sourceToProxy.clear();
        for (auto it = m_proxyToSource.begin(); it != m_proxyToSource.end(); ++it) {
            int sourceIdx = *it;
            if (sourceIdx >= start) {
                *it += count;
            }
            m_sourceToProxy[*it] = it.key();
        }

        int free = -1;
        int rest = -1;

        for (int i = start; i <= end; ++i) {
            free = firstFreeRow();

            if (free != -1) {
                updateMaps(free, i);
                m_pendingChanges << createIndex(free, 0);
            } else {
                rest = i;
                break;
            }
        }

        if (rest != -1) {
            int firstNew = lastRow() + 1;
            int remainder = (end - rest);

            beginInsertRows(parent, firstNew, firstNew + remainder);
            m_beginInsertRowsCalled = true;

            for (int i = 0; i <= remainder; ++i) {
                updateMaps(firstNew + i, rest + i);
            }
        } else {
            m_ignoreNextTransaction = true;
        }
    } else {
        beginInsertRows(parent, start, end);
        beginInsertRows(parent, start, end);
        m_beginInsertRowsCalled = true;
    }
}

void Positioner::sourceRowsAboutToBeMoved(const QModelIndex &sourceParent,
                                          int sourceStart,
                                          int sourceEnd,
                                          const QModelIndex &destinationParent,
                                          int destinationRow)
{
    beginMoveRows(sourceParent, sourceStart, sourceEnd, destinationParent, destinationRow);
}

void Positioner::sourceRowsAboutToBeRemoved(const QModelIndex &parent, int first, int last)
{
    if (m_enabled) {
        qDebug() << "Source rows about to be removed from" << first << "to" << last;
        int oldLast = lastRow();

        for (int i = first; i <= last; ++i) {
            int proxyRow = m_sourceToProxy.take(i);
            m_proxyToSource.remove(proxyRow);
            m_pendingChanges << createIndex(proxyRow, 0);
        }

        QHash<int, int> newProxyToSource;
        QHash<int, int> newSourceToProxy;
        QHashIterator<int, int> it(m_sourceToProxy);
        int delta = std::abs(first - last) + 1;

        while (it.hasNext()) {
            it.next();

            if (it.key() > last) {
                newProxyToSource.insert(it.value(), it.key() - delta);
                newSourceToProxy.insert(it.key() - delta, it.value());
            } else {
                newProxyToSource.insert(it.value(), it.key());
                newSourceToProxy.insert(it.key(), it.value());
            }
        }

        m_proxyToSource = newProxyToSource;
        m_sourceToProxy = newSourceToProxy;

        int newLast = lastRow();

        if (oldLast > newLast) {
            int diff = oldLast - newLast;
            beginRemoveRows(QModelIndex(), ((oldLast - diff) + 1), oldLast);
        } else {
            m_ignoreNextTransaction = true;
        }
    } else {
        beginRemoveRows(parent, first, last);
    }
}

void Positioner::sourceLayoutAboutToBeChanged(const QList<QPersistentModelIndex> &parents, QAbstractItemModel::LayoutChangeHint hint)
{
    Q_UNUSED(parents)

    Q_EMIT layoutAboutToBeChanged(QList<QPersistentModelIndex>(), hint);
}

void Positioner::sourceRowsInserted(const QModelIndex &parent, int first, int last)
{
    Q_UNUSED(parent)
    Q_UNUSED(first)
    Q_UNUSED(last)

    if (!m_ignoreNextTransaction) {
        if (m_beginInsertRowsCalled) {
            endInsertRows();
            m_beginInsertRowsCalled = false;
        }
    } else {
        m_ignoreNextTransaction = false;
    }

    flushPendingChanges();

    // Don't generate new positions data if we're waiting for listing to
    // complete to apply initial positions.
    if (!m_deferApplyPositions) {
        m_updatePositionsTimer->start();
    }
}

void Positioner::sourceRowsMoved(const QModelIndex &sourceParent, int sourceStart, int sourceEnd, const QModelIndex &destinationParent, int destinationRow)
{
    Q_UNUSED(sourceParent)
    Q_UNUSED(sourceStart)
    Q_UNUSED(sourceEnd)
    Q_UNUSED(destinationParent)
    Q_UNUSED(destinationRow)

    endMoveRows();
}

void Positioner::sourceRowsRemoved(const QModelIndex &parent, int first, int last)
{
    Q_UNUSED(parent)
    Q_UNUSED(first)
    Q_UNUSED(last)

    if (!m_ignoreNextTransaction) {
        Q_EMIT endRemoveRows();
    } else {
        m_ignoreNextTransaction = false;
    }

    flushPendingChanges();

    m_updatePositionsTimer->start();
}

void Positioner::sourceLayoutChanged(const QList<QPersistentModelIndex> &parents, QAbstractItemModel::LayoutChangeHint hint)
{
    Q_UNUSED(parents)

    if (m_enabled) {
        initMaps();
    }

    Q_EMIT layoutChanged(QList<QPersistentModelIndex>(), hint);
}

void Positioner::initMaps(int size)
{
    qDebug() << "Initializing maps with size:" << size;
    m_proxyToSource.clear();
    m_sourceToProxy.clear();

    if (size == -1) {
        size = m_thumbnialModel->rowCount();
    }

    if (!size) {
        return;
    }

    for (int i = 0; i < size; ++i) {
        updateMaps(i, i);
    }
}

void Positioner::updateMaps(int proxyIndex, int sourceIndex)
{
    m_proxyToSource.insert(proxyIndex, sourceIndex);
    m_sourceToProxy.insert(sourceIndex, proxyIndex);
}

int Positioner::firstRow() const
{
    if (!m_proxyToSource.isEmpty()) {
        QList<int> keys(m_proxyToSource.keys());
        std::sort(keys.begin(), keys.end());

        return keys.first();
    }

    return -1;
}

int Positioner::lastRow() const
{
    if (!m_proxyToSource.isEmpty()) {
        QList<int> keys(m_proxyToSource.keys());
        std::sort(keys.begin(), keys.end());
        return keys.last();
    }

    return 0;
}

int Positioner::firstFreeRow() const
{
    if (!m_proxyToSource.isEmpty()) {
        int last = lastRow();

        for (int i = 0; i <= last; ++i) {
            if (!m_proxyToSource.contains(i)) {
                return i;
            }
        }
    }

    return -1;
}

void Positioner::applyPositions()
{
    // We were called while the source model is listing. Defer applying positions
    // until listing completes.
    if (m_thumbnialModel->status() == ThumbnailModel::Listing) {
        qDebug() << "Deferring position application - model is listing";
        m_deferApplyPositions = true;

        return;
    }

    if (m_positions.size() < 5) {
        // We were waiting for listing to complete before proxying source rows,
        // but we don't have positions to apply. Reset to populate.
        if (m_deferApplyPositions) {
            qDebug() << "No positions to apply, resetting";
            m_deferApplyPositions = false;
            reset();
        }

        return;
    }

    qDebug() << "Applying positions, count:" << m_positions.size();
    beginResetModel();

    m_proxyToSource.clear();
    m_sourceToProxy.clear();

    const QStringList &positions = m_positions.mid(2);

    if (positions.count() % 3 != 0) {
        qWarning() << "Invalid positions format - count not divisible by 3:" << positions.count();
        return;
    }

    QHash<QString, int> sourceIndices;

    for (int i = 0; i < m_thumbnialModel->rowCount(); ++i) {
        sourceIndices.insert(m_thumbnialModel->data(m_thumbnialModel->index(i, 0), Roles::UrlRole).toString(), i);
    }

    QString name;
    int stripe = -1;
    int pos = -1;
    int sourceIndex = -1;
    int index = -1;
    bool ok = false;
    int offset = 0;

    // Restore positions for items that still fit.
    for (int i = 0; i < positions.count() / 3; ++i) {
        offset = i * 3;
        pos = positions.at(offset + 2).toInt(&ok);
        if (!ok) {
            qWarning() << "Invalid position value at offset" << offset + 2;
            return;
        }

        if (pos <= m_perStripe) {
            name = positions.at(offset);
            stripe = positions.at(offset + 1).toInt(&ok);
            if (!ok) {
                qWarning() << "Invalid stripe value at offset" << offset + 1;
                return;
            }

            if (!sourceIndices.contains(name)) {
                continue;
            } else {
                sourceIndex = sourceIndices.value(name);
            }

            index = (stripe * m_perStripe) + pos;

            if (m_proxyToSource.contains(index)) {
                continue;
            }

            updateMaps(index, sourceIndex);
            sourceIndices.remove(name);
        }
    }

    // Find new positions for items that didn't fit.
    for (int i = 0; i < positions.count() / 3; ++i) {
        offset = i * 3;
        pos = positions.at(offset + 2).toInt(&ok);
        if (!ok) {
            qWarning() << "Invalid position value at offset" << offset + 2;
            return;
        }

        if (pos > m_perStripe) {
            name = positions.at(offset);

            if (!sourceIndices.contains(name)) {
                continue;
            } else {
                sourceIndex = sourceIndices.take(name);
            }

            index = firstFreeRow();

            if (index == -1) {
                index = lastRow() + 1;
            }

            updateMaps(index, sourceIndex);
        }
    }

    QHashIterator<QString, int> it(sourceIndices);

    // Find positions for new source items we don't have records for.
    while (it.hasNext()) {
        it.next();

        index = firstFreeRow();

        if (index == -1) {
            index = lastRow() + 1;
        }

        updateMaps(index, it.value());
    }

    endResetModel();

    m_deferApplyPositions = false;

    m_updatePositionsTimer->start();
}

void Positioner::flushPendingChanges()
{
    if (m_pendingChanges.isEmpty()) {
        return;
    }

    qDebug() << "Flushing" << m_pendingChanges.size() << "pending changes";
    int last = lastRow();

    for (const QModelIndex &index : m_pendingChanges) {
        if (index.row() <= last) {
            Q_EMIT dataChanged(index, index);
        }
    }

    m_pendingChanges.clear();
}

void Positioner::connectSignals(ThumbnailModel *model)
{
    qDebug() << "Connecting signals to thumbnail model";
    connect(model, &QAbstractItemModel::dataChanged, this, &Positioner::sourceDataChanged, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, &Positioner::sourceRowsAboutToBeInserted, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::rowsAboutToBeMoved, this, &Positioner::sourceRowsAboutToBeMoved, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, &Positioner::sourceRowsAboutToBeRemoved, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, &Positioner::sourceLayoutAboutToBeChanged, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::rowsInserted, this, &Positioner::sourceRowsInserted, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::rowsMoved, this, &Positioner::sourceRowsMoved, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &Positioner::sourceRowsRemoved, Qt::UniqueConnection);
    connect(model, &QAbstractItemModel::layoutChanged, this, &Positioner::sourceLayoutChanged, Qt::UniqueConnection);
    connect(m_thumbnialModel, &ThumbnailModel::srcModelReseted, this, &Positioner::reset, Qt::UniqueConnection);
    connect(m_thumbnialModel, &ThumbnailModel::statusChanged, this, &Positioner::sourceStatusChanged, Qt::UniqueConnection);
}

void Positioner::disconnectSignals(ThumbnailModel *model)
{
    qDebug() << "Disconnecting signals from thumbnail model";
    disconnect(model, &QAbstractItemModel::dataChanged, this, &Positioner::sourceDataChanged);
    disconnect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, &Positioner::sourceRowsAboutToBeInserted);
    disconnect(model, &QAbstractItemModel::rowsAboutToBeMoved, this, &Positioner::sourceRowsAboutToBeMoved);
    disconnect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, &Positioner::sourceRowsAboutToBeRemoved);
    disconnect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, &Positioner::sourceLayoutAboutToBeChanged);
    disconnect(model, &QAbstractItemModel::rowsInserted, this, &Positioner::sourceRowsInserted);
    disconnect(model, &QAbstractItemModel::rowsMoved, this, &Positioner::sourceRowsMoved);
    disconnect(model, &QAbstractItemModel::rowsRemoved, this, &Positioner::sourceRowsRemoved);
    disconnect(model, &QAbstractItemModel::layoutChanged, this, &Positioner::sourceLayoutChanged);
    disconnect(m_thumbnialModel, &ThumbnailModel::srcModelReseted, this, &Positioner::reset);
    disconnect(m_thumbnialModel, &ThumbnailModel::statusChanged, this, &Positioner::sourceStatusChanged);
}
