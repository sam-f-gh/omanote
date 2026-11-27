#pragma once

#include <QObject>
#include <memory>

class PointerGestures;
class HoldGesture;

// Fingers resting on the touchpad, from the Wayland pointer-gestures "hold"
// gesture (libinput sends it the moment fingers land, before any motion).
// Qt binds only swipe and pinch, so this binds hold itself on Qt's own
// Wayland connection. It's what lets a touch on the pad stop a fling, like
// putting a finger on a spinning wheel. Silent elsewhere (X11, older
// compositors): the fling then stops on the next scroll instead.
class TouchpadHold : public QObject {
    Q_OBJECT

public:
    static TouchpadHold *instance();
    // Qt creates the pointer with the first input; call when touchpad input
    // arrives. Free once bound.
    void ensure();

signals:
    void fingersDown();

private:
    explicit TouchpadHold(QObject *parent);
    ~TouchpadHold() override;

    std::unique_ptr<PointerGestures> m_gestures;
    std::unique_ptr<HoldGesture> m_hold;
};
