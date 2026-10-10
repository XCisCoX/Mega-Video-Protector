#include "videovault/app/music_window.hpp"

#include "videovault/app/media_decoder.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <random>
#include <utility>

namespace videovault::app {

namespace {

constexpr int kBarHeight = 36;
constexpr int kControlHeight = 30;
const QColor kAccent(0x33, 0x90, 0xEC);
const QColor kIcon(0x8D, 0x98, 0xA3);
const QColor kIconDisabled(0x4A, 0x4A, 0x4C);
const QColor kCard(28, 28, 30);

QString speedLabel(const int milli) {
    const double shown = std::round(milli / 100.0) / 10.0;
    return QString::number(shown) + QLatin1Char('X');
}

void paintSpeaker(QPainter& painter, const QRect& box, const int waves) {
    const QRectF r(box);
    QPainterPath body;
    body.moveTo(r.left() + 4, r.center().y() - 3);
    body.lineTo(r.left() + 8, r.center().y() - 3);
    body.lineTo(r.left() + 12, r.center().y() - 6);
    body.lineTo(r.left() + 12, r.center().y() + 6);
    body.lineTo(r.left() + 8, r.center().y() + 3);
    body.lineTo(r.left() + 4, r.center().y() + 3);
    body.closeSubpath();
    painter.drawPath(body);
    painter.setBrush(Qt::NoBrush);
    QPen pen = painter.pen();
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    if (waves >= 1) {
        painter.drawArc(QRectF(r.center().x() - 2, r.center().y() - 4, 8, 8), -50 * 16, 100 * 16);
    }
    if (waves >= 2) {
        painter.drawArc(QRectF(r.center().x() - 1, r.center().y() - 7, 12, 14), -50 * 16, 100 * 16);
    }
    if (waves <= 0) {
        painter.drawLine(QPointF(r.right() - 6, r.center().y() - 4), QPointF(r.right() - 2, r.center().y() + 4));
        painter.drawLine(QPointF(r.right() - 6, r.center().y() + 4), QPointF(r.right() - 2, r.center().y() - 4));
    }
}

void paintArrowHead(QPainter& painter, const QPointF& tip, const QPointF& direction) {
    const double len = std::hypot(direction.x(), direction.y());
    if (len < 0.1) {
        return;
    }
    const QPointF u = direction / len;
    const QPointF n(-u.y(), u.x());
    QPolygonF head;
    head << tip << (tip - u * 5.0 + n * 3.0) << (tip - u * 5.0 - n * 3.0);
    const QColor ink = painter.pen().color();
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPolygon(head);
    painter.restore();
}

} // namespace

// Overlap-add stretch so 0.5x–2.5x keeps the original pitch. Playback at
// 1x bypasses this and writes the decoded samples unchanged.
class TimeStretch {
public:
    void reset() {
        pending_.clear();
        overlap_.assign(overlap_.size(), 0.f);
        ready_ = false;
    }

    std::vector<std::int16_t> process(
        const std::int16_t* samples,
        const std::size_t count,
        const int channels,
        const double speed,
        std::size_t* consumedFrames) {
        if (channels <= 0) {
            return {};
        }
        if (channels_ != channels) {
            channels_ = channels;
            ready_ = false;
            pending_.clear();
            overlap_.clear();
        }
        const bool bypass = std::abs(speed - 1.0) < 0.03;
        if (bypass) {
            pending_.clear();
            overlap_.clear();
            if (count == 0 || samples == nullptr) {
                return {};
            }
            if (consumedFrames != nullptr) {
                *consumedFrames += count / static_cast<std::size_t>(channels);
            }
            return std::vector<std::int16_t>(samples, samples + count);
        }
        ensureWindow();
        if (samples != nullptr && count > 0) {
            pending_.insert(pending_.end(), samples, samples + count);
        }
        std::vector<std::int16_t> out;
        const int width = kWindow;
        const int hop = kHop;
        const int analysis = std::max(1, static_cast<int>(std::lround(hop * speed)));
        const int frame = channels_;
        while (static_cast<int>(pending_.size()) >= width * frame) {
            for (int i = 0; i < width; ++i) {
                const float weight = window_[static_cast<std::size_t>(i)];
                for (int channel = 0; channel < frame; ++channel) {
                    overlap_[static_cast<std::size_t>(i * frame + channel)] +=
                        weight * static_cast<float>(pending_[static_cast<std::size_t>(i * frame + channel)]);
                }
            }
            out.reserve(out.size() + static_cast<std::size_t>(hop * frame));
            for (int i = 0; i < hop; ++i) {
                const float norm = std::max(0.001f, norm_[static_cast<std::size_t>(i)]);
                for (int channel = 0; channel < frame; ++channel) {
                    const int sample = static_cast<int>(std::lround(
                        overlap_[static_cast<std::size_t>(i * frame + channel)] / norm));
                    out.push_back(static_cast<std::int16_t>(std::clamp(sample, -32768, 32767)));
                }
            }
            const int tail = (width - hop) * frame;
            for (int i = 0; i < tail; ++i) {
                overlap_[static_cast<std::size_t>(i)] =
                    overlap_[static_cast<std::size_t>(i + hop * frame)];
            }
            std::fill(overlap_.begin() + tail, overlap_.end(), 0.f);
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(analysis * frame));
            if (consumedFrames != nullptr) {
                *consumedFrames += static_cast<std::size_t>(analysis);
            }
        }
        return out;
    }

    std::vector<std::int16_t> flush(
        const int channels,
        const double speed,
        std::size_t* consumedFrames) {
        if (channels <= 0 || pending_.empty()) {
            overlap_.clear();
            return {};
        }
        if (std::abs(speed - 1.0) < 0.03) {
            auto out = pending_;
            if (consumedFrames != nullptr) {
                *consumedFrames += pending_.size() / static_cast<std::size_t>(channels);
            }
            pending_.clear();
            return out;
        }
        ensureWindow();
        const int need = kWindow * channels_ - static_cast<int>(pending_.size());
        if (need > 0) {
            pending_.insert(pending_.end(), static_cast<std::size_t>(need), 0);
        }
        auto out = process(nullptr, 0, channels, speed, consumedFrames);
        if (!overlap_.empty() && channels_ > 0) {
            const int frames = static_cast<int>(overlap_.size()) / channels_;
            for (int i = 0; i < frames; ++i) {
                const float norm = std::max(0.001f, norm_[static_cast<std::size_t>(i % kHop)]);
                for (int channel = 0; channel < channels_; ++channel) {
                    const int sample = static_cast<int>(std::lround(
                        overlap_[static_cast<std::size_t>(i * channels_ + channel)] / norm));
                    out.push_back(static_cast<std::int16_t>(std::clamp(sample, -32768, 32767)));
                }
            }
        }
        pending_.clear();
        overlap_.clear();
        return out;
    }

private:
    static constexpr int kWindow = 2048;
    static constexpr int kHop = 512;

    void ensureWindow() {
        if (ready_ || channels_ <= 0) {
            return;
        }
        window_.resize(static_cast<std::size_t>(kWindow));
        for (int i = 0; i < kWindow; ++i) {
            window_[static_cast<std::size_t>(i)] =
                0.5f * (1.f - std::cos(2.f * 3.14159265f * static_cast<float>(i) / (kWindow - 1)));
        }
        norm_.assign(static_cast<std::size_t>(kHop), 0.f);
        for (int i = 0; i < kHop; ++i) {
            float sum = 0.f;
            for (int k = i; k < kWindow; k += kHop) {
                sum += window_[static_cast<std::size_t>(k)];
            }
            norm_[static_cast<std::size_t>(i)] = sum;
        }
        overlap_.assign(static_cast<std::size_t>(kWindow * channels_), 0.f);
        ready_ = true;
    }

    int channels_{0};
    bool ready_{false};
    std::vector<float> window_;
    std::vector<float> norm_;
    std::vector<float> overlap_;
    std::vector<std::int16_t> pending_;
};

enum class Glyph {
    Play,
    Pause,
    Previous,
    Next,
    VolumeOff,
    VolumeLow,
    VolumeFull,
    Repeat,
    RepeatOne,
    Reverse,
    Shuffle,
    Close,
};

class GlyphButton final : public QAbstractButton {
public:
    explicit GlyphButton(const Glyph glyph, QWidget* parent)
        : QAbstractButton(parent), glyph_(glyph) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground, true);
    }

    void setGlyph(const Glyph glyph) {
        glyph_ = glyph;
        update();
    }
    void setActive(const bool active) {
        active_ = active;
        update();
    }
    void setLabel(const QString& label) {
        label_ = label;
        update();
    }

    std::function<void()> entered;
    std::function<void()> left;

