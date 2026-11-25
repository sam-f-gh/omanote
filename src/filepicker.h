#pragma once

#include <QObject>
#include <QString>

// Abstract so tests can inject a fake picker instead of talking to D-Bus.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    ~FilePicker() override = default;

    virtual void openPdf() = 0;
    virtual void savePdf(const QString &suggestedPath) = 0;

signals:
    void opened(const QString &path);
    void saved(const QString &path);
    void cancelled();
    void failed(const QString &message);
};
