#pragma once

#include <QObject>

struct wl_registry;
struct zwp_pointer_gestures_v1;
struct zwp_pointer_gesture_hold_v1;

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
    // The pointer appears with the first input; call when input arrives.
    void ensure();

signals:
    void fingersDown(int fingers);

private:
    explicit TouchpadHold(QObject *parent);
    ~TouchpadHold() override;

    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    static void globalRemove(void *data, wl_registry *registry, uint32_t name);
    static void holdBegin(void *data, zwp_pointer_gesture_hold_v1 *hold, uint32_t serial, uint32_t time,
                          struct wl_surface *surface, uint32_t fingers);
    static void holdEnd(void *data, zwp_pointer_gesture_hold_v1 *hold, uint32_t serial, uint32_t time,
                        int32_t cancelled);

    wl_registry *m_registry = nullptr;
    zwp_pointer_gestures_v1 *m_gestures = nullptr;
    zwp_pointer_gesture_hold_v1 *m_hold = nullptr;
};