protected:
    void enterEvent(QEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
        if (entered) {
            entered();
        }
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
        if (left) {
            left();
        }
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (isEnabled() && underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 18));
            painter.drawEllipse(rect().center(), 13, 13);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink());
        const QRect box(rect().center().x() - 12, rect().center().y() - 12, 24, 24);
        if (!label_.isEmpty()) {
            painter.setPen(ink());
            QFont font(QStringLiteral("Segoe UI"));
            font.setPixelSize(11);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(rect(), Qt::AlignCenter, label_);
            return;
        }
        paintGlyph(painter, box);
    }

private:
    QColor ink() const {
        if (!isEnabled()) {
            return kIconDisabled;
        }
        if (active_) {
            return kAccent;
        }
        if (underMouse()) {
            return Qt::white;
        }
        return kIcon;
    }

    void paintGlyph(QPainter& painter, const QRect& box) const {
        const QRectF r(box);
        switch (glyph_) {
        case Glyph::Play: {
            QPolygonF triangle;
            triangle << QPointF(r.left() + 8, r.top() + 5)
                     << QPointF(r.left() + 8, r.bottom() - 5)
                     << QPointF(r.right() - 5, r.center().y());
            painter.drawPolygon(triangle);
            break;
        }
        case Glyph::Pause:
            painter.drawRoundedRect(QRectF(r.left() + 6, r.top() + 5, 4, 14), 1.2, 1.2);
            painter.drawRoundedRect(QRectF(r.left() + 14, r.top() + 5, 4, 14), 1.2, 1.2);
            break;
        case Glyph::Previous:
            painter.drawRoundedRect(QRectF(r.left() + 5, r.top() + 5, 2.4, 14), 0.8, 0.8);
            {
                QPolygonF triangle;
                triangle << QPointF(r.right() - 5, r.top() + 5)
                         << QPointF(r.right() - 5, r.bottom() - 5)
                         << QPointF(r.left() + 9, r.center().y());
                painter.drawPolygon(triangle);
            }
            break;
        case Glyph::Next:
            painter.drawRoundedRect(QRectF(r.right() - 7.4, r.top() + 5, 2.4, 14), 0.8, 0.8);
            {
                QPolygonF triangle;
                triangle << QPointF(r.left() + 5, r.top() + 5)
                         << QPointF(r.left() + 5, r.bottom() - 5)
                         << QPointF(r.right() - 9, r.center().y());
                painter.drawPolygon(triangle);
            }
            break;
        case Glyph::VolumeOff:
        case Glyph::VolumeLow:
        case Glyph::VolumeFull: {
            painter.setPen(QPen(ink(), 1.4));
            painter.setBrush(ink());
            const int waves = glyph_ == Glyph::VolumeFull ? 2 : glyph_ == Glyph::VolumeLow ? 1 : 0;
            paintSpeaker(painter, box, waves);
            break;
        }
        case Glyph::Repeat:
        case Glyph::RepeatOne:
            paintRepeat(painter, box, glyph_ == Glyph::RepeatOne);
            break;
        case Glyph::Reverse:
            paintOpposed(painter, box, false);
            break;
        case Glyph::Shuffle:
            paintOpposed(painter, box, true);
            break;
        case Glyph::Close: {
            painter.setPen(QPen(ink(), 1.7, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(r.center().x() - 5, r.center().y() - 5),
                QPointF(r.center().x() + 5, r.center().y() + 5));
            painter.drawLine(QPointF(r.center().x() + 5, r.center().y() - 5),
                QPointF(r.center().x() - 5, r.center().y() + 5));
            break;
        }
        }
    }

    void paintRepeat(QPainter& painter, const QRect& box, const bool one) const {
        painter.save();
        QPen pen(ink(), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF arc = QRectF(box).adjusted(4.2, 5.2, -4.2, -5.2);
        painter.drawArc(arc, 40 * 16, 290 * 16);
        const double angle = 40.0 * 3.14159265 / 180.0;
        const QPointF center = arc.center();
        const QPointF tip(center.x() + arc.width() * 0.5 * std::cos(angle),
            center.y() - arc.height() * 0.5 * std::sin(angle));
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink());
        QPolygonF head;
        head << tip + QPointF(1.5, -4.0) << tip + QPointF(5.0, 1.5) << tip + QPointF(-2.0, 1.0);
        painter.drawPolygon(head);
        if (one) {
            painter.setPen(ink());
            QFont font(QStringLiteral("Segoe UI"));
            font.setPixelSize(9);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(box.adjusted(0, 1, 0, 0), Qt::AlignCenter, QStringLiteral("1"));
        }
        painter.restore();
    }

    void paintOpposed(QPainter& painter, const QRect& box, const bool crossed) const {
        painter.save();
        QPen pen(ink(), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        const QRectF r(box.adjusted(3, 4, -3, -4));
        if (!crossed) {
            painter.drawLine(QPointF(r.right(), r.top() + 2), QPointF(r.left() + 4, r.top() + 2));
            paintArrowHead(painter, QPointF(r.left() + 2, r.top() + 2), QPointF(-1, 0));
            painter.setPen(pen);
            painter.drawLine(QPointF(r.left(), r.bottom() - 2), QPointF(r.right() - 4, r.bottom() - 2));
            paintArrowHead(painter, QPointF(r.right() - 2, r.bottom() - 2), QPointF(1, 0));
        } else {
            painter.drawLine(QPointF(r.left(), r.bottom() - 1), QPointF(r.right() - 4, r.top() + 2));
            paintArrowHead(painter, QPointF(r.right() - 2, r.top() + 1), QPointF(1, -0.6));
            painter.setPen(pen);
            painter.drawLine(QPointF(r.left(), r.top() + 1), QPointF(r.right() - 4, r.bottom() - 2));
            paintArrowHead(painter, QPointF(r.right() - 2, r.bottom() - 1), QPointF(1, 0.6));
        }
        painter.restore();
    }

    Glyph glyph_;
    bool active_{false};
    QString label_;
};

class ProgressLine final : public QWidget {
public:
    explicit ProgressLine(QWidget* parent) : QWidget(parent) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
        setMouseTracking(true);
    }

    int value() const { return value_; }
    bool dragging() const { return dragging_; }

    void setValue(const int value) {
        const int next = std::clamp(value, 0, 1000);
        if (next == value_) {
            return;
        }
        value_ = next;
        update();
    }

    std::function<void(int)> moved;
    std::function<void(int)> released;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(Qt::NoPen);
        const int thick = (underMouse() || dragging_) ? 4 : 2;
        const int y = height() - thick;
        const int played = isEnabled() ? width() * value_ / 1000 : 0;
        painter.setBrush(kAccent);
        painter.drawRect(0, y, played, thick);
        painter.setBrush(QColor(0x3A, 0x3A, 0x3C));
        painter.drawRect(played, y, width() - played, thick);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (!isEnabled() || event->button() != Qt::LeftButton) {
            return;
        }
        dragging_ = true;
        apply(event->pos().x());
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_) {
            apply(event->pos().x());
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!dragging_) {
            return;
        }
        apply(event->pos().x());
        dragging_ = false;
        if (released) {
            released(value_);
        }
        update();
    }
    void leaveEvent(QEvent* event) override {
        QWidget::leaveEvent(event);
        update();
    }
    void enterEvent(QEvent* event) override {
        QWidget::enterEvent(event);
        update();
    }

private:
    void apply(const int x) {
        const int next = width() <= 0 ? 0 : std::clamp(x, 0, width()) * 1000 / width();
        setValue(next);
        if (moved) {
            moved(value_);
        }
    }

    int value_{0};
    bool dragging_{false};
};

class PlayerPopup : public QWidget {
public:
    explicit PlayerPopup(QWidget* owner) : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255, 36)));
        painter.setBrush(kCard);
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 12, 12);
    }
};

class MenuRow final : public QAbstractButton {
public:
    explicit MenuRow(const QString& label, QWidget* parent)
        : QAbstractButton(parent), label_(label) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFixedHeight(36);
    }

    void setMarked(const bool marked) {
        marked_ = marked;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 18));
            painter.drawRoundedRect(rect().adjusted(4, 1, -4, -1), 8, 8);
        }
        painter.setPen(marked_ ? kAccent : QColor(Qt::white));
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(13);
        painter.setFont(font);
        painter.drawText(rect().adjusted(16, 0, -32, 0), Qt::AlignVCenter | Qt::AlignLeft, label_);
        if (marked_) {
            QPen pen(kAccent, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(pen);
            const QPointF origin(width() - 22, height() / 2.0);
            painter.drawLine(origin + QPointF(-6, 0), origin + QPointF(-2, 4));
            painter.drawLine(origin + QPointF(-2, 4), origin + QPointF(5, -4));
        }
    }
    void enterEvent(QEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QString label_;
    bool marked_{false};
};

