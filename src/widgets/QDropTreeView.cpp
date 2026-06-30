#include "QDropTreeView.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>

#include <QFileInfo>
#include <services/Importer.h>
#include <utils/Utils.h>

QDropTreeView::QDropTreeView(QWidget* parent)
    : QTreeView(parent)
{
}

void QDropTreeView::dragEnterEvent(QDragEnterEvent* event)
{
    event->acceptProposedAction();
}

void QDropTreeView::dragMoveEvent(QDragMoveEvent* event)
{
    event->accept();
}

void QDropTreeView::dropEvent(QDropEvent* event)
{
    const QMimeData* mimeData = event->mimeData();
    QList<QUrl> urlList = mimeData->urls();
    QStringList fileList;
    if (mimeData->hasFormat("text/uri-list")) {
        foreach (QUrl url, urlList) {
            QString absolutePath = url.toLocalFile();
            if (QFileInfo(absolutePath).isFile()) {
                fileList << url.toLocalFile();
            } else if (QFileInfo(absolutePath).isDir()) {
                QSettings settings;
                bool scanSubfolders = settings.value("preferences/general/import_subfolders", true).toBool();
                fileList.append(Importer::scanDirectory(absolutePath, scanSubfolders));
            }
        }
    }

    event->acceptProposedAction();
    emit dropFinished(fileList);
}

void QDropTreeView::mousePressEvent(QMouseEvent* event)
{
    QModelIndex index = this->indexAt(event->pos());

    bool isCheckboxColumn = index.isValid()
        && index.column() == CImageColumns::CHECKBOX_COLUMN
        && this->selectionModel() != nullptr
        && this->model() != nullptr;

    if (!isCheckboxColumn) {
        // Any other column behaves exactly like stock QTreeView.
        QTreeView::mousePressEvent(event);
        return;
    }

    // --- Checkbox column ---
    bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
    bool ctrl = (event->modifiers() & Qt::ControlModifier) != 0;
    int clickedRow = index.row();

    // Target check state = the toggled state of the clicked checkbox.
    Qt::CheckState current = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
    Qt::CheckState target = (current == Qt::Checked) ? Qt::Unchecked : Qt::Checked;

    // Let the BASE VIEW handle ALL row selection by forwarding a synthetic press
    // over the NAME column (same row). This keeps Qt's internal selection anchor
    // consistent for plain/Ctrl/Shift clicks, so the painted selection always
    // matches the model (no stale-anchor over-highlighting). The base view does
    // not toggle any checkbox for a NAME-column click, so we do that ourselves.
    QModelIndex nameIndex = this->model()->index(clickedRow, CImageColumns::NAME_COLUMN);
    QRect nameRect = this->visualRect(nameIndex);
    QPoint namePos(nameRect.center().x(), event->position().toPoint().y());
    QMouseEvent forwarded(
        event->type(), QPointF(namePos), event->scenePosition(), event->globalPosition(),
        event->button(), event->buttons(), event->modifiers());
    QTreeView::mousePressEvent(&forwarded);

    // Apply the checkbox state. On Shift+click toggle every currently-selected
    // row (the base view already selected the proper range); otherwise just the
    // clicked row.
    if (shift) {
        const QModelIndexList rows = this->selectionModel()->selectedRows(CImageColumns::CHECKBOX_COLUMN);
        for (const QModelIndex& r : rows) {
            this->model()->setData(r, target, Qt::CheckStateRole);
        }
        this->checkboxDragging = false;
    } else {
        this->model()->setData(index, target, Qt::CheckStateRole);
        // A plain (no-modifier) press may begin a drag (swipe) selection.
        if (!ctrl) {
            this->checkboxDragging = true;
            this->checkboxDragStartRow = clickedRow;
            this->checkboxDragLastRow = clickedRow;
            this->checkboxDragTarget = static_cast<int>(target);
            this->checkboxDragStartPos = event->pos();
            this->checkboxDragActive = false;
        } else {
            this->checkboxDragging = false;
        }
    }

    event->accept();
}

