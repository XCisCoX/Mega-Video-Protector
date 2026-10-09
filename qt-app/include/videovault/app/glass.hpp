#pragma once

// Frosted header, footer, and settings backdrop. A live grab of the gallery
// is blurred (three box passes, a Gaussian) and painted under a dark veil.
// The video and image viewer do not use this.

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QHideEvent>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace videovault::app {

inline int blurIndex(const int index, const int limit) {
    return std::clamp(index, 0, limit - 1);
}

inline void boxBlurHorizontal(const QImage& src, QImage& dst, const int radius) {
    const int width = src.width();
    const int height = src.height();
    const int span = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
        int red = 0;
        int green = 0;
        int blue = 0;
        for (int i = -radius; i <= radius; ++i) {
            const QRgb pixel = line[blurIndex(i, width)];
            red += qRed(pixel);
            green += qGreen(pixel);
            blue += qBlue(pixel);
        }
        for (int x = 0; x < width; ++x) {
            out[x] = qRgb(red / span, green / span, blue / span);
            const QRgb add = line[blurIndex(x + radius + 1, width)];
            const QRgb sub = line[blurIndex(x - radius, width)];
            red += qRed(add) - qRed(sub);
            green += qGreen(add) - qGreen(sub);
            blue += qBlue(add) - qBlue(sub);
        }
    }
}

inline void boxBlurVertical(const QImage& src, QImage& dst, const int radius) {
    const int width = src.width();
    const int height = src.height();
    const int span = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        auto at = [&](const int y) {
            return reinterpret_cast<const QRgb*>(src.constScanLine(blurIndex(y, height)))[x];
        };
        int red = 0;
        int green = 0;
        int blue = 0;
        for (int i = -radius; i <= radius; ++i) {
            const QRgb pixel = at(i);
            red += qRed(pixel);
            green += qGreen(pixel);
            blue += qBlue(pixel);
        }
        for (int y = 0; y < height; ++y) {
            reinterpret_cast<QRgb*>(dst.scanLine(y))[x] =
                qRgb(red / span, green / span, blue / span);
            const QRgb add = at(y + radius + 1);
            const QRgb sub = at(y - radius);
            red += qRed(add) - qRed(sub);
            green += qGreen(add) - qGreen(sub);
            blue += qBlue(add) - qBlue(sub);
        }
    }
}

inline void gaussianBlur(QImage& image) {
    constexpr int radius = 8;
    constexpr int passes = 3;
    QImage scratch(image.size(), QImage::Format_ARGB32);
    for (int pass = 0; pass < passes; ++pass) {
        boxBlurHorizontal(image, scratch, radius);
        boxBlurVertical(scratch, image, radius);
    }
}

inline QImage frost(const QImage& source) {
    if (source.isNull() || source.width() < 2 || source.height() < 2) {
        return {};
    }
    int width = std::max(32, source.width() / 2);
    int height = std::max(16, source.height() / 2);
    constexpr int maxEdge = 640;
    const int longEdge = std::max(width, height);
    if (longEdge > maxEdge) {
        const double scale = static_cast<double>(maxEdge) / static_cast<double>(longEdge);
        width = std::max(32, static_cast<int>(width * scale));
        height = std::max(16, static_cast<int>(height * scale));
    }
    QImage image = source.convertToFormat(QImage::Format_ARGB32)
        .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_ARGB32);
    gaussianBlur(image);
    return image;
}

// Samples the gallery under this bar (or the matching edge) and keeps the
// blur updating while the bar is visible.
class FrostedBar final : public QWidget {
public:
    enum class Band { Top, Bottom };

    explicit FrostedBar(const Band band, QWidget* parent = nullptr)
        : QWidget(parent), band_(band) {
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
        timer_ = new QTimer(this);
        timer_->setInterval(70);
        connect(timer_, &QTimer::timeout, this, [this] { rebuild(); });
    }

    void follow(QAbstractScrollArea* area) {
        if (area_ == area && scrollConnection_) {
            return;
        }
        area_ = area;
        if (scrollConnection_) {
            disconnect(scrollConnection_);
            scrollConnection_ = {};
        }
        if (area_ != nullptr) {
            scrollConnection_ = connect(
                area_->verticalScrollBar(), &QAbstractSlider::valueChanged,
                this, [this] { rebuild(); });
        }
        rebuild();
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        timer_->start();
        rebuild();
    }

    void hideEvent(QHideEvent* event) override {
        timer_->stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (!blurred_.isNull()) {
            painter.drawImage(rect(), blurred_);
        } else {
            painter.fillRect(rect(), QColor(12, 12, 16));
        }
        painter.fillRect(rect(), QColor(8, 8, 12, 140));
        painter.setPen(QColor(255, 255, 255, 32));
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
            return;
        }
        QRect grab;
        if (QWidget* host = parentWidget()) {
            const QPoint origin = source->mapFrom(host, geometry().topLeft());
            grab = QRect(origin, size()).intersected(source->rect());
        }
        if (grab.width() < 2 || grab.height() < 2) {
            const int strip = std::min(std::max(height(), 8), source->height());
            if (strip < 2 || source->width() < 2) {
                return;
            }
            grab = band_ == Band::Top
                ? QRect(0, 0, source->width(), strip)
                : QRect(0, std::max(0, source->height() - strip), source->width(), strip);
        }
        QImage image(grab.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(0, 0, 0));
        {
            QPainter target(&image);
            source->render(&target, QPoint(0, 0), QRegion(grab));
        }
        const QImage next = frost(image);
        if (!next.isNull()) {
            blurred_ = next;
            update();
        }
    }

    Band band_;
    QPointer<QAbstractScrollArea> area_;
    QImage blurred_;
    QTimer* timer_{nullptr};
    QMetaObject::Connection scrollConnection_;
};

} // namespace videovault::app