class TrackRow final : public QAbstractButton {
public:
    explicit TrackRow(const QString& title, const QString& detail, QWidget* parent)
        : QAbstractButton(parent), title_(title), detail_(detail) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFixedHeight(52);
    }

    void setCurrent(const bool current) {
        current_ = current;
        update();
    }
    void setCaption(const QString& title, const QString& detail) {
        title_ = title;
        detail_ = detail;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 16));
            painter.drawRoundedRect(rect().adjusted(6, 2, -6, -2), 8, 8);
        }
        const QRect icon(12, 6, 40, 40);
        painter.setPen(Qt::NoPen);
        painter.setBrush(current_ ? kAccent : QColor(0x3A, 0x3A, 0x3C));
        painter.drawRoundedRect(icon, 8, 8);
        painter.setBrush(Qt::white);
        const QRectF note(icon);
        painter.drawEllipse(QPointF(note.left() + 15, note.top() + 26), 5, 4);
        painter.drawRoundedRect(QRectF(note.left() + 19, note.top() + 10, 2, 16), 1, 1);
        painter.setPen(current_ ? kAccent : Qt::white);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(13);
        painter.setFont(font);
        const int textLeft = 64;
        const QString elided = QFontMetrics(font).elidedText(title_, Qt::ElideRight, width() - textLeft - 16);
        painter.drawText(QRect(textLeft, 8, width() - textLeft - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, elided);
        painter.setPen(QColor(0x8E, 0x8E, 0x93));
        font.setPixelSize(11);
        painter.setFont(font);
        painter.drawText(QRect(textLeft, 28, width() - textLeft - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, detail_);
    }
    void enterEvent(QEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QString title_;
    QString detail_;
    bool current_{false};
};

class PlaylistPopup final : public PlayerPopup {
public:
    explicit PlaylistPopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedWidth(344);
        scroll_ = new QScrollArea(this);
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setWidgetResizable(true);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; }"
            "QScrollBar:vertical { width: 8px; background: transparent; margin: 4px 0; }"
            "QScrollBar::handle:vertical { background: rgba(255,255,255,70); border-radius: 4px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));
        scroll_->viewport()->setAutoFillBackground(false);
        host_ = new QWidget();
        host_->setAutoFillBackground(false);
        host_->setStyleSheet(QStringLiteral("background: transparent;"));
        scroll_->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
        rows_ = new QVBoxLayout(host_);
        rows_->setContentsMargins(0, 0, 0, 0);
        rows_->setSpacing(0);
        scroll_->setWidget(host_);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->addWidget(scroll_);
    }

    void sync(const std::vector<MusicTrack>& tracks, const int index) {
        std::vector<std::int64_t> ids;
        ids.reserve(tracks.size());
        for (const auto& track : tracks) {
            ids.push_back(track.id);
        }
        if (ids != ids_) {
            ids_ = ids;
            while (QLayoutItem* item = rows_->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            rowWidgets_.clear();
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
                auto* row = new TrackRow(QString(), QString(), host_);
                rows_->addWidget(row);
                rowWidgets_.push_back(row);
                connect(row, &QAbstractButton::clicked, this, [this, i] {
                    if (picked) {
                        picked(i);
                    }
                });
            }
        }
        for (int i = 0; i < static_cast<int>(rowWidgets_.size()); ++i) {
            QString title;
            QString detail;
            if (labels) {
                labels(i, &title, &detail);
            }
            rowWidgets_[static_cast<std::size_t>(i)]->setCaption(title, detail);
            rowWidgets_[static_cast<std::size_t>(i)]->setCurrent(i == index);
        }
        const int list = std::min(static_cast<int>(tracks.size()) * 52, 280);
        scroll_->setFixedHeight(std::max(52, list));
        setFixedHeight(scroll_->height() + 12);
        if (index >= 0 && index < static_cast<int>(rowWidgets_.size())) {
            scroll_->ensureWidgetVisible(rowWidgets_[static_cast<std::size_t>(index)], 0, 24);
        }
    }

    std::function<void(int)> picked;
    std::function<void(int index, QString* title, QString* detail)> labels;

private:
    QScrollArea* scroll_{nullptr};
    QWidget* host_{nullptr};
    QVBoxLayout* rows_{nullptr};
    std::vector<TrackRow*> rowWidgets_;
    std::vector<std::int64_t> ids_;
};

class OrderPopup final : public PlayerPopup {
public:
    explicit OrderPopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedWidth(220);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(0);
        reverse_ = new MenuRow(QStringLiteral("Reverse order"), this);
        shuffle_ = new MenuRow(QStringLiteral("Shuffle"), this);
        layout->addWidget(reverse_);
        layout->addWidget(shuffle_);
        connect(reverse_, &QAbstractButton::clicked, this, [this] {
            if (picked) {
                picked(1);
            }
        });
        connect(shuffle_, &QAbstractButton::clicked, this, [this] {
            if (picked) {
                picked(2);
            }
        });
    }

    void sync(const int order) {
        reverse_->setMarked(order == 1);
        shuffle_->setMarked(order == 2);
    }

    std::function<void(int)> picked;

private:
    MenuRow* reverse_{nullptr};
    MenuRow* shuffle_{nullptr};
};

class SpeedPopup final : public PlayerPopup {
public:
    explicit SpeedPopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedWidth(248);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(2);
        readout_ = new QLabel(this);
        readout_->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(12);
        font.setBold(true);
        readout_->setFont(font);
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setRange(5, 25);
        slider_->setFocusPolicy(Qt::NoFocus);
        slider_->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 4px; background: #3a3a3c; border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: #3390ec; border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0; "
            "background: white; border-radius: 6px; }"));
        layout->addWidget(readout_);
        layout->addWidget(slider_);
        connect(slider_, &QSlider::valueChanged, this, [this](const int value) {
            readout_->setText(speedLabel(value * 100));
            if (!suppress_ && picked) {
                picked(value * 100);
            }
        });
        const struct {
            int milli;
            const char* label;
        } points[] = {
            {500, "Slow"},
            {1000, "Normal"},
            {1200, "Medium"},
            {1500, "Fast"},
            {1700, "Very fast"},
            {2000, "Super fast"},
        };
        for (const auto& point : points) {
            auto* row = new MenuRow(QString::fromUtf8(point.label), this);
            layout->addWidget(row);
            rows_.push_back({point.milli, row});
            connect(row, &QAbstractButton::clicked, this, [this, milli = point.milli] {
                if (picked) {
                    picked(milli);
                }
            });
        }
    }

    void sync(const int milli) {
        suppress_ = true;
        slider_->setValue(std::clamp(milli / 100, 5, 25));
        readout_->setText(speedLabel(milli));
        suppress_ = false;
        for (const auto& row : rows_) {
            row.widget->setMarked(std::abs(row.milli - milli) < 40);
        }
    }

    std::function<void(int)> picked;

private:
    struct Row {
        int milli;
        MenuRow* widget;
    };
    QLabel* readout_{nullptr};
    QSlider* slider_{nullptr};
    bool suppress_{false};
    std::vector<Row> rows_;
};

class VolumePopup final : public PlayerPopup {
public:
    explicit VolumePopup(QWidget* owner) : PlayerPopup(owner) {
        setFixedSize(46, 132);
        setMouseTracking(true);
    }

    void sync(const int percent) {
        percent_ = std::clamp(percent, 0, 100);
        update();
    }

    std::function<void(int)> picked;

protected:
    void paintEvent(QPaintEvent* event) override {
        PlayerPopup::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect track(width() / 2 - 2, 18, 4, height() - 36);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x3A, 0x3A, 0x3C));
        painter.drawRoundedRect(track, 2, 2);
        const int filled = track.height() * percent_ / 100;
        painter.setBrush(kAccent);
        painter.drawRoundedRect(QRect(track.left(), track.bottom() - filled + 1, track.width(), filled), 2, 2);
        const int handleY = track.bottom() - filled;
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPoint(track.center().x(), handleY), 6, 6);
    }
    void mousePressEvent(QMouseEvent* event) override { apply(event->pos().y()); }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            apply(event->pos().y());
        }
    }

private:
    void apply(const int y) {
        const int top = 18;
        const int height = this->height() - 36;
        const int percent = height <= 0
            ? 0
            : std::clamp(100 - (y - top) * 100 / height, 0, 100);
        percent_ = percent;
        update();
        if (picked) {
            picked(percent_);
        }
    }

    int percent_{100};
};

SongTags songFromFileName(const QString& fileName) {
    SongTags song;
    const QFileInfo info(fileName);
    QString base = info.completeBaseName().trimmed();
    if (base.isEmpty()) {
        base = fileName.trimmed();
    }
    const QString original = base;
    const QRegularExpression leading(QStringLiteral("^\\s*(?:\\d{1,3}\\s*[.\\-_]\\s+)+"));
    base.remove(leading);
    base = base.trimmed();
    if (base.isEmpty()) {
        base = original;
    }
    const QStringList separators{
        QStringLiteral(" — "), QStringLiteral(" – "), QStringLiteral(" - ")};
    for (const QString& separator : separators) {
        const int at = base.indexOf(separator);
        if (at > 0 && at + separator.size() < base.size()) {
            song.artist = base.left(at).trimmed();
            song.title = base.mid(at + separator.size()).trimmed();
            return song;
        }
    }
    song.title = base;
    return song;
}

