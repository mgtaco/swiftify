#pragma once

#include <QString>

struct SwiftifyLiveBuffer;
struct pa_context;
struct pa_stream;
struct pa_threaded_mainloop;

namespace swiftify {

/// where soloist plays when swiftify should hear it first: a sink of its own,
/// whose monitor is recorded into a live buffer for the rust core to play
/// through the equalizer and the visualizers. the sink goes again with this,
/// unless it was there before.
class SoloistAudioRoute {
public:
    static constexpr quint32 kChannels = 2;

    /// records into `buffer` at `rate`, which should be what swiftify's output
    /// runs at, so the audio is resampled once, by the sound server, if at all
    SoloistAudioRoute(SwiftifyLiveBuffer *buffer, quint32 rate);
    ~SoloistAudioRoute();
    SoloistAudioRoute(const SoloistAudioRoute &) = delete;
    SoloistAudioRoute &operator=(const SoloistAudioRoute &) = delete;

    /// makes the sink and starts recording it; false, with `error()` saying
    /// why, if the sound server would not
    bool start();
    /// the pipewire node soloist is told to play to
    static QString sinkName();
    QString error() const { return m_error; }

private:
    static void onContextState(pa_context *context, void *self);
    static void onStreamState(pa_stream *stream, void *self);
    static void onRead(pa_stream *stream, size_t bytes, void *self);
    void stop();
    /// waits on the mainloop, locked, until `done` holds or time is up
    template <typename Done>
    bool waitFor(Done done, int timeoutMs = 3000);

    SwiftifyLiveBuffer *m_buffer;
    quint32 m_rate;
    pa_threaded_mainloop *m_mainloop = nullptr;
    pa_context *m_context = nullptr;
    pa_stream *m_stream = nullptr;
    quint32 m_module = quint32(-1);
    QString m_error;
};

} // namespace swiftify
