#include "notebookbutton.h"
#include "QPainter"
#include "QPainterPath"
#include "QMouseEvent"
#include "colorscheme.h"

NotebookButton::NotebookButton(QWidget *parent)
    : QWidget(parent)
{

}

void NotebookButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    QColor background;
    QColor foreground;

    auto colorScheme = ColorScheme::getInstance();

    if (mouseDown)
    {
        background = colorScheme.TabSelectedBackground;
        foreground = colorScheme.TabSelectedText;
    }
    else if (mouseOver)
    {
        background = colorScheme.TabHoverBackground;
        foreground = colorScheme.TabSelectedBackground;
    }
    else
    {
        background = colorScheme.TabPanelBackground;
        foreground = colorScheme.TabSelectedBackground;
    }

    painter.setPen(Qt::NoPen);
    painter.fillRect(this->rect(), background);

    float h = this->height(), w = this->width();

    if (icon == IconPlus)
    {
        painter.fillRect(QRectF((h / 12) * 2 + 1, (h / 12) * 5 + 1, w - ((h / 12) * 5), (h / 12) * 1), foreground);
        painter.fillRect(QRectF((h / 12) * 5 + 1, (h / 12) * 2 + 1, (h / 12) * 1, w - ((h / 12) * 5)), foreground);
    }
    else if (icon == IconUser)
    {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::HighQualityAntialiasing);

        auto a = w/8;
        QPainterPath path;

        path.arcMoveTo(a, 4 * a, 6 * a, 6 * a, 0);
        path.arcTo(a, 4 * a, 6 * a, 6 * a, 0, 180);

        painter.fillPath(path, foreground);

        painter.setBrush(background);
        painter.drawEllipse(2*a, 1*a, 4*a, 4*a);

        painter.setBrush(foreground);
        painter.drawEllipse(2.5*a, 1.5*a, 3*a + 1, 3*a);
    }
    else // IconSettings
    {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::HighQualityAntialiasing);

        auto a = w/8;
        QPainterPath path;

        path.arcMoveTo(a, a, 6*a, 6*a, 0 - (360 / 32.0));

        for (int i = 0; i < 8; i++)
        {
            path.arcTo(a, a, 6*a, 6*a, i * (360 / 8.0) - (360 / 32.0), (360 / 32.0));
            path.arcTo(2*a, 2*a, 4*a, 4*a, i * (360 / 8.0) + (360 / 32.0), (360 / 32.0));
        }

        painter.fillPath(path, foreground);

        painter.setBrush(background);
        painter.drawEllipse(3*a, 3*a, 2*a, 2*a);
    }
}

void NotebookButton::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        mouseDown = true;

        this->repaint();
    }
}

void NotebookButton::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        mouseDown = false;

        this->repaint();

        emit clicked();
    }
}

void NotebookButton::enterEvent(QEvent *)
{
    mouseOver = true;

    this->repaint();
}

void NotebookButton::leaveEvent(QEvent *)
{
    mouseOver = false;

    this->repaint();
}