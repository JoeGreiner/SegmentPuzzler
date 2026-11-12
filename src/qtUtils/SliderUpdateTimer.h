#pragma once

#include <QTimer>
#include <functional>
#include <utility>

// Render the latest slider state at most once per interval, never restart the
// timer on movement. Release/close flushes a pending final value immediately.
class SliderUpdateTimer : public QObject {
public:
    SliderUpdateTimer(QObject *parent, std::function<void()> update)
        : QObject(parent), update_(std::move(update)) {
        timer_.setSingleShot(true);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] { update_(); });
    }

    void request(bool dragging) {
        if (!dragging) {
            timer_.stop();
            update_();
        } else if (!timer_.isActive()) {
            timer_.start();
        }
    }

    void flush() {
        if (timer_.isActive()) {
            timer_.stop();
            update_();
        }
    }

private:
    QTimer timer_;
    std::function<void()> update_;
};