void QDropTreeView::applyCheckboxRange(int fromRow, int toRow, int target)
{
    if (this->model() == nullptr) {
        return;
    }
    int lo = qMin(fromRow, toRow);
    int hi = qMax(fromRow, toRow);
    for (int row = lo; row <= hi; ++row) {
        QModelIndex cell = this->model()->index(row, CImageColumns::CHECKBOX_COLUMN);
        if (cell.isValid()) {
            this->model()->setData(cell, static_cast<Qt::CheckState>(target), Qt::CheckStateRole);
        }
    }
}

void QDropTreeView::mouseMoveEvent(QMouseEvent* event)
{
    bool noModifier = (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) == 0;
    if (this->checkboxDragging && noModifier && (event->buttons() & Qt::LeftButton)
        && this->selectionModel() != nullptr && this->model() != nullptr) {

        // Don't start swipe-selecting until the pointer has clearly moved,
        // beyond a plain-click jitter. A simple click can wobble ~10px and, on a
        // row boundary, cross into the adjacent row; requiring a larger,
        // row-height-based threshold ensures only an intentional drag triggers it.
        if (!this->checkboxDragActive) {
            int rowHeight = this->sizeHintForRow(this->checkboxDragStartRow);
            if (rowHeight <= 0) {
                rowHeight = 24;
            }
            int threshold = qMax(QApplication::startDragDistance() * 2, rowHeight + rowHeight / 2);
            int dist = (event->pos() - this->checkboxDragStartPos).manhattanLength();
            if (dist < threshold) {
                event->accept();
                return;
            }
            this->checkboxDragActive = true;
        }

        QModelIndex index = this->indexAt(event->pos());
        if (index.isValid()) {
            int currentRow = index.row();

            // Forward the move to the base view over the NAME column so the base
            // class performs the rubber-band/drag row selection with its own
            // (consistent) anchor. This keeps selection unified with the other
            // columns and with a subsequent Shift+click.
            QModelIndex nameIndex = this->model()->index(currentRow, CImageColumns::NAME_COLUMN);
            QRect nameRect = this->visualRect(nameIndex);
            QPoint namePos(nameRect.center().x(), event->position().toPoint().y());
            QMouseEvent forwarded(
                event->type(), QPointF(namePos), event->scenePosition(), event->globalPosition(),
                event->button(), event->buttons(), event->modifiers());
            QTreeView::mouseMoveEvent(&forwarded);

            // Apply the drag's target check state to the swept range.
            if (currentRow != this->checkboxDragLastRow) {
                this->applyCheckboxRange(this->checkboxDragStartRow, currentRow, this->checkboxDragTarget);
                this->checkboxDragLastRow = currentRow;
            }
        }
        event->accept();
        return;
    }

    QTreeView::mouseMoveEvent(event);
}

void QDropTreeView::mouseReleaseEvent(QMouseEvent* event)
{
    if (this->checkboxDragging) {
        bool wasActiveDrag = this->checkboxDragActive;
        this->checkboxDragging = false;
        this->checkboxDragActive = false;
        this->checkboxDragStartRow = -1;
        this->checkboxDragLastRow = -1;
        this->checkboxDragTarget = -1;

        // If an actual drag happened we forwarded moves to the base view, so let
        // the base finalize its drag/selection state with a matching release.
        if (wasActiveDrag) {
            QModelIndex index = this->indexAt(event->pos());
            int row = index.isValid() ? index.row() : 0;
            QModelIndex nameIndex = this->model() ? this->model()->index(row, CImageColumns::NAME_COLUMN) : QModelIndex();
            QRect nameRect = nameIndex.isValid() ? this->visualRect(nameIndex) : QRect();
            QPoint namePos(nameRect.isValid() ? nameRect.center().x() : event->position().toPoint().x(),
                event->position().toPoint().y());
            QMouseEvent forwarded(
                event->type(), QPointF(namePos), event->scenePosition(), event->globalPosition(),
                event->button(), event->buttons(), event->modifiers());
            QTreeView::mouseReleaseEvent(&forwarded);
        }
        event->accept();
        return;
    }

    QTreeView::mouseReleaseEvent(event);
}
