#include "CImageSortFilterProxyModel.h"
#include "models/CImageTreeItem.h"
#include "models/CImageTreeModel.h"
#include "utils/Utils.h"
#include <QCollator>

CImageSortFilterProxyModel::CImageSortFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel { parent }
{
}

void CImageSortFilterProxyModel::setSearchFilter(const QString& pattern, bool regexEnabled, bool caseSensitive)
{
    this->searchPattern = pattern;
    this->searchRegexEnabled = regexEnabled;
    this->searchCaseSensitive = caseSensitive;
    this->searchExpression = QRegularExpression();
    if (regexEnabled && !pattern.isEmpty()) {
        this->searchExpression = QRegularExpression(pattern,
            caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    }
    this->invalidateFilter();
}

bool CImageSortFilterProxyModel::hasSearchFilter() const
{
    return !this->searchPattern.isEmpty()
        && (!this->searchRegexEnabled || this->searchExpression.isValid());
}

bool CImageSortFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (!this->hasSearchFilter()) {
        return true;
    }

    // The search runs on the text the name column shows: the path of the image
    // relative to the imported root folder with its original file name (e.g.
    // "sub/texture.tga"), so both the folder part and the name can be found.
    const QModelIndex nameIndex = sourceModel()->index(sourceRow, CImageColumns::NAME_COLUMN, sourceParent);
    if (!nameIndex.isValid()) {
        return true;
    }
    auto* item = static_cast<CImageTreeItem*>(nameIndex.internalPointer());
    const CImage* image = item != nullptr ? item->getCImage() : nullptr;
    if (image == nullptr) {
        return true;
    }
    auto* model = static_cast<const CImageTreeModel*>(this->sourceModel());
    const QString name = model->displayedRelativePath(item->getCImage());

    if (this->searchRegexEnabled) {
        return this->searchExpression.match(name).hasMatch();
    }
    return name.contains(this->searchPattern, this->searchCaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
}

bool CImageSortFilterProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    QVariant leftData = sourceModel()->data(left);
    QVariant rightData = sourceModel()->data(right);

    CImage* leftCImage = static_cast<CImageTreeItem*>(left.internalPointer())->getCImage();
    CImage* rightCImage = static_cast<CImageTreeItem*>(right.internalPointer())->getCImage();

    if (left.column() == CImageColumns::NAME_COLUMN && right.column() == CImageColumns::NAME_COLUMN) {
        // TODO Needs a regex for the HTML field?
        return naturalLessThan(leftData.toString(), rightData.toString());
    } else if (left.column() == CImageColumns::SIZE_COLUMN && right.column() == CImageColumns::SIZE_COLUMN) {
        return leftCImage->getOriginalSize() < rightCImage->getOriginalSize();
    } else if (left.column() == CImageColumns::RESOLUTION_COLUMN && right.column() == CImageColumns::RESOLUTION_COLUMN) {
        return leftCImage->getTotalPixels() < rightCImage->getTotalPixels();
    } else if (left.column() == CImageColumns::RATIO_COLUMN && right.column() == CImageColumns::RATIO_COLUMN) {
        if (leftCImage->getCompressedSize() == 0 && rightCImage->getCompressedSize() == 0) {
            return naturalLessThan(leftData.toString(), rightData.toString());
        }

        return leftCImage->getRatio() < rightCImage->getRatio();
    }
    return naturalLessThan(leftData.toString(), rightData.toString());
}

bool CImageSortFilterProxyModel::naturalLessThan(QString left, QString right)
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseSensitive);
    collator.setNumericMode(true);
    return collator.compare(left, right) < 0;
}
