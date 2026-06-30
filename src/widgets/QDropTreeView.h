#include <climits>
#ifndef QDROPTREEWIDGET_H
#define QDROPTREEWIDGET_H

#include <QPoint>
#include <QTreeView>

class QMimeData;

class QDropTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit QDropTreeView(QWidget* parent = nullptr);

signals:
    void dropFinished(QStringList);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void applyCheckboxRange(int fromRow, int toRow, int target);

    // Drag (swipe) selection state for the checkbox column.
    bool checkboxDragging = false;
    bool checkboxDragActive = false; // true once the pointer passed the drag threshold
    QPoint checkboxDragStartPos;
    int checkboxDragStartRow = -1;
    int checkboxDragLastRow = -1;
    int checkboxDragTarget = -1; // Qt::CheckState as int
};

#endif // QDROPTREEWIDGET_H
