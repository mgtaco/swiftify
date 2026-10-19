#include "FakeCore.h"
#include "app/SoloistSetup.h"
#include "soloist/SoloistCore.h"

#include <QCoroTask>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

#include <memory>
#include <optional>

#include <sys/stat.h>

using namespace swiftify;
using namespace swiftify::testing;

namespace {

/// soloist's websocket, as far as the core uses it. it answers play as
/// soloist would, unless told to stay quiet.
class FakeSoloist : public QObject {
    Q_OBJECT

public:
    FakeSoloist()
        : m_server(QStringLiteral("fake soloist"), QWebSocketServer::NonSecureMode)
    {
        QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            m_client = m_server.nextPendingConnection();
            ++connections;
            connect(m_client, &QWebSocket::textMessageReceived, this, &FakeSoloist::received);
            send({{QStringLiteral("type"), QStringLiteral("auth_state")},
                  {QStringLiteral("logged_in"), loggedIn},
                  {QStringLiteral("is_active"), false},
                  {QStringLiteral("device_name"), QStringLiteral("Swiftify")}});
            // left repeating a song, as another app or an older swiftify can
            send({{QStringLiteral("type"), QStringLiteral("playback_state")},
                  {QStringLiteral("status"), QStringLiteral("idle")},
                  {QStringLiteral("options"), QJsonObject{{QStringLiteral("repeat"), QStringLiteral("track")}}}});
        });
    }

    quint16 port() const { return m_server.serverPort(); }

    /// closes the socket, as soloist does if its websocket falls over
    void dropClient() { m_client->close(); }

    void send(const QJsonObject &message)
    {
        m_client->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

    void sendStatus(const QString &status)
    {
        send({{QStringLiteral("type"), QStringLiteral("playback_changed")}, {QStringLiteral("status"), status}});
    }

    void sendTrack(const QString &uri, qint64 durationMs, const QString &name = QStringLiteral("Song"))
    {
        const auto entity = [](const QString &entityUri, const QString &entityName) {
            return QJsonObject{{QStringLiteral("entity"), QJsonObject{
                {QStringLiteral("uri"), entityUri},
                {QStringLiteral("decorations"), QJsonObject{
                    {QStringLiteral("identity"), QJsonObject{{QStringLiteral("name"), entityName}}}}}}}};
        };
        send({{QStringLiteral("type"), QStringLiteral("track_changed")},
              {QStringLiteral("item"), QJsonObject{
                  {QStringLiteral("uri"), uri},
                  {QStringLiteral("decorations"), QJsonObject{
                      {QStringLiteral("identity"), QJsonObject{{QStringLiteral("name"), name}}},
                      {QStringLiteral("creators"), QJsonArray{entity(QStringLiteral("spotify:artist:art"), QStringLiteral("Artist"))}},
                      {QStringLiteral("parent"), entity(QStringLiteral("spotify:album:alb"), QStringLiteral("Album"))},
                      {QStringLiteral("visual_identity"), QJsonObject{{QStringLiteral("cover"), QJsonArray{
                          QJsonObject{{QStringLiteral("url"), QStringLiteral("https://i.scdn.co/image/large")},
                                      {QStringLiteral("size"), QStringLiteral("large")}}}}}},
                      {QStringLiteral("playback"), QJsonObject{{QStringLiteral("duration_ms"), durationMs}}}}}}}});
    }

    void sendPosition(qint64 positionMs, double speed = 1.0)
    {
        send({{QStringLiteral("type"), QStringLiteral("position_sync")},
              {QStringLiteral("position"), QJsonObject{{QStringLiteral("position_ms"), positionMs},
                                                       {QStringLiteral("timestamp_ms"), QDateTime::currentMSecsSinceEpoch()},
                                                       {QStringLiteral("speed"), speed}}}});
    }

    QStringList commandNames() const
    {
        QStringList names;
        for (const QJsonObject &command : commands) {
            names << command.value(QStringLiteral("command")).toString();
        }
        return names;
    }

    bool loggedIn = true;
    /// answers play with the song starting
    bool answerPlay = true;
    /// answers play with this error instead
    QString playError;
    /// plays to leave unanswered, as a soloist stuck loading would
    int ignorePlays = 0;
    qint64 durationMs = 200'000;
    QList<QJsonObject> commands;
    int connections = 0;

private:
    void received(const QString &text)
    {
        const QJsonObject command = QJsonDocument::fromJson(text.toUtf8()).object();
        commands << command;
        if (command.value(QStringLiteral("command")) != QLatin1StringView("play")) {
            return;
        }
        if (!playError.isEmpty()) {
            send({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("message"), playError}});
            return;
        }
        const QString uri = command.value(QStringLiteral("uri")).toString();
        if (!answerPlay || uri.isEmpty()) {
            return;
        }
        if (ignorePlays > 0) {
            --ignorePlays;
            return;
        }
        send({{QStringLiteral("type"), QStringLiteral("track_changed")},
              {QStringLiteral("item"), QJsonObject{
                  {QStringLiteral("uri"), uri},
                  {QStringLiteral("entity_type"), QStringLiteral("track")},
                  // as soloist 1.3.9 sends it
                  {QStringLiteral("decorations"), QJsonObject{
                      {QStringLiteral("identity"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Song")}}},
                      {QStringLiteral("playback"), QJsonObject{{QStringLiteral("duration_ms"), durationMs}}}}}}}});
        send({{QStringLiteral("type"), QStringLiteral("playback_changed")}, {QStringLiteral("status"), QStringLiteral("playing")}});
        sendPosition(0);
    }

    QWebSocketServer m_server;
    QWebSocket *m_client = nullptr;
};

} // namespace