QPixmap paintNotePlate(const int side) {
    QPixmap plate(side, side);
    plate.fill(Qt::transparent);
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kAccent);
    painter.drawRoundedRect(QRectF(0, 0, side, side), side * 0.18, side * 0.18);
    painter.setBrush(Qt::white);
    const double s = side;
    painter.drawEllipse(QPointF(s * 0.38, s * 0.66), s * 0.12, s * 0.09);
    painter.drawRoundedRect(QRectF(s * 0.48, s * 0.28, s * 0.045, s * 0.40), 1, 1);
    return plate;
}

QIcon musicTrayIcon() {
    QIcon icon;
    for (const int side : {16, 20, 24, 32, 48}) {
        icon.addPixmap(paintNotePlate(side));
    }
    return icon;
}

QPixmap roundedCover(const QImage& image, const int side) {
    if (image.isNull()) {
        return paintNotePlate(side);
    }
    QPixmap plate(side, side);
    plate.fill(Qt::transparent);
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, side, side), 10, 10);
    painter.setClipPath(clip);
    painter.drawImage(QRect(0, 0, side, side), image);
    return plate;
}

class NowPlayingCard final : public QWidget {
public:
    explicit NowPlayingCard(QWidget* owner)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(340);
        auto* root = new QHBoxLayout(this);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(12);
        cover_ = new QLabel(this);
        cover_->setFixedSize(84, 84);
        cover_->setPixmap(roundedCover({}, 84));
        cover_->setAttribute(Qt::WA_TransparentForMouseEvents);
        root->addWidget(cover_, 0, Qt::AlignTop);

        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        title_ = new QLabel(this);
        artist_ = new QLabel(this);
        album_ = new QLabel(this);
        QFont titleFont(QStringLiteral("Segoe UI"));
        titleFont.setPixelSize(15);
        titleFont.setBold(true);
        title_->setFont(titleFont);
        title_->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
        artist_->setStyleSheet(QStringLiteral("color: #d0d0d4; background: transparent;"));
        album_->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
        QFont small(QStringLiteral("Segoe UI"));
        small.setPixelSize(12);
        artist_->setFont(small);
        album_->setFont(small);
        title_->setWordWrap(false);
        title_->setAttribute(Qt::WA_TransparentForMouseEvents);
        artist_->setAttribute(Qt::WA_TransparentForMouseEvents);
        album_->setAttribute(Qt::WA_TransparentForMouseEvents);
        column->addWidget(title_);
        column->addWidget(artist_);
        column->addWidget(album_);
        album_->setVisible(false);

        timeline_ = new QSlider(Qt::Horizontal, this);
        timeline_->setRange(0, 1000);
        timeline_->setFocusPolicy(Qt::NoFocus);
        timeline_->setFixedHeight(16);
        timeline_->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 3px; background: #3a3a3c; border-radius: 1px; }"
            "QSlider::sub-page:horizontal { background: #3390ec; border-radius: 1px; }"
            "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -4px 0; "
            "background: white; border-radius: 5px; }"));
        column->addSpacing(4);
        column->addWidget(timeline_);

        auto* controls = new QHBoxLayout();
        controls->setSpacing(2);
        previous_ = new GlyphButton(Glyph::Previous, this);
        play_ = new GlyphButton(Glyph::Pause, this);
        next_ = new GlyphButton(Glyph::Next, this);
        play_->setActive(true);
        previous_->setFixedSize(32, 32);
        play_->setFixedSize(36, 32);
        next_->setFixedSize(32, 32);
        previous_->setToolTip(QStringLiteral("Previous"));
        play_->setToolTip(QStringLiteral("Pause"));
        next_->setToolTip(QStringLiteral("Next"));
        volume_ = new QSlider(Qt::Horizontal, this);
        volume_->setRange(0, 100);
        volume_->setFocusPolicy(Qt::NoFocus);
        volume_->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 4px; background: #3a3a3c; border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: #3390ec; border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0; "
            "background: white; border-radius: 6px; }"));
        controls->addWidget(previous_);
        controls->addWidget(play_);
        controls->addWidget(next_);
        controls->addWidget(volume_, 1);
        column->addSpacing(6);
        column->addLayout(controls);
        root->addLayout(column, 1);

        connect(previous_, &QAbstractButton::clicked, this, [this] { if (onPrevious) onPrevious(); });
        connect(play_, &QAbstractButton::clicked, this, [this] { if (onPlay) onPlay(); });
        connect(next_, &QAbstractButton::clicked, this, [this] { if (onNext) onNext(); });
        connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
            if (!volumeGuard_ && onVolume) {
                onVolume(value);
            }
        });
        connect(timeline_, &QSlider::sliderPressed, this, [this] { timelineScrub_ = true; });
        connect(timeline_, &QSlider::sliderReleased, this, [this] {
            timelineScrub_ = false;
            if (onSeek) {
                onSeek(timeline_->value());
            }
        });

        hideButton_ = new GlyphButton(Glyph::Close, this);
        hideButton_->setFixedSize(22, 22);
        hideButton_->setToolTip(QStringLiteral("Hide"));
        connect(hideButton_, &QAbstractButton::clicked, this, [this] {
            hide();
            if (onClose) {
                onClose();
            }
        });
    }

    void placeHideButton() {
        if (hideButton_ != nullptr) {
            hideButton_->move(width() - hideButton_->width() - 8, 8);
            hideButton_->raise();
        }
    }

    void setTimeline(const int value) {
        if (timelineScrub_ || timeline_ == nullptr || timeline_->value() == value) {
            return;
        }
        timeline_->setValue(value);
    }

    void present(
        const QString& title,
        const QString& artist,
        const QString& album,
        const QImage& cover,
        const bool playing,
        const int volume,
        const bool hasPrevious,
        const bool hasNext) {
        const QFontMetrics titleMetrics(title_->font());
        title_->setText(titleMetrics.elidedText(title.isEmpty() ? QStringLiteral("Nothing to play") : title, Qt::ElideRight, 210));
        artist_->setText(QFontMetrics(artist_->font()).elidedText(artist, Qt::ElideRight, 210));
        artist_->setVisible(!artist.isEmpty());
        album_->setText(QFontMetrics(album_->font()).elidedText(album, Qt::ElideRight, 210));
        album_->setVisible(!album.isEmpty());
        cover_->setPixmap(roundedCover(cover, 84));
        play_->setGlyph(playing ? Glyph::Pause : Glyph::Play);
        play_->setToolTip(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
        previous_->setEnabled(hasPrevious);
        next_->setEnabled(hasNext);
        if (volume_->value() != volume) {
            volumeGuard_ = true;
            volume_->setValue(volume);
            volumeGuard_ = false;
        }
        adjustSize();
        placeHideButton();
    }

    std::function<void()> onPrevious;
    std::function<void()> onPlay;
    std::function<void()> onNext;
    std::function<void(int)> onVolume;
    std::function<void(int)> onSeek;
    std::function<void()> onClose;
    std::function<void()> onMoved;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255, 36)));
        painter.setBrush(kCard);
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 16, 16);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        placeHideButton();
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            dragOffset_ = event->globalPos() - frameGeometry().topLeft();
        }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            move(event->globalPos() - dragOffset_);
        }
        QWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            if (onMoved) {
                onMoved();
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QLabel* cover_{nullptr};
    QLabel* title_{nullptr};
    QLabel* artist_{nullptr};
    QLabel* album_{nullptr};
    GlyphButton* previous_{nullptr};
    GlyphButton* play_{nullptr};
    GlyphButton* next_{nullptr};
    QSlider* volume_{nullptr};
    QSlider* timeline_{nullptr};
    GlyphButton* hideButton_{nullptr};
    bool volumeGuard_{false};
    bool timelineScrub_{false};
    bool dragging_{false};
    QPoint dragOffset_{};
};

