#include "CImageTreeModel.h"

#include <QApplication>
#include <QIcon>
#include <QLabel>
#include <QPropertyAnimation>
#include <QStyle>

CImageTreeModel::CImageTreeModel()
{
    rootItem = new CImageTreeItem({ QString(""), tr("Name"), tr("Size"), tr("Resolution"), tr("Saved"), tr("Info") });
}

CImageTreeModel::~CImageTreeModel()
{
    delete rootItem;
}

QModelIndex CImageTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    CImageTreeItem* parentItem;

    if (!parent.isValid()) {
        parentItem = rootItem;
    } else {
        parentItem = static_cast<CImageTreeItem*>(parent.internalPointer());
    }

    CImageTreeItem* childItem = parentItem->child(row);
    if (childItem) {
        return createIndex(row, column, childItem);
    }
    return QModelIndex();
}

QModelIndex CImageTreeModel::parent(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    CImageTreeItem* childItem = static_cast<CImageTreeItem*>(index.internalPointer());
    CImageTreeItem* parentItem = childItem->parentItem();

    if (parentItem == rootItem) {

        return QModelIndex();
    }
    return createIndex(parentItem->row(), 0, parentItem);
}

int CImageTreeModel::rowCount(const QModelIndex& parent) const
{
    CImageTreeItem* parentItem;

    if (parent.column() > 0) {
        return 0;
    }

    if (!parent.isValid()) {
        parentItem = rootItem;
    } else {
        parentItem = static_cast<CImageTreeItem*>(parent.internalPointer());
    }

    return parentItem->childCount();
}

int CImageTreeModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return static_cast<CImageTreeItem*>(parent.internalPointer())->columnCount();
    }
    return rootItem->columnCount();
}

bool CImageTreeModel::removeRows(int row, int count, const QModelIndex& parent)
{
    beginRemoveRows(parent, row, row + count - 1);

    for (int i = 0; i < count; i++) {
        this->rootItem->removeChildAt(row);
    }

    endRemoveRows();
    emit itemsChanged();
    return true;
}

void CImageTreeModel::appendItems(QList<CImage*> imageList, QString folder)
{
    this->baseFolder = folder;
    this->setupModelData(imageList, rootItem);
}

void CImageTreeModel::setupModelData(const QList<CImage*> imageList, CImageTreeItem* parent)
{
    QListIterator<CImage*> iterator(imageList);
    this->beginInsertRows(QModelIndex(), this->rowCount(), this->rowCount() + imageList.count() - 1);
    while (iterator.hasNext()) {
        CImage* nextImage = iterator.next();
        auto* cImageTreeItem = new CImageTreeItem(nextImage, parent);
        parent->appendChild(cImageTreeItem);
    }
    endInsertRows();
    emit itemsChanged();
}

void CImageTreeModel::emitDataChanged(int row)
{
    QModelIndex modelIndexStart = this->index(row, 0);
    QModelIndex modelIndexEnd = this->index(row, this->columnCount() - 1);
    emit dataChanged(modelIndexStart, modelIndexEnd);
}

CImageTreeItem* CImageTreeModel::getRootItem() const
{
    return rootItem;
}

bool CImageTreeModel::contains(CImage* cImage)
{
    QVectorIterator<CImageTreeItem*> it(this->rootItem->children());
    while (it.hasNext()) {
        if (*it.next()->getCImage() == *cImage) {
            return true;
        }
    }
    return false;
}