class TestSoloist : public QObject {
    Q_OBJECT

    /// waits for the end, taking it once: QTRY_VERIFY reads its condition again
    /// after it held, and taking the end consumes it
    bool waitForEnd(int timeoutMs = 5000)
    {
        bool ended = false;
        QElapsedTimer waited;
        waited.start();
        while (!(ended = m_core->takeEndOfTrack()) && waited.elapsed() < timeoutMs) {
            QTest::qWait(20);
        }
        return ended;
    }

    /// the variables detection reads, as they were before a test
    const QList<QByteArray> m_variables{"XDG_CONFIG_HOME", "SOLOIST_API_KEY", "SWIFTIFY_SOLOIST", "SWIFTIFY_PLAYBACK", "PATH", "HOME"};
    QHash<QByteArray, std::optional<QByteArray>> m_environment;

    /// soloist found at `program`, or nowhere if empty
    void findSoloistAt(const QString &program)
    {
        qputenv("SWIFTIFY_SOLOIST", program.toUtf8());
        if (program.isEmpty()) {
            // nothing on the PATH or in ~/.local/bin either
            qputenv("PATH", m_dir->filePath(QStringLiteral("empty")).toUtf8());
            qputenv("HOME", m_dir->filePath(QStringLiteral("home")).toUtf8());
        }
    }

    static mode_t modeOf(const QString &path)
    {
        struct stat info {};
        return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? info.st_mode & 07777 : 0;
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        for (const QByteArray &name : m_variables) {
            m_environment[name] = qEnvironmentVariableIsSet(name.constData()) ? std::optional(qgetenv(name.constData())) : std::nullopt;
        }
        // keys are saved and looked for here, never in the real ~/.config
        qputenv("XDG_CONFIG_HOME", m_dir->filePath(QStringLiteral("config")).toUtf8());
        qunsetenv("SOLOIST_API_KEY");
        qunsetenv("SWIFTIFY_PLAYBACK");
        // stands in for the soloist binary: writes the port the fake listens
        // on where soloist would, and waits to be stopped
        m_program = m_dir->filePath(QStringLiteral("soloist"));
        QFile script(m_program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n"
                     "while [ $# -gt 0 ]; do case \"$1\" in --data-dir) d=\"$2\"; shift;; esac; shift; done\n"
                     "printf %s \"$FAKE_SOLOIST_PORT\" > \"$d/ws.port\"\n"
                     "exec sleep 600\n");
        script.close();
        script.setPermissions(script.permissions() | QFile::ExeOwner);
        m_soloist = std::make_unique<FakeSoloist>();
        qputenv("FAKE_SOLOIST_PORT", QByteArray::number(m_soloist->port()));
        // no sink in the real sound server for a test
        qputenv("SWIFTIFY_SOLOIST_ROUTE", "off");
        m_fallback = std::make_unique<FakeCore>();
        m_core = std::make_unique<SoloistCore>(
            m_fallback.get(), SoloistConfig{m_program, QStringLiteral("key"), m_dir->filePath(QStringLiteral("data"))});
    }

    void cleanup()
    {
        m_core.reset();
        m_fallback.reset();
        m_soloist.reset();
        for (const QByteArray &name : m_variables) {
            if (const auto value = m_environment.value(name)) {
                qputenv(name.constData(), *value);
            } else {
                qunsetenv(name.constData());
            }
        }
        m_dir.reset();
    }

