#include "touchpadhold.h"

#include <QGuiApplication>
#include <QWaylandClientExtensionTemplate>

#include "qwayland-pointer-gestures-unstable-v1.h"

class PointerGestures : public QWaylandClientExtensionTemplate<PointerGestures, &QtWayland::zwp_pointer_gestures_v1::release>,
                        public QtWayland::zwp_pointer_gestures_v1 {
public:
    PointerGestures() : QWaylandClientExtensionTemplate(3) {}
};

class HoldGesture : public QtWayland::zwp_pointer_gesture_hold_v1 {
public:
    HoldGesture(::zwp_pointer_gesture_hold_v1 *object, TouchpadHold *owner)
        : QtWayland::zwp_pointer_gesture_hold_v1(object), m_owner(owner) {}
    ~HoldGesture() override { destroy(); }

protected:
    void zwp_pointer_gesture_hold_v1_begin(uint32_t, uint32_t, wl_surface *, uint32_t) override {
        emit m_owner->fingersDown();
    }

private:
    TouchpadHold *m_owner;
};

TouchpadHold *TouchpadHold::instance() {
    static TouchpadHold *hold = new TouchpadHold(qApp);
    return hold;
}

TouchpadHold::TouchpadHold(QObject *parent) : QObject(parent) {
    if (qApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        m_gestures = std::make_unique<PointerGestures>();
}

TouchpadHold::~TouchpadHold() = default;

void TouchpadHold::ensure() {
    // Hold arrived in version 3 of the protocol.
    if (m_hold || !m_gestures || !m_gestures->isActive() || m_gestures->QtWayland::zwp_pointer_gestures_v1::version() < 3)
        return;
    wl_pointer *pointer = qApp->nativeInterface<QNativeInterface::QWaylandApplication>()->pointer();
    if (pointer)
        m_hold = std::make_unique<HoldGesture>(m_gestures->get_hold_gesture(pointer), this);
}
