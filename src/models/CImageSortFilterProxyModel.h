#ifndef CIMAGESORTFILTERPROXYMODEL_H
#define CIMAGESORTFILTERPROXYMODEL_H

#include <QRegularExpression>
#include <QSortFilterProxyModel>

class CImageSortFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
private:
    static bool naturalLessThan(QString left, QString right);

    // The file names listed are narrowed to this pattern: fuzzy (any part of
    // the name) or, when enabled, a regular expression.
    QString searchPattern;
    QRegularExpression searchExpression;
    bool searchRegexEnabled = false;
    bool searchCaseSensitive = false;

public:
    explicit CImageSortFilterProxyModel(QObject* parent = nullptr);

    bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override;

    // Narrow the listed rows to file names matching the pattern. Fuzzy mode
    // (the default) matches any part of the name; regex mode matches the
    // pattern as a regular expression. An incomplete regular expression
    // disables the filter until it is valid again, so the list does not go
    // empty while typing. Matching is case insensitive unless caseSensitive
    // is set.
    void setSearchFilter(const QString& pattern, bool regexEnabled, bool caseSensitive = false);
    bool hasSearchFilter() const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
};

#endif // CIMAGESORTFILTERPROXYMODEL_H
