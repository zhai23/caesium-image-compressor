#include "QCheckBoxHeaderView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionHeader>

QCheckBoxHeaderView::QCheckBoxHeaderView(int checkboxSection, Qt::Orientation orientation, QWidget* parent)
    : QHeaderView(orientation, parent)
    , m_checkboxSection(checkboxSection)
{
    this->setSectionsClickable(true);
}

Qt::CheckState QCheckBoxHeaderView::checkState() const
{
    return m_checkState;
}

void QCheckBoxHeaderView::setCheckState(Qt::CheckState state)
{
    if (m_checkState == state) {
        return;
    }
    m_checkState = state;
    this->updateSection(m_checkboxSection);
}

QRect QCheckBoxHeaderView::checkBoxRect(const QRect& sectionRect) const
{
    QStyleOptionButton option;
    QRect indicator = this->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
    int x = sectionRect.x() + (sectionRect.width() - indicator.width()) / 2;
    int y = sectionRect.y() + (sectionRect.height() - indicator.height()) / 2;
    return QRect(QPoint(x, y), indicator.size());
}

void QCheckBoxHeaderView::paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const
{
    if (logicalIndex != m_checkboxSection) {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        return;
    }

    // Draw a clean header background for the checkbox section: no label text and
    // no sort indicator, so nothing peeks out from behind the checkbox.
    painter->save();
    QStyleOptionHeader headerOption;
    this->initStyleOption(&headerOption);
    headerOption.section = logicalIndex;
    headerOption.rect = rect;
    headerOption.text = QString();
    headerOption.icon = QIcon();
    headerOption.sortIndicator = QStyleOptionHeader::None;
    headerOption.position = QStyleOptionHeader::Beginning;
    headerOption.state = QStyle::State_Enabled | QStyle::State_Active | QStyle::State_Horizontal;
    this->style()->drawControl(QStyle::CE_HeaderSection, &headerOption, painter, this);
    painter->restore();

    QStyleOptionButton option;
    option.rect = this->checkBoxRect(rect);
    option.state = QStyle::State_Enabled;
    if (m_checkState == Qt::Checked) {
        option.state |= QStyle::State_On;
    } else if (m_checkState == Qt::PartiallyChecked) {
        option.state |= QStyle::State_NoChange;
    } else {
        option.state |= QStyle::State_Off;
    }

    this->style()->drawControl(QStyle::CE_CheckBox, &option, painter, this);
}

void QCheckBoxHeaderView::mousePressEvent(QMouseEvent* event)
{
    int logicalIndex = this->logicalIndexAt(event->pos());
    if (logicalIndex == m_checkboxSection) {
        // Toggle: anything that is not fully checked becomes checked, otherwise unchecked.
        Qt::CheckState newState = (m_checkState == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        m_checkState = newState;
        this->updateSection(m_checkboxSection);
        emit checkStateToggled(newState);
        return;
    }

    QHeaderView::mousePressEvent(event);
}
