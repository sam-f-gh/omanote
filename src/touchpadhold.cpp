#include "touchpadhold.h"

#include <QGuiApplication>
#include <cstring>
#include <wayland-client.h>

#include "pointer-gestures-unstable-v1-client-protocol.h"

TouchpadHold *TouchpadHold::instance() {
    static TouchpadHold *hold = new TouchpadHold(qApp);
    return hold;
}

TouchpadHold::TouchpadHold(QObject *parent) : QObject(parent) {
    auto *wayland = qApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->display())
        return;
    // Proxies on the default queue: Qt dispatches it on the GUI thread, so
    // the callbacks below run there too.
    static const wl_registry_listener listener = {&TouchpadHold::global, &TouchpadHold::globalRemove};
    m_registry = wl_display_get_registry(wayland->display());
    wl_registry_add_listener(m_registry, &listener, this);
    wl_display_flush(wayland->display());
}

TouchpadHold::~TouchpadHold() {
    if (m_hold)
        zwp_pointer_gesture_hold_v1_destroy(m_hold);
    if (m_gestures)
        zwp_pointer_gestures_v1_release(m_gestures);
    if (m_registry)
        wl_registry_destroy(m_registry);
}

void TouchpadHold::global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto *self = static_cast<TouchpadHold *>(data);
    // Hold arrived in version 3 of the protocol.
    if (!self->m_gestures && std::strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0 && version >= 3) {
        self->m_gestures = static_cast<zwp_pointer_gestures_v1 *>(
            wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 3));
        self->ensure();
    }
}

void TouchpadHold::globalRemove(void *, wl_registry *, uint32_t) {}

void TouchpadHold::ensure() {
    if (m_hold || !m_gestures)
        return;
    auto *wayland = qApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_pointer *pointer = wayland ? wayland->pointer() : nullptr;
    if (!pointer)
        return;
    static const zwp_pointer_gesture_hold_v1_listener listener = {&TouchpadHold::holdBegin, &TouchpadHold::holdEnd};
    m_hold = zwp_pointer_gestures_v1_get_hold_gesture(m_gestures, pointer);
    zwp_pointer_gesture_hold_v1_add_listener(m_hold, &listener, this);
    wl_display_flush(wayland->display());
}

void TouchpadHold::holdBegin(void *data, zwp_pointer_gesture_hold_v1 *, uint32_t, uint32_t, wl_surface *, uint32_t fingers) {
    emit static_cast<TouchpadHold *>(data)->fingersDown(int(fingers));
}

void TouchpadHold::holdEnd(void *, zwp_pointer_gesture_hold_v1 *, uint32_t, uint32_t, int32_t) {}
