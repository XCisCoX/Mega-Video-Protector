#pragma once

// Dark frosted glass. A bar samples the widget behind it, shrinks that strip
// until it is soft, then paints it back under a charcoal veil.

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace videovault::app {

inline QImage frost(const QImage& source) {
    if (source.isNull()) {
        return {};
    }
    const int width = std::max(16, source.width() / 8);
    const int height = std::max(8, source.height() / 6);
    return source.convertToFormat(QImage::Format_ARGB32_Premultiplied)
        .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// Samples the top or bottom of a scrolling view and blurs it into this bar.
class FrostedBar final : public QWidget {
public:
    enum class Band { Top, Bottom };

    explicit FrostedBar(QWidget* parent = nullptr)
        : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(40);
        connect(timer_, &QTimer::timeout, this, [this] { rebuild(); });
    }

    void follow(QAbstractScrollArea* area, const Band band) {
        area_ = area;
        band_ = band;
        if (scrollConnection_) {
            disconnect(scrollConnection_);
        }
        if (area_ != nullptr) {
            scrollConnection_ = connect(
                area_->verticalScrollBar(), &QAbstractSlider::valueChanged,
                this, [this] { refresh(); });
        }
        refresh();
    }

    void refresh() {
        timer_->start();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        refresh();
    }

    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        refresh();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (!blurred_.isNull()) {
            painter.drawImage(rect(), blurred_);
        } else {
            painter.fillRect(rect(), QColor(12, 12, 16));
        }
        painter.fillRect(rect(), QColor(10, 10, 14, 196));
        painter.setPen(QColor(255, 255, 255, 28));
        if (band_ == Band::Top) {
            painter.drawLine(0, height() - 1, width(), height() - 1);
        } else {
            painter.drawLine(0, 0, width(), 0);
        }
    }

private:
    void rebuild() {
        QWidget* source = area_ != nullptr ? area_->viewport() : nullptr;
        if (source == nullptr || !isVisible() || width() < 4 || height() < 4) {
            blurred_ = QImage();
            update();
            return;
        }
        const QRect bounds = source->rect();
        const int strip = std::min(height(), bounds.height());
        if (strip < 2) {
            return;
        }
        const QRect grab = band_ == Band::Top
            ? QRect(0, 0, bounds.width(), strip)
            : QRect(0, bounds.height() - strip, bounds.width(), strip);
        QImage image(grab.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(0, 0, 0));
        QPainter target(&image);
        source->render(&target, QPoint(0, 0), QRegion(grab));
        target.end();
        blurred_ = frost(image);
        update();
    }

    QPointer<QAbstractScrollArea> area_;
    Band band_{Band::Top};
    QImage blurred_;
    QTimer* timer_{nullptr};
    QMetaObject::Connection scrollConnection_;
};

} // namespace videovault::app