MusicWindow::MusicWindow(QWidget* parent)
    : QWidget(parent)
    , decoder_(std::make_unique<MediaDecoder>())
    , stretch_(std::make_unique<TimeStretch>()) {
    setObjectName(QStringLiteral("musicBar"));
    setFixedHeight(kBarHeight);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    QFont textFont(QStringLiteral("Segoe UI"));
    textFont.setPixelSize(13);

    previousButton_ = new GlyphButton(Glyph::Previous, this);
    playButton_ = new GlyphButton(Glyph::Pause, this);
    nextButton_ = new GlyphButton(Glyph::Next, this);
    volumeButton_ = new GlyphButton(Glyph::VolumeFull, this);
    orderButton_ = new GlyphButton(Glyph::Reverse, this);
    repeatButton_ = new GlyphButton(Glyph::Repeat, this);
    speedButton_ = new GlyphButton(Glyph::Close, this);
    speedButton_->setLabel(QStringLiteral("1X"));
    closeButton_ = new GlyphButton(Glyph::Close, this);
    playButton_->setActive(true);
    volumeButton_->setActive(true);
    previousButton_->setToolTip(QStringLiteral("Previous"));
    playButton_->setToolTip(QStringLiteral("Pause"));
    nextButton_->setToolTip(QStringLiteral("Next"));
    volumeButton_->setToolTip(QStringLiteral("Volume"));
    orderButton_->setToolTip(QStringLiteral("Playback order"));
    repeatButton_->setToolTip(QStringLiteral("Repeat"));
    speedButton_->setToolTip(QStringLiteral("Playback speed"));
    closeButton_->setToolTip(QStringLiteral("Close"));

    nameLabel_ = new QLabel(this);
    timeLabel_ = new QLabel(QStringLiteral("0:00"), this);
    nameLabel_->setFont(textFont);
    timeLabel_->setFont(textFont);
    nameLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    timeLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    nameLabel_->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    timeLabel_->setStyleSheet(QStringLiteral("color: #999999; background: transparent;"));
    nameLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    progress_ = new ProgressLine(this);
    playlist_ = new PlaylistPopup(this);
    orderMenu_ = new OrderPopup(this);
    speedMenu_ = new SpeedPopup(this);
    volumeMenu_ = new VolumePopup(this);

    previousButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    nextButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    playButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    closeButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    repeatButton_->entered = [this] { showOnly(nullptr, nullptr, false); };
    volumeButton_->entered = [this] { showOnly(volumeMenu_, volumeButton_, true); };
    orderButton_->entered = [this] { showOnly(orderMenu_, orderButton_, true); };
    speedButton_->entered = [this] { showOnly(speedMenu_, speedButton_, true); };
    for (auto* button : {previousButton_, playButton_, nextButton_, volumeButton_, orderButton_, repeatButton_, speedButton_, closeButton_}) {
        button->left = [this] { armHide(); };
    }

    connect(previousButton_, &QAbstractButton::clicked, this, [this] { advance(-1); });
    connect(nextButton_, &QAbstractButton::clicked, this, [this] { advance(1); });
    connect(playButton_, &QAbstractButton::clicked, this, [this] { togglePlay(); });
    connect(closeButton_, &QAbstractButton::clicked, this, &QWidget::close);
    connect(repeatButton_, &QAbstractButton::clicked, this, [this] {
        repeat_ = (repeat_ + 1) % 3;
        syncTransport();
        savePlayback();
    });
    connect(volumeButton_, &QAbstractButton::clicked, this, [this] { toggleMute(); });
    connect(speedButton_, &QAbstractButton::clicked, this, [this] { toggleSpeed(); });
    connect(orderButton_, &QAbstractButton::clicked, this, [this] { showOnly(orderMenu_, orderButton_, true); });

    playlist_->picked = [this](const int index) { playIndex(index); };
    playlist_->labels = [this](const int index, QString* title, QString* detail) {
        const SongTags song = songFor(index);
        if (title != nullptr) {
            *title = song.title;
        }
        if (detail == nullptr) {
            return;
        }
        if (!song.artist.isEmpty()) {
            *detail = song.artist;
        } else if (!song.album.isEmpty()) {
            *detail = song.album;
        } else if (index >= 0 && index < static_cast<int>(tracks_.size())) {
            const QFileInfo info(tracks_[static_cast<std::size_t>(index)].title);
            *detail = info.suffix().isEmpty() ? QStringLiteral("Audio") : info.suffix().toUpper();
        }
    };
    orderMenu_->picked = [this](const int mode) { setOrder(order_ == mode ? 0 : mode); };
    speedMenu_->picked = [this](const int milli) { setSpeedMilli(milli); };
    volumeMenu_->picked = [this](const int percent) { setVolumePercent(percent); };
    progress_->moved = [this](const int value) {
        scrubbing_ = true;
        const auto duration = durationMs_.load();
        timeLabel_->setText(formatTime(duration > 0 ? duration * value / 1000 : 0));
    };
    progress_->released = [this](const int value) {
        scrubbing_ = false;
        const auto duration = durationMs_.load();
        if (duration > 0) {
            seekTo(duration * value / 1000);
        }
    };

    sink_ = new AudioSink(this);
    clock_ = new QTimer(this);
    clock_->setInterval(200);
    connect(clock_, &QTimer::timeout, this, [this] { refreshClock(); });
    clock_->start();
    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    hideTimer_->setInterval(180);
    connect(hideTimer_, &QTimer::timeout, this, [this] {
        if (!cursorOnChrome()) {
            hidePopups();
        }
    });
    qApp->installEventFilter(this);

    tray_ = new QSystemTrayIcon(musicTrayIcon(), this);
    tray_->setToolTip(QStringLiteral("Mega Vault Protect"));
    trayCard_ = new NowPlayingCard(this);
    trayCard_->onPrevious = [this] { advance(-1); };
    trayCard_->onPlay = [this] { togglePlay(); };
    trayCard_->onNext = [this] { advance(1); };
    trayCard_->onVolume = [this](const int percent) { setVolumePercent(percent); };
    trayCard_->onSeek = [this](const int value) {
        const auto duration = durationMs_.load();
        if (duration > 0) {
            seekTo(duration * value / 1000);
        }
    };
    trayCard_->onClose = [this] { trayDismissedAt_ = QDateTime::currentMSecsSinceEpoch(); };
    trayCard_->onMoved = [this] {
        if (trayCard_ == nullptr) {
            return;
        }
        QSettings settings;
        settings.setValue(QStringLiteral("player/popupX"), trayCard_->x());
        settings.setValue(QStringLiteral("player/popupY"), trayCard_->y());
        popupPlaced_ = true;
    };
    connect(tray_, &QSystemTrayIcon::activated, this, [this](const QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger
            && reason != QSystemTrayIcon::DoubleClick
            && reason != QSystemTrayIcon::Context) {
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() - trayDismissedAt_ < 350) {
            return;
        }
        if (trayCard_ != nullptr && trayCard_->isVisible()) {
            trayCard_->hide();
            return;
        }
        showTrayCard();
    });
    loadPlayback();
}

MusicWindow::~MusicWindow() {
    qApp->removeEventFilter(this);
    hidePopups();
    stopWorker();
}

void MusicWindow::playQueue(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::vector<MusicTrack>& tracks,
    const int index) {
    vault_ = vault;
    tracks_ = tracks;
    index_ = tracks_.empty() ? 0 : std::max(0, std::min(index, static_cast<int>(tracks_.size()) - 1));
    history_.clear();
    refillBag();
    showTrack();
    startWorker();
    if (tray_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_->show();
    }
    updateTray();
}

void MusicWindow::showTrack() {
    errorText_.clear();
    if (!tracks_.empty() && coverId_ != tracks_[static_cast<std::size_t>(index_)].id) {
        cover_ = QImage();
        coverId_ = 0;
    }
    paused_.store(false);
    playButton_->setGlyph(Glyph::Pause);
    playButton_->setToolTip(QStringLiteral("Pause"));
    timeLabel_->setText(QStringLiteral("0:00"));
    progress_->setValue(0);
    applySong();
    syncTransport();
}

