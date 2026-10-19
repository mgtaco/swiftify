#include "app/SoloistSetup.h"

#ifdef SWIFTIFY_HAS_SOLOIST
#include "soloist/SoloistCore.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlEngine>
#include <QStandardPaths>

namespace swiftify {

namespace {
SoloistSetup *s_instance = nullptr;

bool librespotAsked()
{
    return qEnvironmentVariable("SWIFTIFY_PLAYBACK") == QLatin1StringView("librespot");
}
}

namespace soloist {

QString findProgram()
{
    QString program = qEnvironmentVariable("SWIFTIFY_SOLOIST");
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable(QStringLiteral("soloist"));
    }
    if (program.isEmpty()) {
        // a launcher's PATH often leaves out ~/.local/bin, where soloist's
        // guide installs it without sudo
        program = QStandardPaths::findExecutable(QStringLiteral("soloist"),
                                                 {QDir::homePath() + QStringLiteral("/.local/bin")});
    }
    return program;
}

QString apiKeyPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/soloist/api-key");
}

QString readApiKey()
{
    QString key = qEnvironmentVariable("SOLOIST_API_KEY").trimmed();
    if (key.isEmpty()) {
        QFile file(apiKeyPath());
        if (file.open(QIODevice::ReadOnly)) {
            key = QString::fromUtf8(file.readAll()).trimmed();
        }
    }
    return key;
}

bool saveApiKey(const QString &raw, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    const QString key = raw.trimmed();
    if (key.isEmpty()) {
        return fail(SoloistSetup::tr("Paste your Soloist API key first."));
    }
    // a key is one word: anything else is a paste that caught more
    for (const QChar character : key) {
        if (character.isSpace()) {
            return fail(SoloistSetup::tr("That doesn't look like a Soloist API key, it has spaces in it."));
        }
    }
    const QString path = apiKeyPath();
    if (!QDir().mkpath(QFileInfo(path).path())) {
        return fail(SoloistSetup::tr("Could not create %1.").arg(QFileInfo(path).path()));
    }
    constexpr auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QFile file(path);
    // made readable by its owner alone, and an older one made so before the
    // new key goes in
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate, ownerOnly) || !file.setPermissions(ownerOnly)) {
        return fail(SoloistSetup::tr("Could not write %1: %2").arg(path, file.errorString()));
    }
    if (file.write(key.toUtf8() + '\n') < 0 || !file.flush()) {
        return fail(SoloistSetup::tr("Could not write %1: %2").arg(path, file.errorString()));
    }
    return true;
}

} // namespace soloist

SoloistSetup::SoloistSetup(QObject *parent)
    : QObject(parent)
{
    if (!s_instance) {
        s_instance = this;
    }
    refresh();
}

SoloistSetup::~SoloistSetup()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

SoloistSetup *SoloistSetup::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void SoloistSetup::attach(SoloistCore *core)
{
#ifdef SWIFTIFY_HAS_SOLOIST
    if (m_core) {
        m_core->disconnect(this);
    }
    m_core = core;
    if (m_core) {
        // soloist says something with every message; the settings hear of
        // what changes what they show
        connect(m_core, &SoloistCore::stateChanged, this, [this] {
            if (state() != m_shownState || failure() != m_shownFailure) {
                m_shownState = state();
                m_shownFailure = failure();
                emit stateChanged();
            }
        });
    }
    m_shownState = state();
    m_shownFailure = failure();
    emit stateChanged();
#else
    Q_UNUSED(core)
#endif
}

bool SoloistSetup::supported() const
{
#ifdef SWIFTIFY_HAS_SOLOIST
    return true;
#else
    return false;
#endif
}

SoloistSetup::State SoloistSetup::state() const
{
    if (!supported()) {
        return State::Unsupported;
    }
#ifdef SWIFTIFY_HAS_SOLOIST
    if (m_core) {
        switch (m_core->health()) {
        case SoloistCore::Health::Starting: return State::Starting;
        case SoloistCore::Health::Unpaired: return State::Unpaired;
        case SoloistCore::Health::Ready: return State::Active;
        case SoloistCore::Health::Expired: return State::Expired;
        case SoloistCore::Health::Failed: return State::Failed;
        }
    }
#endif
    if (librespotAsked()) {
        return State::Disabled;
    }
    if (m_program.isEmpty()) {
        return State::NotFound;
    }
    return m_hasKey ? State::RestartNeeded : State::NoKey;
}

bool SoloistSetup::keyFromEnvironment() const
{
    return !qEnvironmentVariable("SOLOIST_API_KEY").trimmed().isEmpty();
}

QString SoloistSetup::keyPath() const
{
    return soloist::apiKeyPath();
}

QString SoloistSetup::failure() const
{
#ifdef SWIFTIFY_HAS_SOLOIST
    if (m_core) {
        return m_core->failure();
    }
#endif
    return {};
}

QString SoloistSetup::deviceName() const
{
    return QStringLiteral("Swiftify");
}

void SoloistSetup::refresh()
{
    const QString program = supported() ? soloist::findProgram() : QString();
    // whether there is a key, not what it is: the settings never hold it
    const bool hasKey = supported() && (keyFromEnvironment() || QFileInfo(soloist::apiKeyPath()).size() > 0);
    if (program != m_program || hasKey != m_hasKey) {
        m_program = program;
        m_hasKey = hasKey;
        emit stateChanged();
    }
}

QString SoloistSetup::saveApiKey(const QString &key)
{
    QString error;
    if (!soloist::saveApiKey(key, &error)) {
        return error;
    }
    refresh();
    return {};
}

bool SoloistSetup::suggestedFor(const QString &error) const
{
    // with soloist playing, a song it can't load says so in its own words
    return supported() && !m_core && !librespotAsked()
        && error.contains(QLatin1StringView("could not load this track"));
}

} // namespace swiftify
