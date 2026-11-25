#pragma once

#include <QVariantMap>

#include "filepicker.h"

// Open and Save As through org.freedesktop.portal.FileChooser, so dialogs
// match the desktop and respect sandboxing. Pattern from Omacut (MIT).
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openPdf() override;
    void savePdf(const QString &suggestedPath) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action { None, Open, Save };
    void request(const QString &method, const QString &title, QVariantMap options, Action action);

    QString m_requestPath;
    Action m_action = Action::None;
};