void MusicWindow::startWorker() {
    stopWorker();
    if (!vault_ || tracks_.empty()) {
        return;
    }
    stop_.store(false);
    workerFinished_.store(false);
    ended_.store(false);
    paused_.store(false);
    seekMs_.store(-1);
    originMs_.store(0);
    consumedFrames_.store(0);
    durationMs_.store(0);
    sampleRate_.store(0);
    {
        std::lock_guard<std::mutex> guard(errorMutex_);
        error_.clear();
    }
    stretch_->reset();
    if (sink_ != nullptr) {
        sink_->clear();
    }
    const auto vault = vault_;
    const auto id = tracks_[static_cast<std::size_t>(index_)].id;
    worker_ = std::thread([this, vault, id] {
        QImage cover;
        if (vault) {
            // Read the cover before the decoder opens so the two FFmpeg
            // contexts are not live together, and so a real cover replaces
            // a previously stored note.
            const auto stored = vault->display_thumbnail(id, 480U);
            if (stored && !stored.value().bytes.empty()) {
                cover.loadFromData(
                    reinterpret_cast<const uchar*>(stored.value().bytes.data()),
                    static_cast<int>(stored.value().bytes.size()));
            }
        }
        QString opened;
        decoder_->set_stream_cache_bytes(1U << 20);
        decoder_->prepare_for_audio();
        if (!decoder_->open(vault, id, &opened) || !decoder_->has_audio()) {
            std::lock_guard<std::mutex> guard(errorMutex_);
            error_ = opened.isEmpty()
                ? std::string("This file has no audio stream.")
                : opened.toStdString();
            workerFinished_.store(true);
            return;
        }
        durationMs_.store(decoder_->duration_ms());
        sampleRate_.store(decoder_->audio_sample_rate());
        channels_.store(std::max(1, decoder_->audio_channels()));
        const AudioTags tags = decoder_->audio_tags();
        if (cover.isNull()) {
            cover = decoder_->attached_cover();
        }
        if (cover.width() > 480 || cover.height() > 480) {
            cover = cover.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QMetaObject::invokeMethod(this, [this, id, tags, cover] {
            publishTrackInfo(id, tags.title, tags.artist, tags.album, cover);
        }, Qt::QueuedConnection);
        bool flushed = false;
        while (!stop_.load()) {
            const auto seek = seekMs_.exchange(-1);
            if (seek >= 0) {
                (void)decoder_->seek_to(seek);
                consumedFrames_.store(0);
                originMs_.store(seek);
                stretch_->reset();
                flushed = false;
                const int ticket = clearRequest_.fetch_add(1) + 1;
                while (!stop_.load() && clearApplied_.load() != ticket) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            if (paused_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const int rate = std::max(1, sampleRate_.load());
            const int channels = std::max(1, channels_.load());
            const auto buffered = sink_ != nullptr ? sink_->buffered_samples() : 0U;
            if (buffered > static_cast<std::size_t>(rate)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const double speed = std::max(0.5, speedMilli_.load() / 1000.0);
            if (!decoder_->pump_audio(rate / 5 * channels, nullptr)) {
                if (!flushed) {
                    std::size_t consumed = 0;
                    auto tail = stretch_->flush(channels, speed, &consumed);
                    consumedFrames_.fetch_add(static_cast<std::int64_t>(consumed));
                    if (!tail.empty() && sink_ != nullptr) {
                        sink_->append(tail.data(), tail.size());
                    }
                    flushed = true;
                }
                if (sink_ == nullptr || sink_->buffered_samples() == 0U) {
                    ended_.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            flushed = false;
            auto samples = decoder_->take_audio_samples();
            std::size_t consumed = 0;
            auto shaped = stretch_->process(
                samples.data(), samples.size(), channels, speed, &consumed);
            consumedFrames_.fetch_add(static_cast<std::int64_t>(consumed));
            if (!shaped.empty() && sink_ != nullptr) {
                sink_->append(shaped.data(), shaped.size());
            }
        }
        decoder_->close();
        workerFinished_.store(true);
    });
}

void MusicWindow::stopWorker() {
    stop_.store(true);
    if (!worker_.joinable()) {
        workerFinished_.store(true);
        stop_.store(false);
        return;
    }
    while (!workerFinished_.load()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    worker_.join();
    workerFinished_.store(true);
    stop_.store(false);
}

bool MusicWindow::nextAvailable() const {
    const int count = static_cast<int>(tracks_.size());
    if (count <= 0) {
        return false;
    }
    if (count == 1) {
        return repeat_ == 2;
    }
    if (order_ == 2) {
        return !bag_.empty() || repeat_ == 2;
    }
    const int step = order_ == 1 ? -1 : 1;
    const int next = index_ + step;
    return (next >= 0 && next < count) || repeat_ == 2;
}

bool MusicWindow::previousAvailable() const {
    const int count = static_cast<int>(tracks_.size());
    if (count <= 0) {
        return false;
    }
    if (order_ == 2) {
        return !history_.empty();
    }
    if (count == 1) {
        return repeat_ == 2;
    }
    const int step = order_ == 1 ? 1 : -1;
    const int previous = index_ + step;
    return (previous >= 0 && previous < count) || repeat_ == 2;
}

void MusicWindow::refillBag() {
    bag_.clear();
    const int count = static_cast<int>(tracks_.size());
    std::vector<int> ids;
    ids.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (i != index_) {
            ids.push_back(i);
        }
    }
    std::mt19937 rng{std::random_device{}()};
    std::shuffle(ids.begin(), ids.end(), rng);
    bag_ = std::move(ids);
}

bool MusicWindow::advance(const int delta) {
    if (tracks_.empty() || delta == 0) {
        return false;
    }
    const int count = static_cast<int>(tracks_.size());
    int next = index_;
    if (order_ == 2) {
        if (delta > 0) {
            if (bag_.empty()) {
                if (repeat_ != 2) {
                    return false;
                }
                refillBag();
                if (bag_.empty()) {
                    next = index_;
                }
            }
            if (!bag_.empty()) {
                history_.push_back(index_);
                next = bag_.back();
                bag_.pop_back();
            }
        } else {
            if (history_.empty()) {
                return false;
            }
            bag_.push_back(index_);
            next = history_.back();
            history_.pop_back();
        }
    } else {
        const int step = (order_ == 1 ? -1 : 1) * delta;
        next = index_ + step;
        if (next < 0 || next >= count) {
            if (repeat_ != 2) {
                return false;
            }
            next = next < 0 ? count - 1 : 0;
        }
    }
    index_ = next;
    showTrack();
    startWorker();
    return true;
}

void MusicWindow::playIndex(const int index) {
    if (index < 0 || index >= static_cast<int>(tracks_.size()) || index == index_) {
        return;
    }
    history_.clear();
    index_ = index;
    refillBag();
    showTrack();
    startWorker();
}

void MusicWindow::togglePlay() {
    if (tracks_.empty()) {
        return;
    }
    if (ended_.load() && workerFinished_.load()) {
        showTrack();
        startWorker();
        return;
    }
    const bool pause = !paused_.load();
    paused_.store(pause);
    playButton_->setGlyph(pause ? Glyph::Play : Glyph::Pause);
    playButton_->setToolTip(pause ? QStringLiteral("Play") : QStringLiteral("Pause"));
    updateTray();
    if (output_ != nullptr) {
        if (pause) {
            output_->suspend();
        } else {
            output_->resume();
        }
    }
}

void MusicWindow::seekTo(const std::int64_t ms) {
    seekMs_.store(std::max<std::int64_t>(0, ms));
    ended_.store(false);
    if (paused_.load()) {
        paused_.store(false);
        playButton_->setGlyph(Glyph::Pause);
        playButton_->setToolTip(QStringLiteral("Pause"));
        if (output_ != nullptr) {
            output_->resume();
        }
    }
}

void MusicWindow::seekBy(const std::int64_t deltaMs) {
    const auto duration = durationMs_.load();
    auto target = positionMs() + deltaMs;
    if (duration > 0) {
        target = std::min(target, duration);
    }
    seekTo(std::max<std::int64_t>(0, target));
}

void MusicWindow::ensureOutput() {
    const int rate = sampleRate_.load();
    const int channels = std::max(1, channels_.load());
    if (rate <= 0) {
        return;
    }
    if (output_ != nullptr && outputRate_ == rate && outputChannels_ == channels) {
        const float volume = muted_.load() ? 0.f : volumePercent_.load() / 100.f;
        output_->setVolume(volume);
        return;
    }
    if (output_ != nullptr) {
        output_->stop();
        output_->deleteLater();
        output_ = nullptr;
    }
    QAudioFormat format;
    format.setSampleRate(rate);
    format.setChannelCount(channels);
    format.setSampleSize(16);
    format.setCodec(QStringLiteral("audio/pcm"));
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    output_ = new QAudioOutput(format, this);
    output_->setBufferSize(rate / 2);
    output_->setVolume(muted_.load() ? 0.f : volumePercent_.load() / 100.f);
    output_->start(sink_);
    outputRate_ = rate;
    outputChannels_ = channels;
    if (paused_.load()) {
        output_->suspend();
    }
}

void MusicWindow::applySeekClear() {
    const int requested = clearRequest_.load();
    if (requested == clearApplied_.load()) {
        return;
    }
    if (sink_ != nullptr) {
        sink_->clear();
    }
    if (output_ != nullptr) {
        output_->stop();
        output_->start(sink_);
        if (paused_.load()) {
            output_->suspend();
        }
    }
    clearApplied_.store(requested);
}

QString MusicWindow::formatTime(const std::int64_t ms) const {
    const auto total = std::max<std::int64_t>(0, ms) / 1000;
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto seconds = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QChar('0'));
}

std::int64_t MusicWindow::positionMs() const {
    const int rate = sampleRate_.load();
    if (rate <= 0) {
        return 0;
    }
    const int channels = std::max(1, channels_.load());
    const double speed = std::max(0.5, speedMilli_.load() / 1000.0);
    const auto buffered = sink_ == nullptr
        ? 0
        : static_cast<std::int64_t>(sink_->buffered_samples() / static_cast<std::size_t>(channels));
    const auto heard = consumedFrames_.load()
        - static_cast<std::int64_t>(std::llround(static_cast<double>(buffered) * speed));
    return std::max<std::int64_t>(0, originMs_.load() + heard * 1000 / rate);
}

void MusicWindow::refreshClock() {
    ensureOutput();
    applySeekClear();
    if (workerFinished_.load()) {
        std::string error;
        {
            std::lock_guard<std::mutex> guard(errorMutex_);
            error = error_;
        }
        if (!error.empty()) {
            if (errorText_.isEmpty()) {
                errorText_ = QString::fromStdString(error);
                applyName();
                playButton_->setGlyph(Glyph::Play);
                playButton_->setToolTip(QStringLiteral("Play"));
            }
        } else if (ended_.exchange(false)) {
            onTrackEnded();
            return;
        }
    }
    const auto duration = durationMs_.load();
    progress_->setEnabled(duration > 0);
    if (scrubbing_) {
        return;
    }
    const auto position = positionMs();
    if (duration > 0) {
        const int value = static_cast<int>(std::min<std::int64_t>(1000, position * 1000 / duration));
        progress_->setValue(value);
        if (trayCard_ != nullptr && trayCard_->isVisible()) {
            trayCard_->setTimeline(value);
        }
    }
    const QString text = formatTime(position);
    if (timeLabel_->text() != text) {
        timeLabel_->setText(text);
        layoutControls();
    }
}

void MusicWindow::onTrackEnded() {
    if (repeat_ == 1) {
        showTrack();
        startWorker();
        return;
    }
    if (!advance(1)) {
        paused_.store(true);
        playButton_->setGlyph(Glyph::Play);
        playButton_->setToolTip(QStringLiteral("Play"));
        if (output_ != nullptr) {
            output_->suspend();
        }
        const auto duration = durationMs_.load();
        if (duration > 0) {
            progress_->setValue(1000);
            timeLabel_->setText(formatTime(duration));
        }
        updateTray();
    }
}

void MusicWindow::layoutControls() {
    const bool wide = width() >= 460 || barOver_ || cursorOnChrome();
    const bool skip = previousAvailable() || nextAvailable();
    previousButton_->setVisible(skip);
    nextButton_->setVisible(skip);
    volumeButton_->setVisible(wide);
    orderButton_->setVisible(wide);
    repeatButton_->setVisible(wide);
    speedButton_->setVisible(wide);

    int x = 9;
    if (skip) {
        previousButton_->setGeometry(x, 0, 24, kControlHeight);
        x += 25;
    }
    playButton_->setGeometry(x, 0, 24, kControlHeight);
    x += 24;
    if (skip) {
        x += 1;
        nextButton_->setGeometry(x, 0, 24, kControlHeight);
        x += 24;
    }
    x += 8;
    const int nameLeft = x;

    int right = width();
    closeButton_->setGeometry(right - 39, 0, 39, kControlHeight);
    right -= 39;
    if (wide) {
        speedButton_->setGeometry(right - 30, 0, 30, kControlHeight);
        right -= 30;
        repeatButton_->setGeometry(right - 30, 0, 30, kControlHeight);
        right -= 30;
        orderButton_->setGeometry(right - 30, 0, 30, kControlHeight);
        right -= 30;
        volumeButton_->setGeometry(right - 34, 0, 34, kControlHeight);
        right -= 34;
    }
    right -= 8;
    const int timeWidth = std::max(32, timeLabel_->fontMetrics().horizontalAdvance(timeLabel_->text()) + 2);
    timeLabel_->setGeometry(right - timeWidth, 0, timeWidth, kControlHeight);
    const int nameWidth = std::max(0, right - timeWidth - 8 - nameLeft);
    nameLabel_->setGeometry(nameLeft, 0, nameWidth, kControlHeight);
    nameRect_ = nameLabel_->geometry();
    progress_->setGeometry(0, kControlHeight, width(), kBarHeight - kControlHeight);
    applyName();
}

void MusicWindow::applyName() {
    const bool failed = !errorText_.isEmpty();
    nameLabel_->setStyleSheet(failed
        ? QStringLiteral("color: #ff6b6b; background: transparent;")
        : QStringLiteral("color: white; background: transparent;"));
    const QString full = failed ? errorText_ : titleText_;
    nameLabel_->setText(nameLabel_->fontMetrics().elidedText(full, Qt::ElideRight, std::max(0, nameLabel_->width())));
}

void MusicWindow::syncTransport() {
    const bool skip = previousAvailable() || nextAvailable();
    previousButton_->setVisible(skip);
    nextButton_->setVisible(skip);
    previousButton_->setEnabled(previousAvailable());
    nextButton_->setEnabled(nextAvailable());
    switch (repeat_) {
    case 1:
        repeatButton_->setGlyph(Glyph::RepeatOne);
        repeatButton_->setActive(true);
        break;
    case 2:
        repeatButton_->setGlyph(Glyph::Repeat);
        repeatButton_->setActive(true);
        break;
    default:
        repeatButton_->setGlyph(Glyph::Repeat);
        repeatButton_->setActive(false);
        break;
    }
    if (order_ == 2) {
        orderButton_->setGlyph(Glyph::Shuffle);
        orderButton_->setActive(true);
    } else {
        orderButton_->setGlyph(Glyph::Reverse);
        orderButton_->setActive(order_ == 1);
    }
    const int volume = muted_.load() ? 0 : volumePercent_.load();
    volumeButton_->setGlyph(volume <= 0 ? Glyph::VolumeOff : volume < 50 ? Glyph::VolumeLow : Glyph::VolumeFull);
    layoutControls();
    updateTray();
}

void MusicWindow::setSpeedMilli(const int milli) {
    const int snapped = std::clamp((milli + 50) / 100 * 100, 500, 2500);
    speedMilli_.store(snapped);
    if (snapped != 1000) {
        lastSpeedMilli_ = snapped;
    }
    speedButton_->setLabel(speedLabel(snapped));
    speedButton_->setActive(snapped != 1000);
    if (speedMenu_ != nullptr && speedMenu_->isVisible()) {
        speedMenu_->sync(snapped);
    }
    savePlayback();
}

void MusicWindow::toggleSpeed() {
    if (std::abs(speedMilli_.load() - 1000) < 50) {
        setSpeedMilli(lastSpeedMilli_);
    } else {
        setSpeedMilli(1000);
    }
}

void MusicWindow::setOrder(const int order) {
    order_ = order;
    history_.clear();
    if (order_ == 2) {
        refillBag();
    } else {
        bag_.clear();
    }
    syncTransport();
    if (orderMenu_ != nullptr && orderMenu_->isVisible()) {
        orderMenu_->sync(order_);
    }
    savePlayback();
}

void MusicWindow::setVolumePercent(const int percent) {
    const int next = std::clamp(percent, 0, 100);
    if (next > 0) {
        rememberedVolume_ = next;
    }
    volumePercent_.store(next);
    muted_.store(next == 0);
    syncTransport();
    if (output_ != nullptr) {
        output_->setVolume(next / 100.f);
    }
    if (volumeMenu_ != nullptr && volumeMenu_->isVisible()) {
        volumeMenu_->sync(next);
    }
    savePlayback();
}

void MusicWindow::toggleMute() {
    if (!muted_.load() && volumePercent_.load() > 0) {
        rememberedVolume_ = volumePercent_.load();
        muted_.store(true);
    } else {
        muted_.store(false);
        if (volumePercent_.load() <= 0) {
            volumePercent_.store(rememberedVolume_ > 0 ? rememberedVolume_ : 80);
        }
    }
    syncTransport();
    if (output_ != nullptr) {
        output_->setVolume(muted_.load() ? 0.f : volumePercent_.load() / 100.f);
    }
    if (volumeMenu_ != nullptr && volumeMenu_->isVisible()) {
        volumeMenu_->sync(muted_.load() ? 0 : volumePercent_.load());
    }
    savePlayback();
}

void MusicWindow::showOnly(QWidget* popup, QWidget* anchor, const bool alignRight) {
    for (QWidget* other : {static_cast<QWidget*>(playlist_), static_cast<QWidget*>(orderMenu_), static_cast<QWidget*>(speedMenu_), static_cast<QWidget*>(volumeMenu_)}) {
        if (other != nullptr && other != popup) {
            other->hide();
        }
    }
    if (popup == nullptr || anchor == nullptr) {
        return;
    }
    if (popup == playlist_) {
        if (tracks_.empty()) {
            return;
        }
        playlist_->sync(tracks_, index_);
    } else if (popup == orderMenu_) {
        orderMenu_->sync(order_);
    } else if (popup == speedMenu_) {
        speedMenu_->sync(speedMilli_.load());
    } else if (popup == volumeMenu_) {
        volumeMenu_->sync(muted_.load() ? 0 : volumePercent_.load());
    }
    popup->adjustSize();
    const QPoint origin = anchor->mapToGlobal(QPoint(0, 0));
    int x = alignRight ? origin.x() + anchor->width() - popup->width() : origin.x();
    int y = origin.y() - popup->height() + 8;
    if (QScreen* screen = QGuiApplication::screenAt(origin)) {
        const QRect area = screen->availableGeometry();
        x = std::clamp(x, area.left() + 8, std::max(area.left() + 8, area.right() - popup->width() - 8));
        y = std::max(area.top() + 8, y);
    }
    popup->move(x, y);
    popup->show();
    popup->raise();
}

void MusicWindow::hidePopups() {
    for (QWidget* popup : {static_cast<QWidget*>(playlist_), static_cast<QWidget*>(orderMenu_), static_cast<QWidget*>(speedMenu_), static_cast<QWidget*>(volumeMenu_)}) {
        if (popup != nullptr) {
            popup->hide();
        }
    }
}

void MusicWindow::armHide() {
    hideTimer_->start();
}

bool MusicWindow::cursorOnChrome() const {
    const QPoint cursor = QCursor::pos();
    const auto hits = [&](QWidget* widget) {
        return widget != nullptr && widget->isVisible()
            && QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size()).contains(cursor);
    };
    if (hits(playlist_) || hits(orderMenu_) || hits(speedMenu_) || hits(volumeMenu_)) {
        return true;
    }
    if (!isVisible()) {
        return false;
    }
    if (QRect(mapToGlobal(nameRect_.topLeft()), nameRect_.size()).contains(cursor)) {
        return true;
    }
    return hits(volumeButton_) || hits(orderButton_) || hits(speedButton_);
}

void MusicWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutControls();
}

void MusicWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    painter.fillRect(0, 0, width(), 1, QColor(255, 255, 255, 28));
}

void MusicWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (nameRect_.contains(event->pos())) {
        if (trayCard_ != nullptr && trayCard_->isVisible()) {
            trayCard_->hide();
        } else {
            showTrayCard();
        }
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MusicWindow::mouseMoveEvent(QMouseEvent* event) {
    QWidget::mouseMoveEvent(event);
    if (nameRect_.contains(event->pos())) {
        showOnly(playlist_, nameLabel_, false);
    }
}

void MusicWindow::enterEvent(QEvent* event) {
    QWidget::enterEvent(event);
    barOver_ = true;
    layoutControls();
}

void MusicWindow::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    QTimer::singleShot(0, this, [this] {
        if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
            barOver_ = false;
            layoutControls();
        }
        armHide();
    });
}

bool MusicWindow::eventFilter(QObject* watched, QEvent* event) {
    if (!isVisible()) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::MouseButtonPress) {
        if (!cursorOnChrome() && !rect().contains(mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPos()))) {
            hidePopups();
        }
    }
    if (event->type() == QEvent::KeyPress) {
        if (QApplication::activeModalWidget() != nullptr) {
            return QWidget::eventFilter(watched, event);
        }
        auto* focus = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(focus) != nullptr || qobject_cast<QComboBox*>(focus) != nullptr) {
            return QWidget::eventFilter(watched, event);
        }
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && !key->isAutoRepeat()) {
            togglePlay();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MusicWindow::loadPlayback() {
    QSettings settings;
    volumePercent_.store(std::clamp(settings.value(QStringLiteral("player/volume"), 100).toInt(), 0, 100));
    muted_.store(settings.value(QStringLiteral("player/muted"), false).toBool());
    rememberedVolume_ = std::clamp(
        settings.value(QStringLiteral("player/rememberedVolume"), volumePercent_.load()).toInt(), 1, 100);
    order_ = std::clamp(settings.value(QStringLiteral("player/order"), 0).toInt(), 0, 2);
    repeat_ = std::clamp(settings.value(QStringLiteral("player/repeat"), 0).toInt(), 0, 2);
    const int speed = std::clamp(settings.value(QStringLiteral("player/speedMilli"), 1000).toInt(), 500, 2500);
    speedMilli_.store(speed);
    lastSpeedMilli_ = std::clamp(settings.value(QStringLiteral("player/lastSpeedMilli"), 1500).toInt(), 500, 2500);
    speedButton_->setLabel(speedLabel(speed));
    speedButton_->setActive(std::abs(speed - 1000) > 40);
    syncTransport();
}

void MusicWindow::savePlayback() {
    QSettings settings;
    settings.setValue(QStringLiteral("player/volume"), volumePercent_.load());
    settings.setValue(QStringLiteral("player/muted"), muted_.load());
    settings.setValue(QStringLiteral("player/rememberedVolume"), rememberedVolume_);
    settings.setValue(QStringLiteral("player/order"), order_);
    settings.setValue(QStringLiteral("player/repeat"), repeat_);
    settings.setValue(QStringLiteral("player/speedMilli"), speedMilli_.load());
    settings.setValue(QStringLiteral("player/lastSpeedMilli"), lastSpeedMilli_);
}

SongTags MusicWindow::songFor(const int index) const {
    SongTags song;
    if (index < 0 || index >= static_cast<int>(tracks_.size())) {
        return song;
    }
    song = songFromFileName(tracks_[static_cast<std::size_t>(index)].title);
    const auto known = tags_.find(tracks_[static_cast<std::size_t>(index)].id);
    if (known != tags_.end()) {
        if (!known->second.title.isEmpty()) {
            song.title = known->second.title;
        }
        if (!known->second.artist.isEmpty()) {
            song.artist = known->second.artist;
        }
        if (!known->second.album.isEmpty()) {
            song.album = known->second.album;
        }
    }
    if (song.title.isEmpty()) {
        song.title = tracks_[static_cast<std::size_t>(index)].title;
    }
    return song;
}

void MusicWindow::applySong() {
    if (tracks_.empty()) {
        songTitle_ = QStringLiteral("Nothing to play");
        artistText_.clear();
        albumText_.clear();
        titleText_ = songTitle_;
    } else {
        const SongTags song = songFor(index_);
        songTitle_ = song.title;
        artistText_ = song.artist;
        albumText_ = song.album;
        titleText_ = artistText_.isEmpty()
            ? songTitle_
            : artistText_ + QStringLiteral(" — ") + songTitle_;
    }
    applyName();
    if (playlist_ != nullptr) {
        playlist_->sync(tracks_, index_);
    }
    updateTray();
}

void MusicWindow::publishTrackInfo(
    const std::int64_t id,
    const QString& title,
    const QString& artist,
    const QString& album,
    const QImage& cover) {
    if (tracks_.empty() || tracks_[static_cast<std::size_t>(index_)].id != id) {
        return;
    }
    SongTags tags;
    tags.title = title.trimmed();
    tags.artist = artist.trimmed();
    tags.album = album.trimmed();
    if (!tags.title.isEmpty() || !tags.artist.isEmpty() || !tags.album.isEmpty()) {
        tags_[id] = tags;
    }
    if (!cover.isNull()) {
        cover_ = cover;
        coverId_ = id;
    }
    applySong();
}

void MusicWindow::updateTray() {
    if (tray_ == nullptr) {
        return;
    }
    QString tip = songTitle_.isEmpty() ? QStringLiteral("Mega Vault Protect") : songTitle_;
    if (!artistText_.isEmpty() && !tracks_.empty()) {
        tip = artistText_ + QStringLiteral(" — ") + songTitle_;
    }
    tray_->setToolTip(tip);
    if (trayCard_ == nullptr) {
        return;
    }
    const std::int64_t id = tracks_.empty() ? 0 : tracks_[static_cast<std::size_t>(index_)].id;
    trayCard_->present(
        tracks_.empty() ? QStringLiteral("Nothing to play") : songTitle_,
        artistText_,
        albumText_,
        coverId_ == id ? cover_ : QImage(),
        !paused_.load() && !tracks_.empty() && errorText_.isEmpty(),
        muted_.load() ? 0 : volumePercent_.load(),
        previousAvailable(),
        nextAvailable());
}

void MusicWindow::showTrayCard() {
    if (trayCard_ == nullptr) {
        return;
    }
    updateTray();
    trayCard_->adjustSize();
    if (!popupPlaced_) {
        QSettings settings;
        const bool saved = settings.contains(QStringLiteral("player/popupX"))
            && settings.contains(QStringLiteral("player/popupY"));
        int x = settings.value(QStringLiteral("player/popupX")).toInt();
        int y = settings.value(QStringLiteral("player/popupY")).toInt();
        QScreen* screen = QGuiApplication::primaryScreen();
        const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
        if (!saved) {
            x = area.right() - trayCard_->width() - 16;
            y = area.bottom() - trayCard_->height() - 48;
        }
        const QRect card(x, y, trayCard_->width(), trayCard_->height());
        bool visible = false;
        for (QScreen* candidate : QGuiApplication::screens()) {
            if (candidate->availableGeometry().intersects(card)) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            x = area.right() - trayCard_->width() - 16;
            y = area.bottom() - trayCard_->height() - 48;
        }
        trayCard_->move(x, y);
        popupPlaced_ = true;
    }
    trayCard_->show();
    trayCard_->raise();
}

void MusicWindow::closeEvent(QCloseEvent* event) {
    hidePopups();
    if (trayCard_ != nullptr) {
        trayCard_->hide();
    }
    if (tray_ != nullptr) {
        tray_->hide();
    }
    qApp->removeEventFilter(this);
    stopWorker();
    if (output_ != nullptr) {
        output_->stop();
    }
    QWidget::closeEvent(event);
}

void MusicWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
        togglePlay();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Left) {
        seekBy(-5000);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        seekBy(5000);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace videovault::app
