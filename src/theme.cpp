// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

namespace {
// Omarchy's absence still gets a calm dark chrome with the house yellow.
const QHash<QString, QString> fallback{
    {"accent", "#FFD60A"}, {"background", "#141417"}, {"foreground", "#e4e4e7"}};

QString mix(const QString &a, const QString &b, double t) {
    const QColor x(a), y(b);
    return QColor::fromRgbF(x.redF() + (y.redF() - x.redF()) * t, x.greenF() + (y.greenF() - x.greenF()) * t,
                            x.blueF() + (y.blueF() - x.blueF()) * t).name();
}
}

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_colors(fallback) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!hyprctl.isEmpty()) {
        m_roundingPoll.start(); // Tests inject a fake hyprctl and poll it.
    } else if (!m_hyprctl.isEmpty()) {
        // Rounding only changes when Hyprland reloads its config, which it
        // announces on its event socket: no polling, no idle wakeups.
        connect(&m_events, &QLocalSocket::readyRead, this, [this] {
            while (m_events.canReadLine())
                if (m_events.readLine().startsWith("configreloaded"))
                    refreshRounding();
        });
        connect(&m_events, &QLocalSocket::disconnected, this, [this] { m_reconnect.start(); });
        connect(&m_events, &QLocalSocket::errorOccurred, this, [this] { m_reconnect.start(); });
        m_reconnect.setSingleShot(true);
        m_reconnect.setInterval(5000);
        connect(&m_reconnect, &QTimer::timeout, this, &Theme::connectEvents);
        connectEvents();
    }
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::connectEvents() {
    const QString socket = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/hypr/" +
                           qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE") + "/.socket2.sock";
    m_events.abort();
    m_events.connectToServer(socket, QIODevice::ReadOnly);
    refreshRounding(); // Whatever changed while we weren't listening.
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QHash<QString, QString> Theme::readColors(const QString &path) {
    QHash<QString, QString> colors;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return colors;
    static const QRegularExpression line(R"re(^\s*([a-z_0-9]+)\s*=\s*["']([^"']*)["'])re",
                                         QRegularExpression::MultilineOption);
    auto it = line.globalMatch(QString::fromUtf8(file.readAll()));
    while (it.hasNext()) {
        const auto m = it.next();
        const QString value = m.captured(2);
        if (m.captured(1) == "mode" || QColor::isValidColorName(value))
            colors.insert(m.captured(1), value);
    }
    return colors;
}

double Theme::luminance(const QString &color) {
    const QColor c(color);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

QString Theme::color(const QString &key) const {
    return m_colors.value(key, fallback.value(key));
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const { return luminance(accent()) > .179 ? "black" : "white"; }

bool Theme::dark() const {
    const QString mode = m_colors.value("mode");
    if (mode == "light" || mode == "dark")
        return mode == "dark";
    return luminance(background()) < .4;
}

// Panels sit one step off the background, toward the foreground.
QString Theme::surface() const {
    if (m_colors.contains("lighter_background") && dark())
        return m_colors.value("lighter_background");
    return mix(background(), foreground(), dark() ? .07 : .05);
}

// The workspace behind the white pages: a touch deeper than the background,
// so paper still stands out on light themes.
QString Theme::backdrop() const {
    return dark() ? mix(background(), "#000000", .25) : mix(background(), foreground(), .10);
}

QString Theme::muted() const {
    return m_colors.value("muted", mix(foreground(), background(), .45));
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    auto colors = fallback;
    const auto read = readColors(m_directory + "/theme/colors.toml");
    for (auto it = read.begin(); it != read.end(); ++it)
        colors.insert(it.key(), it.value());
    if (colors != m_colors) {
        m_colors = colors;
        emit changed();
    }
}
