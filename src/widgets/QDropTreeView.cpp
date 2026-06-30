#include "QDropTreeView.h"

#include <QDragEnterEvent>
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

    // Let the base class toggle the checkbox / handle normal selection first.
    QTreeView::mousePressEvent(event);

    // Clicking the checkbox indicator toggles the check but does NOT change the
    // selection. Make a click in the checkbox column also select the row, just
    // like clicking any other column.
    if (index.isValid() && index.column() == CImageColumns::CHECKBOX_COLUMN && this->selectionModel() != nullptr) {
        QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Rows;
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
            // Preserve multi-select gestures.
            flags |= QItemSelectionModel::Select;
        } else {
            flags |= QItemSelectionModel::ClearAndSelect;
        }
        this->selectionModel()->select(index, flags);
        this->setCurrentIndex(index);
    }
}
