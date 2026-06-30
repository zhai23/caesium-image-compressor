#include "QZoomGraphicsView.h"

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QLabel>
#include <QMenu>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>

QZoomGraphicsView::QZoomGraphicsView(QWidget* parent)
    : QGraphicsView(parent)
{
    this->graphicsScene = new QGraphicsScene();
    this->setScene(this->graphicsScene);
    // Ensure the whole viewport background is repainted (needed so the fixed
    // white/black/checkerboard background covers the full area when scrolling).
    this->setCacheMode(QGraphicsView::CacheNone);
    this->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);

    this->loaderLabel = new QLabel();
    this->loaderLabel->setAutoFillBackground(false);
    this->loaderLabel->setAttribute(Qt::WA_NoSystemBackground);
    this->loaderMovie = new QMovie(":/icons/ui/loader.webp");
    this->loaderMovie->setScaledSize(QSize(40, 40));
    this->loaderLabel->setMovie(this->loaderMovie);
    this->loaderMovie->start();
    this->loaderProxyWidget = this->graphicsScene->addWidget(this->loaderLabel);
    this->loaderProxyWidget->hide();

    this->pixmapItem = new QGraphicsPixmapItem();
}

void QZoomGraphicsView::wheelEvent(QWheelEvent* event)
{
    if (!this->zoomEnabled) {
        return;
    }
    this->zooming = true;
    this->setScaleFactor(event);
    emit scaleFactorChanged(event);
    this->zooming = false;
    emit this->horizontalScrollBar()->valueChanged(this->horizontalScrollBar()->value());
    emit this->verticalScrollBar()->valueChanged(this->verticalScrollBar()->value());
}

void QZoomGraphicsView::setScaleFactor(QWheelEvent* event)
{
    const ViewportAnchor anchor = transformationAnchor();
    setTransformationAnchor(QGraphicsView::AnchorViewCenter);
    int angle = event->angleDelta().y();

    qreal factor = 1;
    if ((float)angle > WHEEL_TOLERANCE) {
        factor = ZOOM_IN_RATIO;
    } else if ((float)angle < -WHEEL_TOLERANCE) {
        factor = ZOOM_OUT_RATIO;
    }

    double expectedScaleFactor = this->scaleFactor * factor;
    if (expectedScaleFactor > MAX_ZOOM_IN || expectedScaleFactor < MAX_ZOOM_OUT) {
        return;
    }

    setTransformationAnchor(anchor);
    scale(factor, factor);

    this->scaleFactor *= factor;
}

void QZoomGraphicsView::resetScaleFactor()
{
    this->scaleFactor = 1;
}

void QZoomGraphicsView::setHorizontalScrollBarValue(int value)
{
    if (!this->zooming) {
        this->horizontalScrollBar()->setValue(value);
    }
}

void QZoomGraphicsView::setVerticalScrollBarValue(int value)
{
    if (!this->zooming) {
        this->verticalScrollBar()->setValue(value);
    }
}

void QZoomGraphicsView::setLoading(bool l)
{
    this->loading = l;

    if (l) {
        this->graphicsScene->setSceneRect(0, 0, 40, 40);
        this->fitInView(this->rect(), Qt::KeepAspectRatio);
        this->loaderProxyWidget->show();
    } else {
        this->loaderProxyWidget->hide();
    }
}

void QZoomGraphicsView::setZoomEnabled(bool l)
{
    this->zoomEnabled = l;
}

void QZoomGraphicsView::showPixmap(const QPixmap& pixmap)
{
    this->pixmapItem = this->graphicsScene->addPixmap(pixmap);
    this->graphicsScene->setSceneRect(this->graphicsScene->itemsBoundingRect());
}

void QZoomGraphicsView::removePixmap()
{
    qsizetype indexOf = this->graphicsScene->items().indexOf(this->pixmapItem);
    if (indexOf != -1) {
        auto* item = this->graphicsScene->items().at(indexOf);
        delete item;
    }
}

void QZoomGraphicsView::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    auto* group = new QActionGroup(&menu);
    group->setExclusive(true);

    struct Entry {
        int mode;
        QString label;
    };
    const QList<Entry> entries = {
        { THEME, tr("Theme") },
        { WHITE, tr("White") },
        { BLACK, tr("Black") },
        { CHECKERBOARD, tr("Checkerboard") }
    };

    for (const Entry& e : entries) {
        QAction* action = menu.addAction(e.label);
        action->setCheckable(true);
        action->setChecked(this->backgroundMode == e.mode);
        group->addAction(action);
        int mode = e.mode;
        connect(action, &QAction::triggered, this, [this, mode]() {
            emit this->previewBackgroundChangeRequested(mode);
        });
    }

    menu.exec(event->globalPos());
}

void QZoomGraphicsView::setPreviewBackground(int mode)
{
    this->backgroundMode = mode;
    this->resetCachedContent();
    this->viewport()->update();
}

void QZoomGraphicsView::drawBackground(QPainter* painter, const QRectF& rect)
{
    if (this->backgroundMode == THEME) {
        // Keep the default theme/palette-driven background.
        QGraphicsView::drawBackground(painter, rect);
        return;
    }

    // Paint a fixed background that fills the whole viewport, independent of the
    // scene's scroll/zoom transform.
    painter->save();
    painter->resetTransform();
    QRect viewportRect = this->viewport()->rect();

    if (this->backgroundMode == WHITE) {
        painter->fillRect(viewportRect, Qt::white);
    } else if (this->backgroundMode == BLACK) {
        painter->fillRect(viewportRect, Qt::black);
    } else if (this->backgroundMode == CHECKERBOARD) {
        const int tile = 10;
        const QColor light(204, 204, 204);
        const QColor dark(255, 255, 255);
        painter->fillRect(viewportRect, dark);
        for (int y = 0; y < viewportRect.height(); y += tile) {
            for (int x = 0; x < viewportRect.width(); x += tile) {
                bool shaded = ((x / tile) + (y / tile)) % 2 == 0;
                if (shaded) {
                    painter->fillRect(QRect(x, y, tile, tile), light);
                }
            }
        }
    }

    painter->restore();
}
