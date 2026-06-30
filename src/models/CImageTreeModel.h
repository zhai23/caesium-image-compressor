#ifndef CIMAGETREEMODEL_H
#define CIMAGETREEMODEL_H

#include "CImage.h"
#include "CImageTreeItem.h"

#include <QAbstractItemModel>
#include <QDir>

class CImageTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit CImageTreeModel();
    ~CImageTreeModel();

    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    bool removeRows(int row, int count, const QModelIndex& parent = QModelIndex()) override;

    void appendItems(QList<CImage*> imageList, QString folder = "");

    CImageTreeItem* getRootItem() const;
    bool contains(CImage* cImage);

    void setAllChecked(bool checked);
    int checkedItemsCount() const;

    double originalItemsSize() const;
    double compressedItemsSize() const;
    double checkedOriginalItemsSize() const;
    double checkedCompressedItemsSize() const;

private:
    void setupModelData(const QList<CImage*> imageList, CImageTreeItem* parent);

    CImageTreeItem* rootItem;
    QString baseFolder;

signals:
    void itemsChanged();
    void checkedItemsChanged();

public slots:
    void emitDataChanged(int row);
};

#endif // CIMAGETREEMODEL_H
