#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace swiftify {

class SoloistCore;

namespace soloist {
/// soloist from SWIFTIFY_SOLOIST, the PATH or ~/.local/bin; empty if none
QString findProgram();
/// where the key is kept: ~/.config/soloist/api-key
QString apiKeyPath();
/// the key from SOLOIST_API_KEY, or else from apiKeyPath(); empty if none
QString readApiKey();
/// writes `key` to apiKeyPath(), readable by its owner alone. false, with
/// `error` set, if it is not a key or could not be written
bool saveApiKey(const QString &key, QString *error = nullptr);
}

/// what Settings → Account says of spotify soloist: whether swiftify found
/// it and a key for it, and how the soloist it started is doing. the key is
/// kept in the file detection reads, beside soloist's own config, so setting
/// it up here or by hand as the readme says comes to the same. playback picks
/// its core once, as swiftify starts, so a soloist set up here plays from the
/// next start on; what is found is looked for again on refresh().
class SoloistSetup : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Soloist)
    QML_SINGLETON

    /// this build can play through soloist (linux)
    Q_PROPERTY(bool supported READ supported CONSTANT FINAL)
    Q_PROPERTY(State state READ state NOTIFY stateChanged FINAL)
    /// where soloist was found, if it was
    Q_PROPERTY(QString program READ program NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool hasKey READ hasKey NOTIFY stateChanged FINAL)
    /// SOLOIST_API_KEY is set, and wins over the saved key
    Q_PROPERTY(bool keyFromEnvironment READ keyFromEnvironment NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString keyPath READ keyPath CONSTANT FINAL)
    /// why soloist stopped or could not start, in soloist's own words
    Q_PROPERTY(QString failure READ failure NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString deviceName READ deviceName CONSTANT FINAL)

public:
    enum class State {
        /// not built with soloist
        Unsupported,
        /// SWIFTIFY_PLAYBACK=librespot asks for librespot alone
        Disabled,
        NotFound,
        NoKey,
        /// found with a key, but swiftify started without it
        RestartNeeded,
        Starting,
        /// running, and waiting to be picked in a spotify app's devices
        Unpaired,
        Active,
        /// a build past its 90 days
        Expired,
        Failed,
    };
    Q_ENUM(State)

    /// no default argument: qml builds a singleton with a default constructor
    /// in preference to create(), and would end up with an instance of its own
    explicit SoloistSetup(QObject *parent);
    ~SoloistSetup() override;

    static SoloistSetup *create(QQmlEngine *, QJSEngine *);

    /// the soloist swiftify plays through, whose state is then what is shown
    void attach(SoloistCore *core);

    bool supported() const;
    State state() const;
    QString program() const { return m_program; }
    bool hasKey() const { return m_hasKey; }
    bool keyFromEnvironment() const;
    QString keyPath() const;
    QString failure() const;
    QString deviceName() const;

    /// looks again for soloist and its key
    Q_INVOKABLE void refresh();
    /// saves the key and looks again; an empty string if it was saved, or
    /// else why not
    Q_INVOKABLE QString saveApiKey(const QString &key);
    /// whether a playback error is the one soloist would get past, and
    /// soloist is not what plays: librespot refused the song
    Q_INVOKABLE bool suggestedFor(const QString &error) const;

signals:
    void stateChanged();

private:
    QPointer<SoloistCore> m_core;
    /// what the settings were last told of the core, so they hear of changes
    State m_shownState = State::Unsupported;
    QString m_shownFailure;
    QString m_program;
    bool m_hasKey = false;
};

} // namespace swiftify