QVariant CImageTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    if (role != Qt::DisplayRole && role != Qt::DecorationRole && role != Qt::CheckStateRole) {
        return QVariant();
    }

    CImageTreeItem* item = static_cast<CImageTreeItem*>(index.internalPointer());

    if (role == Qt::CheckStateRole) {
        // Only the checkbox column has a check state; every other column must
        // return an invalid QVariant, otherwise Qt draws a checkbox in them too.
        if (index.column() == CImageColumns::CHECKBOX_COLUMN) {
            return item->isChecked() ? Qt::Checked : Qt::Unchecked;
        }
        return QVariant();
    }

    if (role == Qt::DisplayRole && index.column() == CImageColumns::NAME_COLUMN) {
        // Little hack to get the default application text color to apply transparency to the base folder text
        QColor defaultColor = QApplication::palette().text().color();
        if (role & QStyle::State_Selected) {
            defaultColor = QApplication::palette().highlightedText().color();
        }
        QString fullPath = item->getCImage()->getFullPath();
        QString computedBaseFolder = fullPath.remove(baseFolder + "/");
        QString baseFolderWithoutName = computedBaseFolder.remove(item->getCImage()->getFileName());
        QString rgbaString = "rgba(" + QString::number(defaultColor.red()) + "," + QString::number(defaultColor.green()) + "," + QString::number(defaultColor.blue()) + ",.6);";
        return "<span style=\"color:" + rgbaString + ";\">" + baseFolderWithoutName + "</span>" + item->getCImage()->getFileName();
    }

    if (role == Qt::DecorationRole && index.column() == CImageColumns::NAME_COLUMN) {
        CImageStatus status = item->getCImage()->getStatus();
        if (status == CImageStatus::COMPRESSED) {
            return QIcon(":/icons/compression_statuses/compressed.svg").pixmap(16, 16);
        } else if (status == CImageStatus::ERROR) {
            return QIcon(":/icons/compression_statuses/error.svg").pixmap(16, 16);
        } else if (status == CImageStatus::WARNING) {
            return QIcon(":/icons/compression_statuses/warning.svg").pixmap(16, 16);
        } else if (status == CImageStatus::COMPRESSING) {
            return QIcon(":/icons/compression_statuses/compressing.svg").pixmap(16, 16);
        } else {
            return QIcon(":/icons/compression_statuses/uncompressed.svg").pixmap(16, 16);
        }
    }

    if (role == Qt::DisplayRole && index.column() == CImageColumns::SIZE_COLUMN) {
        return item->getCImage()->getRichFormattedSize();
    }

    if (role == Qt::DisplayRole && index.column() == CImageColumns::RESOLUTION_COLUMN) {
        return item->getCImage()->getRichResolution();
    }

    if (role == Qt::DisplayRole && index.column() == CImageColumns::RATIO_COLUMN) {
        return item->getCImage()->getRichFormattedSavedRatio();
    }

    if (role == Qt::DisplayRole && index.column() == CImageColumns::INFO_COLUMN) {
        return item->getCImage()->getFormattedStatus();
    }

    return item->data(index.column());
}

Qt::ItemFlags CImageTreeModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);
    if (index.column() == CImageColumns::CHECKBOX_COLUMN) {
        defaultFlags |= Qt::ItemIsUserCheckable;
    }
    return defaultFlags;
}

bool CImageTreeModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid()) {
        return false;
    }

    if (role == Qt::CheckStateRole && index.column() == CImageColumns::CHECKBOX_COLUMN) {
        CImageTreeItem* item = static_cast<CImageTreeItem*>(index.internalPointer());
        item->setChecked(value.toInt() == Qt::Checked);
        emit dataChanged(index, index, { Qt::CheckStateRole });
        emit checkedItemsChanged();
        return true;
    }

    return false;
}

void CImageTreeModel::setAllChecked(bool checked)
{
    const QVector<CImageTreeItem*> items = rootItem->children();
    if (items.isEmpty()) {
        return;
    }
    for (CImageTreeItem* item : items) {
        item->setChecked(checked);
    }
    QModelIndex topLeft = this->index(0, CImageColumns::CHECKBOX_COLUMN);
    QModelIndex bottomRight = this->index(items.count() - 1, CImageColumns::CHECKBOX_COLUMN);
    emit dataChanged(topLeft, bottomRight, { Qt::CheckStateRole });
    emit checkedItemsChanged();
}

int CImageTreeModel::checkedItemsCount() const
{
    int count = 0;
    const QVector<CImageTreeItem*> items = rootItem->children();
    for (CImageTreeItem* item : items) {
        if (item->isChecked()) {
            count++;
        }
    }
    return count;
}

QVariant CImageTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        return rootItem->data(section);
    }

    return QVariant();
}

double CImageTreeModel::compressedItemsSize() const
{
    QVectorIterator<CImageTreeItem*> itemsIterator(rootItem->children());
    double totalSize = 0;
    while (itemsIterator.hasNext()) {
        auto item = itemsIterator.next();
        auto size = (double)item->getCImage()->getCompressedSize();
        totalSize += size;
    }
    return totalSize;
}

double CImageTreeModel::originalItemsSize() const
{
    QVectorIterator<CImageTreeItem*> itemsIterator(rootItem->children());
    double totalSize = 0;
    while (itemsIterator.hasNext()) {
        auto item = itemsIterator.next();
        auto size = (double)item->getCImage()->getOriginalSize();
        totalSize += size;
    }
    return totalSize;
}

double CImageTreeModel::checkedCompressedItemsSize() const
{
    QVectorIterator<CImageTreeItem*> itemsIterator(rootItem->children());
    double totalSize = 0;
    while (itemsIterator.hasNext()) {
        auto item = itemsIterator.next();
        if (!item->isChecked()) {
            continue;
        }
        totalSize += (double)item->getCImage()->getCompressedSize();
    }
    return totalSize;
}

double CImageTreeModel::checkedOriginalItemsSize() const
{
    QVectorIterator<CImageTreeItem*> itemsIterator(rootItem->children());
    double totalSize = 0;
    while (itemsIterator.hasNext()) {
        auto item = itemsIterator.next();
        if (!item->isChecked()) {
            continue;
        }
        totalSize += (double)item->getCImage()->getOriginalSize();
    }
    return totalSize;
}
