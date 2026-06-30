#ifndef QCHECKBOXHEADERVIEW_H
#define QCHECKBOXHEADERVIEW_H

#include <QHeaderView>

class QCheckBoxHeaderView : public QHeaderView {
    Q_OBJECT

public:
    explicit QCheckBoxHeaderView(int checkboxSection, Qt::Orientation orientation = Qt::Horizontal, QWidget* parent = nullptr);

    Qt::CheckState checkState() const;
    void setCheckState(Qt::CheckState state);

signals:
    void checkStateToggled(Qt::CheckState state);

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QRect checkBoxRect(const QRect& sectionRect) const;

    int m_checkboxSection;
    Qt::CheckState m_checkState = Qt::Checked;
};

#endif // QCHECKBOXHEADERVIEW_H
