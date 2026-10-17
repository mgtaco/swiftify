//! playback dsp: a parametric equalizer, and tempo and pitch that can be moved
//! independently of one another.

use std::{
    f32::consts::TAU,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use librespot::playback::{NUM_CHANNELS, SAMPLE_RATE};

const CHANNELS: usize = NUM_CHANNELS as usize;

pub(crate) const EQ_BAND_COUNT: usize = 6;
/// the same centres spotify's own equalizer uses, so a curve copied from there
/// means the same thing here.
pub(crate) const EQ_FREQUENCIES: [f32; EQ_BAND_COUNT] =
    [60.0, 150.0, 400.0, 1_000.0, 2_400.0, 15_000.0];
/// wide enough that five bands cover the spectrum without gaps between them.
const EQ_Q: f32 = 0.9;
pub(crate) const MAX_GAIN_DB: f32 = 12.0;

#[derive(Clone, Copy, PartialEq, Debug)]
pub(crate) struct DspSettings {
    pub gains_db: [f32; EQ_BAND_COUNT],
    /// duration factor: 0.5 plays the track in twice the time.
    pub tempo: f32,
    pub pitch_semitones: f32,
    /// whether tempo leaves pitch alone. off is the slowed-and-reverb or
    /// nightcore effect: the whole recording is simply played at another rate.
    pub preserves_pitch: bool,
}

impl Default for DspSettings {
    fn default() -> Self {
        Self {
            gains_db: [0.0; EQ_BAND_COUNT],
            tempo: 1.0,
            pitch_semitones: 0.0,
            preserves_pitch: true,
        }
    }
}

impl DspSettings {
    pub(crate) fn sanitized(mut self) -> Self {
        for gain in &mut self.gains_db {
            *gain = gain.clamp(-MAX_GAIN_DB, MAX_GAIN_DB);
            if !gain.is_finite() {
                *gain = 0.0;
            }
        }
        self.tempo = if self.tempo.is_finite() {
            self.tempo.clamp(0.5, 2.0)
        } else {
            1.0
        };
        self.pitch_semitones = if self.pitch_semitones.is_finite() {
            self.pitch_semitones.clamp(-12.0, 12.0)
        } else {
            0.0
        };
        self
    }

    fn equalizes(&self) -> bool {
        self.gains_db.iter().any(|gain| gain.abs() > 0.01)
    }

    /// how much faster the input is read. multiplies pitch, divides duration.
    fn resample_ratio(&self) -> f32 {
        let pitch = 2f32.powf(self.pitch_semitones / 12.0);
        if self.preserves_pitch {
            pitch
        } else {
            pitch * self.tempo
        }
    }

    /// how much the resampled audio is stretched back out. multiplies duration, leaves
    /// pitch alone.
    fn stretch_factor(&self) -> f32 {
        self.resample_ratio() / self.tempo
    }
}

/// the settings, as the ui sets them and the audio thread reads them.
#[derive(Clone)]
pub(crate) struct SharedDsp {
    settings: Arc<Mutex<DspSettings>>,
    generation: Arc<AtomicU64>,
}

impl SharedDsp {
    pub(crate) fn new() -> Self {
        Self {
            settings: Arc::new(Mutex::new(DspSettings::default())),
            generation: Arc::new(AtomicU64::new(0)),
        }
    }

    pub(crate) fn set(&self, settings: DspSettings) {
        if let Ok(mut current) = self.settings.lock() {
            let sanitized = settings.sanitized();
            if *current == sanitized {
                return;
            }
            *current = sanitized;
            self.generation.fetch_add(1, Ordering::Release);
        }
    }

    pub(crate) fn get(&self) -> DspSettings {
        self.settings
            .lock()
            .map(|settings| *settings)
            .unwrap_or_default()
    }

    fn generation(&self) -> u64 {
        self.generation.load(Ordering::Acquire)
    }
}

/// one chain, owned by whichever thread is feeding the output.
pub(crate) struct DspProcessor {
    shared: SharedDsp,
    settings: DspSettings,
    generation: u64,
    equalizers: [ChannelEqualizer; CHANNELS],
    resampler: Resampler,
    stretch: TimeStretch,
}

impl DspProcessor {
    pub(crate) fn new(shared: SharedDsp) -> Self {
        let settings = shared.get();
        Self {
            generation: shared.generation(),
            equalizers: std::array::from_fn(|_| ChannelEqualizer::new(settings.gains_db)),
            resampler: Resampler::default(),
            stretch: TimeStretch::new(),
            settings,
            shared,
        }
    }

    /// drops every buffer. called between tracks, so the tail of one cannot
    /// be overlap-added onto the head of the next.
    pub(crate) fn reset(&mut self) {
        self.resampler = Resampler::default();
        self.stretch = TimeStretch::new();
    }

    pub(crate) fn process(&mut self, samples: Vec<f32>) -> Vec<f32> {
        self.refresh();

        let mut samples = samples;
        if self.settings.equalizes() {
            for (index, sample) in samples.iter_mut().enumerate() {
                *sample = self.equalizers[index % CHANNELS].process(*sample);
            }
        }

        let ratio = self.settings.resample_ratio();
        if (ratio - 1.0).abs() > 1e-4 {
            samples = self.resampler.process(&samples, ratio);
        }

        let stretch = self.settings.stretch_factor();
        if (stretch - 1.0).abs() > 1e-4 {
            samples = self.stretch.process(&samples, stretch);
        }

        samples
    }

    /// the equalizer alone, in place: for live audio, which can be neither
    /// slowed down nor sped up, so tempo and pitch are left out
    pub(crate) fn equalize(&mut self, samples: &mut [f32]) {
        self.refresh();
        if self.settings.equalizes() {
            for (index, sample) in samples.iter_mut().enumerate() {
                *sample = self.equalizers[index % CHANNELS].process(*sample);
            }
        }
    }

    fn refresh(&mut self) {
        let generation = self.shared.generation();
        if generation == self.generation {
            return;
        }
        self.generation = generation;
        let settings = self.shared.get();
        if settings.gains_db != self.settings.gains_db {
            for equalizer in &mut self.equalizers {
                equalizer.retune(settings.gains_db);
            }
        }
        self.settings = settings;
    }
}

// mark: - equalizer

/// direct form i. peaking filters in series, one per band.
#[derive(Clone, Copy, Default)]
struct Biquad {
    b0: f32,
    b1: f32,
    b2: f32,
    a1: f32,
    a2: f32,
    x1: f32,
    x2: f32,
    y1: f32,
    y2: f32,
}

impl Biquad {
    /// rbj cookbook peaking eq.
    fn peaking(frequency: f32, q: f32, gain_db: f32) -> Self {
        let amplitude = 10f32.powf(gain_db / 40.0);
        let omega = TAU * frequency / SAMPLE_RATE as f32;
        let (sin, cos) = (omega.sin(), omega.cos());
        let alpha = sin / (2.0 * q);

        let a0 = 1.0 + alpha / amplitude;
        Self {
            b0: (1.0 + alpha * amplitude) / a0,
            b1: (-2.0 * cos) / a0,
            b2: (1.0 - alpha * amplitude) / a0,
            a1: (-2.0 * cos) / a0,
            a2: (1.0 - alpha / amplitude) / a0,
            ..Default::default()
        }
    }

    fn process(&mut self, x: f32) -> f32 {
        let y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2
            - self.a1 * self.y1
            - self.a2 * self.y2;
        self.x2 = self.x1;
        self.x1 = x;
        self.y2 = self.y1;
        self.y1 = y;
        y
    }
}

struct ChannelEqualizer {
    bands: [Biquad; EQ_BAND_COUNT],
    active: bool,
}

impl ChannelEqualizer {
    fn new(gains_db: [f32; EQ_BAND_COUNT]) -> Self {
        let mut equalizer = Self {
            bands: [Biquad::default(); EQ_BAND_COUNT],
            active: false,
        };
        equalizer.retune(gains_db);
        equalizer
    }

    /// coefficients change, filter state does not: recreating the whole filter
    /// on a slider drag would zero its history and click.
    fn retune(&mut self, gains_db: [f32; EQ_BAND_COUNT]) {
        self.active = gains_db.iter().any(|gain| gain.abs() > 0.01);
        for (index, gain) in gains_db.iter().enumerate() {
            let tuned = Biquad::peaking(EQ_FREQUENCIES[index], EQ_Q, *gain);
            let band = &mut self.bands[index];
            band.b0 = tuned.b0;
            band.b1 = tuned.b1;
            band.b2 = tuned.b2;
            band.a1 = tuned.a1;
            band.a2 = tuned.a2;
        }
    }

    fn process(&mut self, sample: f32) -> f32 {
        if !self.active {
            return sample;
        }
        let mut value = sample;
        for band in &mut self.bands {
            value = band.process(value);
        }
        value
    }
}

// mark: - resampling

/// linear interpolation. changes tempo and pitch together, which is exactly
/// the slowed or sped-up sound when pitch is not being preserved, and is the
/// pitch stage of the chain when it is.
#[derive(Default)]
struct Resampler {
    /// fractional read position within the block, carried across calls.
    position: f32,
    /// last frame of the previous block, so interpolation has somewhere to
    /// come from at a block boundary.
    carry: [f32; CHANNELS],
    primed: bool,
}

impl Resampler {
    fn process(&mut self, input: &[f32], ratio: f32) -> Vec<f32> {
        let frames = input.len() / CHANNELS;
        if frames == 0 {
            return Vec::new();
        }
        if !self.primed {
            self.carry = [input[0], input[1.min(input.len() - 1)]];
            self.primed = true;
        }

        let mut output = Vec::with_capacity((frames as f32 / ratio) as usize * CHANNELS + 4);
        let limit = frames as f32;
        while self.position < limit {
            let index = self.position.floor() as usize;
            let fraction = self.position - index as f32;
            for channel in 0..CHANNELS {
                let a = if index == 0 {
                    self.carry[channel]
                } else {
                    input[(index - 1) * CHANNELS + channel]
                };
                let b = input[index * CHANNELS + channel];
                output.push(a + (b - a) * fraction);
            }
            self.position += ratio;
        }
        self.position -= limit;

        self.carry
            .copy_from_slice(&input[(frames - 1) * CHANNELS..frames * CHANNELS]);
        output
    }
}

// mark: - time stretching

/// frame length, per channel. about 23 ms at 44.1 khz, which is long enough to
/// hold a bass period and short enough not to smear transients.
const FRAME: usize = 1_024;
const SYNTHESIS_HOP: usize = FRAME / 2;
/// how far the search may slide a frame to find a phase-aligned join.
const SEARCH: isize = 256;
/// the correlation search runs on every fourth sample, then refines. a full
/// search costs about sixteen times as much for no audible gain.
const COARSE: isize = 4;
const COMPACT_AFTER_FRAMES: usize = FRAME * 4;

/// wsola: overlap-add at a fixed output rate, taking each frame from wherever nearby it
/// lines up best with what has already been written.
struct TimeStretch {
    /// interleaved, and consumed from the front as frames are taken.
    input: Vec<f32>,
    /// the part of the last output frame not yet emitted, waiting to be
    /// overlap-added with the next one.
    tail: Vec<f32>,
    window: Vec<f32>,
    /// fractional read position, in frames, from the head of input.
    read: f64,
}

impl TimeStretch {
    fn new() -> Self {
        Self {
            input: Vec::new(),
            tail: vec![0.0; (FRAME - SYNTHESIS_HOP) * CHANNELS],
            window: (0..FRAME)
                .map(|index| {
                    let phase = TAU * index as f32 / FRAME as f32;
                    0.5 - 0.5 * phase.cos()
                })
                .collect(),
            // starts a search window in, so the very first frame has room to
            // slide backwards. at zero the search could never fit and the
            // loop below never ran at all.
            read: SEARCH as f64,
        }
    }

    fn process(&mut self, input: &[f32], stretch: f32) -> Vec<f32> {
        self.input.extend_from_slice(input);

        let analysis_hop = (SYNTHESIS_HOP as f64 / stretch as f64).max(1.0);
        let overlap = FRAME - SYNTHESIS_HOP;
        let mut output = Vec::with_capacity(input.len());

        loop {
            let centre = self.read.floor() as isize;
            // room for the whole frame, plus the search window either side.
            let last = centre + SEARCH + FRAME as isize;
            if centre - SEARCH < 0 || last as usize * CHANNELS > self.input.len() {
                break;
            }

            let start = self.best_offset(centre);
            for index in 0..overlap {
                for channel in 0..CHANNELS {
                    let sample = self.input[(start as usize + index) * CHANNELS + channel]
                        * self.window[index];
                    output.push(self.tail[index * CHANNELS + channel] + sample);
                }
            }

            // what is left of this frame becomes the tail the next one joins.
            for index in 0..overlap {
                for channel in 0..CHANNELS {
                    let source = start as usize + SYNTHESIS_HOP + index;
                    self.tail[index * CHANNELS + channel] = self.input[source * CHANNELS + channel]
                        * self.window[SYNTHESIS_HOP + index];
                }
            }

            self.read += analysis_hop;

            // drop what the search can no longer reach back to.
            let consumed = (self.read.floor() as isize - SEARCH).max(0) as usize;
            if consumed >= COMPACT_AFTER_FRAMES {
                self.input.drain(..consumed * CHANNELS);
                self.read -= consumed as f64;
            }
        }

        output
    }

    /// the frame start, within the search window, whose opening overlap best
    /// matches the tail it is about to be added to.
    fn best_offset(&self, centre: isize) -> isize {
        let overlap = FRAME - SYNTHESIS_HOP;
        let mut best = centre;
        let mut best_score = f32::NEG_INFINITY;

        let mut coarse = -SEARCH;
        while coarse <= SEARCH {
            let score = self.correlation(centre + coarse, overlap, COARSE as usize);
            if score > best_score {
                best_score = score;
                best = centre + coarse;
            }
            coarse += COARSE;
        }

        let mut fine = best - COARSE + 1;
        while fine < best + COARSE {
            if fine >= centre - SEARCH && fine <= centre + SEARCH {
                let score = self.correlation(fine, overlap, 1);
                if score > best_score {
                    best_score = score;
                    best = fine;
                }
            }
            fine += 1;
        }
        best
    }

    fn correlation(&self, start: isize, overlap: usize, step: usize) -> f32 {
        if start < 0 {
            return f32::NEG_INFINITY;
        }
        let start = start as usize;
        let mut sum = 0.0;
        let mut index = 0;
        while index < overlap {
            let offset = (start + index) * CHANNELS;
            // the left channel alone is enough to find the join, and costs
            // half of what mixing both down would.
            sum += self.input[offset] * self.tail[index * CHANNELS];
            index += step;
        }
        sum
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tone(frequency: f32, seconds: f32) -> Vec<f32> {
        let frames = (SAMPLE_RATE as f32 * seconds) as usize;
        (0..frames)
            .flat_map(|frame| {
                let value = (TAU * frequency * frame as f32 / SAMPLE_RATE as f32).sin() * 0.5;
                [value, value]
            })
            .collect()
    }

    /// zero crossings per second of the left channel, which is a frequency
    /// measurement that does not need an fft.
    fn crossings_per_second(samples: &[f32]) -> f32 {
        let frames = samples.len() / CHANNELS;
        if frames < 2 {
            return 0.0;
        }
        let crossings = (1..frames)
            .filter(|frame| {
                let previous = samples[(frame - 1) * CHANNELS];
                let current = samples[frame * CHANNELS];
                previous <= 0.0 && current > 0.0
            })
            .count();
        crossings as f32 * SAMPLE_RATE as f32 / frames as f32
    }

    fn settings(tempo: f32, semitones: f32, preserves_pitch: bool) -> DspSettings {
        DspSettings {
            tempo,
            pitch_semitones: semitones,
            preserves_pitch,
            ..DspSettings::default()
        }
    }

    #[test]
    fn a_flat_chain_returns_the_audio_untouched() {
        let shared = SharedDsp::new();
        let mut processor = DspProcessor::new(shared);
        let input = tone(440.0, 0.2);
        assert_eq!(processor.process(input.clone()), input);
    }

    #[test]
    fn halving_the_tempo_doubles_the_length_and_keeps_the_pitch() {
        let shared = SharedDsp::new();
        shared.set(settings(0.5, 0.0, true));
        let mut processor = DspProcessor::new(shared);

        let mut output = Vec::new();
        for _ in 0..40 {
            output.extend(processor.process(tone(440.0, 0.05)));
        }

        let input_frames = 40 * (SAMPLE_RATE as f32 * 0.05) as usize;
        let ratio = (output.len() / CHANNELS) as f32 / input_frames as f32;
        assert!(
            (ratio - 2.0).abs() < 0.08,
            "expected roughly twice the frames, got {ratio}x"
        );

        let frequency = crossings_per_second(&output);
        assert!(
            (frequency - 440.0).abs() < 20.0,
            "pitch should not move when only the tempo does, measured {frequency} Hz"
        );
    }

    #[test]
    fn letting_pitch_follow_the_tempo_lowers_the_tone() {
        let shared = SharedDsp::new();
        shared.set(settings(0.5, 0.0, false));
        let mut processor = DspProcessor::new(shared);

        let mut output = Vec::new();
        for _ in 0..20 {
            output.extend(processor.process(tone(440.0, 0.05)));
        }

        let frequency = crossings_per_second(&output);
        assert!(
            (frequency - 220.0).abs() < 10.0,
            "half speed should halve the frequency, measured {frequency} Hz"
        );
    }

    #[test]
    fn an_octave_of_pitch_doubles_the_frequency_and_keeps_the_length() {
        let shared = SharedDsp::new();
        shared.set(settings(1.0, 12.0, true));
        let mut processor = DspProcessor::new(shared);

        let mut output = Vec::new();
        for _ in 0..40 {
            output.extend(processor.process(tone(440.0, 0.05)));
        }

        let input_frames = 40 * (SAMPLE_RATE as f32 * 0.05) as usize;
        let ratio = (output.len() / CHANNELS) as f32 / input_frames as f32;
        assert!(
            (ratio - 1.0).abs() < 0.08,
            "the track should still take as long, got {ratio}x"
        );

        let frequency = crossings_per_second(&output);
        assert!(
            (frequency - 880.0).abs() < 40.0,
            "an octave up should double the frequency, measured {frequency} Hz"
        );
    }

    #[test]
    fn a_boosted_band_grows_and_leaves_the_others_alone() {
        let shared = SharedDsp::new();
        let mut boosted = DspSettings::default();
        boosted.gains_db[0] = 12.0;
        shared.set(boosted);
        let mut processor = DspProcessor::new(shared);

        let bass = processor.process(tone(EQ_FREQUENCIES[0], 0.5));
        let mut flat = DspProcessor::new(SharedDsp::new());
        let air = processor.process(tone(EQ_FREQUENCIES[5], 0.5));
        let air_reference = flat.process(tone(EQ_FREQUENCIES[5], 0.5));

        // compare the settled half, past the filter's own start-up.
        let energy = |samples: &[f32]| -> f32 {
            let half = samples.len() / 2;
            samples[half..]
                .iter()
                .map(|value| value * value)
                .sum::<f32>()
                / (samples.len() - half) as f32
        };

        assert!(
            energy(&bass) > energy(&air) * 4.0,
            "a 12 dB boost at {} Hz should stand well above an untouched band",
            EQ_FREQUENCIES[0]
        );
        assert!(
            (energy(&air) - energy(&air_reference)).abs() < energy(&air_reference) * 0.25,
            "a band nobody touched should come out roughly as it went in"
        );
    }

    #[test]
    fn settings_are_clamped_to_something_playable() {
        let wild = DspSettings {
            gains_db: [40.0; EQ_BAND_COUNT],
            tempo: 12.0,
            pitch_semitones: f32::NAN,
            preserves_pitch: true,
        }
        .sanitized();

        assert_eq!(wild.gains_db[0], MAX_GAIN_DB);
        assert_eq!(wild.tempo, 2.0);
        assert_eq!(wild.pitch_semitones, 0.0);
    }
}