    void playsASongThroughSoloist()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        const QStringList names = m_soloist->commandNames();
        const qsizetype play = names.indexOf(QStringLiteral("play"));
        QVERIFY(names.indexOf(QStringLiteral("activate")) >= 0 && names.indexOf(QStringLiteral("activate")) < play);
        QCOMPARE(names.at(play - 1), QStringLiteral("set_volume"));
        QCOMPARE(m_soloist->commands.at(play).value(QStringLiteral("uri")).toString(), QStringLiteral("spotify:track:a"));
        // swiftify's queue decides what follows, so soloist's repeat goes off
        const qsizetype repeat = m_soloist->commandNames().indexOf(QStringLiteral("set_repeat_track"));
        QVERIFY(repeat >= 0);
        QCOMPARE(m_soloist->commands.at(repeat).value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(m_core->playbackDurationMs(), 200'000u);
        QVERIFY(m_core->playbackPositionMs() < 1000);
        QVERIFY(!m_core->takeEndOfTrack());
        // nothing reached librespot
        QVERIFY(m_fallback->pending.isEmpty());
    }

    void anUnpairedSoloistSaysHowToPair()
    {
        m_soloist->loggedIn = false;
        try {
            QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
            QFAIL("played while unpaired");
        } catch (const SpotifyError &error) {
            QCOMPARE(error.kind(), SpotifyError::Kind::Session);
            QVERIFY(error.message().contains(QStringLiteral("Swiftify")));
        }
        QVERIFY(!m_soloist->commandNames().contains(QStringLiteral("play")));
    }

    void soloistsErrorFailsThePlay()
    {
        m_soloist->playError = QStringLiteral("track unavailable");
        try {
            QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
            QFAIL("played despite the error");
        } catch (const SpotifyError &error) {
            QCOMPARE(error.kind(), SpotifyError::Kind::Core);
            QVERIFY(error.message().contains(QStringLiteral("track unavailable")));
        }
    }

    void theEndOfASongIsReportedOnce()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendPosition(199'900);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 199'800);
        QVERIFY(waitForEnd());
        QVERIFY(!m_core->takeEndOfTrack());
    }

