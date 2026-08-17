#ifndef SEGMENTPUZZLER_APPLICATIONQUITEVENTFILTER_H
#define SEGMENTPUZZLER_APPLICATIONQUITEVENTFILTER_H

#include <QEvent>
#include <QObject>

#include <functional>
#include <utility>

class ApplicationQuitEventFilter final : public QObject {
public:
    using QuitRequestHandler = std::function<bool()>;

    explicit ApplicationQuitEventFilter(QuitRequestHandler handleQuitRequest,
                                        QObject *parent = nullptr)
        : QObject(parent),
          handleQuitRequest_(std::move(handleQuitRequest)) {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        Q_UNUSED(watched);
        if (event->type() != QEvent::Quit || handleQuitRequest_()) {
            return false;
        }

        event->ignore();
        return true;
    }

private:
    QuitRequestHandler handleQuitRequest_;
};

#endif // SEGMENTPUZZLER_APPLICATIONQUITEVENTFILTER_H