    void theLastSecondIsNotCutOff()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendPosition(199'000);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 199'000);
        QVERIFY(!m_core->takeEndOfTrack());
        QTest::qWait(500);
        QVERIFY(!m_core->takeEndOfTrack());
        QVERIFY(waitForEnd(2000));
    }

    void theClockStoppedToBufferAtTheEndWaitsForTheRest()
    {
        // what soloist 1.3.9 does about 0.8s from the end: the clock stops
        // while what follows loads, and the song plays on to its end
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendStatus(QStringLiteral("buffering"));
        m_soloist->sendPosition(199'230, 0.0);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 199'230);
        QVERIFY(!m_core->takeEndOfTrack());
        // the time shown runs on to the end meanwhile
        QTRY_VERIFY(m_core->playbackPositionMs() >= 199'700);
        QVERIFY(waitForEnd(2000));
    }

    void autoplayTakingOverAtTheEndIsTheEnd()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendStatus(QStringLiteral("buffering"));
        m_soloist->sendPosition(199'230, 0.0);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 199'230);
        m_soloist->sendTrack(QStringLiteral("spotify:track:autoplay"), 180'000);
        QVERIFY(waitForEnd());
    }

    void aSongPlayedFromElsewhereMidwayIsNotAnEnd()
    {
        // picked in the spotify app on a phone, halfway through
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendPosition(60'000);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 60'000);
        m_soloist->sendTrack(QStringLiteral("spotify:track:other"), 180'000);
        QTest::qWait(200);
        QVERIFY(!m_core->takeEndOfTrack());
    }

    void aSongPickedElsewhereIsFollowed()
    {
        QSignalSpy started(m_core.get(), &PlaybackCore::remoteTrackStarted);
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        // swiftify's own play is nobody else's doing
        QCOMPARE(started.count(), 0);
        m_soloist->sendPosition(60'000);
        m_soloist->sendTrack(QStringLiteral("spotify:track:picked"), 180'000, QStringLiteral("Picked"));
        m_soloist->sendPosition(0);
        QTRY_COMPARE(started.count(), 1);
        const Song song = started.first().first().value<Song>();
        QCOMPARE(song.uri, QStringLiteral("spotify:track:picked"));
        QCOMPARE(song.name, QStringLiteral("Picked"));
        QCOMPARE(song.artists, QStringLiteral("Artist"));
        QCOMPARE(song.albumName, QStringLiteral("Album"));
        QCOMPARE(song.albumId, QStringLiteral("alb"));
        QCOMPARE(song.artworkUrl, QUrl(QStringLiteral("https://i.scdn.co/image/large")));
        QCOMPARE(song.durationMs, 180'000);
        QCOMPARE(m_core->playbackDurationMs(), 180'000u);
        // its end is soloist's business: it goes on by its own queue
        m_soloist->sendPosition(179'990);
        QTest::qWait(150);
        QVERIFY(!m_core->takeEndOfTrack());
        // and the skips go to soloist
        QVERIFY(m_core->skipRemote(true));
        QTRY_VERIFY(m_soloist->commandNames().contains(QStringLiteral("skip_next")));
        // until swiftify plays something itself
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:b")));
        QVERIFY(!m_core->skipRemote(true));
    }

    void aPauseElsewhereIsReported()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        QSignalSpy playing(m_core.get(), &PlaybackCore::remotePlayingChanged);
        m_soloist->sendStatus(QStringLiteral("paused"));
        QTRY_COMPARE(playing.count(), 1);
        QCOMPARE(playing.last().first().toBool(), false);
        m_soloist->sendStatus(QStringLiteral("playing"));
        QTRY_COMPARE(playing.count(), 2);
        QCOMPARE(playing.last().first().toBool(), true);
    }

    void aSongLeftLoadingIsAskedForAgain()
    {
        m_soloist->ignorePlays = 1;
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        QCOMPARE(m_soloist->commandNames().count(QStringLiteral("play")), 2);
    }

    void aSongStartedOverAfterBufferingHasEnded()
    {
        // the tick missed the stopped clock, and the song started over
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendPosition(198'500, 0.0);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 198'000);
        m_soloist->sendPosition(1);
        QTRY_COMPARE(m_core->playbackPositionMs(), 200'000u);
        QVERIFY(m_core->takeEndOfTrack());
    }

    void aSongStartedOverByRepeatHasEnded()
    {
        // the tick missed the last moments, and soloist already started over
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        m_soloist->sendPosition(199'800);
        QTRY_VERIFY(m_core->playbackPositionMs() >= 199'700);
        m_soloist->sendPosition(0);
        QTRY_COMPARE(m_core->playbackPositionMs(), 200'000u);
        QVERIFY(m_core->takeEndOfTrack());
    }

    void aNewerPlaySupersedesAnOlderOne()
    {
        m_soloist->answerPlay = false;
        auto first = m_core->playTrack(QStringLiteral("spotify:track:a"));
        QTRY_VERIFY(m_soloist->commandNames().contains(QStringLiteral("play")));
        m_soloist->answerPlay = true;
        auto second = m_core->playTrack(QStringLiteral("spotify:track:b"));
        QCoro::waitFor(second);
        try {
            QCoro::waitFor(first);
            QFAIL("the older play finished");
        } catch (const SpotifyError &error) {
            QCOMPARE(error.kind(), SpotifyError::Kind::Superseded);
        }
    }

    void aDroppedSocketIsOpenedAgain()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        QCOMPARE(m_soloist->connections, 1);
        m_soloist->dropClient();
        QTRY_COMPARE(m_soloist->connections, 2);
        m_soloist->commands.clear();
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:b")));
        QVERIFY(m_soloist->commandNames().contains(QStringLiteral("play")));
    }

    void localFilesPlayThroughTheRustCore()
    {
        QCoro::waitFor(m_core->playTrack(QStringLiteral("spotify:track:a")));
        auto file = m_core->playFile(QStringLiteral("/music/song.flac"));
        // the spotify song stops for it
        QTRY_VERIFY(m_soloist->commandNames().contains(QStringLiteral("pause")));
        const auto call = m_fallback->take(QStringLiteral("playFile"));
        QVERIFY(call);
        QCOMPARE(call->argument, QStringLiteral("/music/song.flac"));
        call->succeed();
        QCoro::waitFor(file);
        m_fallback->duration = 1234;
        QCOMPARE(m_core->playbackDurationMs(), 1234u);
        m_core->pause();
        QVERIFY(m_fallback->log.contains(QStringLiteral("pause")));
    }

    void theKeyIsSavedForItsOwnerAlone()
    {
        const QString path = soloist::apiKeyPath();
        QVERIFY(path.startsWith(m_dir->path()));
        QString error;
        QVERIFY(!soloist::saveApiKey(QStringLiteral("   "), &error));
        QVERIFY(!error.isEmpty());
        // a paste that caught more than the key
        QVERIFY(!soloist::saveApiKey(QStringLiteral("key and more"), &error));
        QVERIFY(!QFile::exists(path));

        QVERIFY2(soloist::saveApiKey(QStringLiteral("  first-key\n"), &error), qPrintable(error));
        QCOMPARE(modeOf(path), mode_t(0600));
        QCOMPARE(soloist::readApiKey(), QStringLiteral("first-key"));

        // one left readable by others, as by hand, is made private again
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
        QVERIFY(soloist::saveApiKey(QStringLiteral("second"), &error));
        QCOMPARE(modeOf(path), mode_t(0600));
        QCOMPARE(soloist::readApiKey(), QStringLiteral("second"));

        // the variable wins over the file
        qputenv("SOLOIST_API_KEY", " from-environment ");
        QCOMPARE(soloist::readApiKey(), QStringLiteral("from-environment"));
    }

    void theSetupSaysWhatIsMissing()
    {
        findSoloistAt({});
        SoloistSetup setup(nullptr);
        QVERIFY(setup.supported());
        QCOMPARE(setup.state(), SoloistSetup::State::NotFound);
        // librespot refusing a song points here, while soloist isn't playing
        QVERIFY(setup.suggestedFor(QStringLiteral("Spotify could not load this track.")));
        QVERIFY(!setup.suggestedFor(QStringLiteral("Spotify Premium is required.")));

        QSignalSpy changed(&setup, &SoloistSetup::stateChanged);
        findSoloistAt(m_program);
        setup.refresh();
        QCOMPARE(changed.size(), 1);
        QCOMPARE(setup.program(), m_program);
        QCOMPARE(setup.state(), SoloistSetup::State::NoKey);
        QVERIFY(!SoloistCore::detect());

        QVERIFY(!setup.saveApiKey(QStringLiteral("two words")).isEmpty());
        QCOMPARE(setup.state(), SoloistSetup::State::NoKey);
        QCOMPARE(setup.saveApiKey(QStringLiteral("abc")), QString());
        QVERIFY(setup.hasKey());
        // playback picks its core as swiftify starts
        QCOMPARE(setup.state(), SoloistSetup::State::RestartNeeded);
        const auto config = SoloistCore::detect();
        QVERIFY(config);
        QCOMPARE(config->program, m_program);
        QCOMPARE(config->apiKey, QStringLiteral("abc"));

        qputenv("SWIFTIFY_PLAYBACK", "librespot");
        QCOMPARE(setup.state(), SoloistSetup::State::Disabled);
        QVERIFY(!setup.suggestedFor(QStringLiteral("Spotify could not load this track.")));
    }

    void theSetupFollowsTheSoloistItStarted()
    {
        SoloistSetup setup(nullptr);
        setup.attach(m_core.get());
        QTRY_COMPARE(setup.state(), SoloistSetup::State::Active);
        // soloist plays, so librespot's errors aren't soloist's to fix
        QVERIFY(!setup.suggestedFor(QStringLiteral("Spotify could not load this track.")));
        // position updates change nothing the settings show
        QSignalSpy changed(&setup, &SoloistSetup::stateChanged);
        QSignalSpy heard(m_core.get(), &SoloistCore::stateChanged);
        m_soloist->sendPosition(1000);
        m_soloist->sendStatus(QStringLiteral("paused"));
        QTRY_COMPARE(heard.size(), 2);
        QCOMPARE(changed.size(), 0);
    }

    void theSetupSaysSoloistWaitsToBePaired()
    {
        m_soloist->loggedIn = false;
        SoloistSetup setup(nullptr);
        setup.attach(m_core.get());
        QTRY_COMPARE(setup.state(), SoloistSetup::State::Unpaired);
    }

    void theSetupSaysABuildHasExpired()
    {
        // soloist 90 days on: it stops at once, with exit code 10
        const QString program = m_dir->filePath(QStringLiteral("expired"));
        QFile script(program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nexit 10\n");
        script.close();
        script.setPermissions(script.permissions() | QFile::ExeOwner);
        SoloistCore core(m_fallback.get(), SoloistConfig{program, QStringLiteral("key"), m_dir->filePath(QStringLiteral("expired-data"))});
        SoloistSetup setup(nullptr);
        setup.attach(&core);
        QTRY_COMPARE(setup.state(), SoloistSetup::State::Expired);
        QVERIFY(setup.failure().contains(QStringLiteral("expired")));
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_program;
    std::unique_ptr<FakeSoloist> m_soloist;
    std::unique_ptr<FakeCore> m_fallback;
    std::unique_ptr<SoloistCore> m_core;
};

QTEST_GUILESS_MAIN(TestSoloist)
#include "tst_soloist.moc"
