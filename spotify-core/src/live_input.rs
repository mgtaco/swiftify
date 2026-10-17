//! audio another program plays, handed over as it plays: spotify soloist's,
//! recorded from a sink of its own. it goes the way a file does, through the
//! equalizer and the spectrum to the output, so both work on it too.

use std::{
    collections::VecDeque,
    sync::{
        Arc, Mutex, PoisonError,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};

use rodio::Source;

use crate::dsp::{DspProcessor, SharedDsp};
use crate::output::BlockSource;
use crate::spectrum::{ANALYSIS_SAMPLE_COUNT, SharedSpectrum};

/// what is waiting to be played, filled by the recorder and emptied by the
/// output. kept short, since everything in it is heard late by as much.
///
/// the recorder and the output run on different clocks (soloist's sink on
/// the system's, the speakers on their own), which drift apart by a few
/// frames a second. the level is followed smoothly, and a single frame is
/// dropped or repeated now and then to hold it, which can't be heard;
/// dropping a backlog wholesale is left for when something stalls.
pub struct LiveBuffer {
    queue: Mutex<Queue>,
    channels: usize,
    /// interleaved samples past which a backlog is cut, and what is kept
    limit: usize,
    keep: usize,
    /// interleaved samples held back after running dry, so playback starts
    /// again on a cushion rather than in fits
    cushion: usize,
    /// how far, in interleaved samples, the smoothed level may wander from
    /// where it settled before a frame is slipped, and where slipping stops
    tolerance: f64,
    settled_within: f64,
    /// frames played between slips, at the least
    slip_spacing: usize,
}

struct Queue {
    samples: VecDeque<f32>,
    /// the last frame played, repeated to slip one in
    last: Vec<f32>,
    /// the level at each output's start, smoothed over a couple of seconds
    level: f64,
    /// where it settled once playback began; None until it has
    target: Option<f64>,
    /// output callbacks since playback (re)started
    reads: usize,
    /// frames to play before another slip
    slip_due: usize,
    /// slipping until the level is back near the target
    correcting: bool,
    slips: u64,
}

/// output callbacks the level settles over before it is held
const SETTLE_READS: usize = 100;
/// how much of each new level the smoothed one takes
const SMOOTHING: f64 = 0.02;

impl LiveBuffer {
    /// for `channels` at `rate`: a cushion of 35ms, a backlog cut past 250ms
    pub fn new(rate: u32, channels: u32) -> Self {
        let channels = channels.max(1) as usize;
        let per_ms = rate.max(1) as f64 * channels as f64 / 1_000.0;
        Self {
            queue: Mutex::new(Queue {
                samples: VecDeque::with_capacity((per_ms * 500.0) as usize),
                last: vec![0.0; channels],
                level: 0.0,
                target: None,
                reads: 0,
                slip_due: 0,
                correcting: false,
                slips: 0,
            }),
            channels,
            limit: (per_ms * 250.0) as usize,
            keep: (per_ms * 60.0) as usize,
            cushion: (per_ms * 35.0) as usize,
            tolerance: per_ms * 8.0,
            settled_within: per_ms * 2.0,
            slip_spacing: (rate.max(1) / 20) as usize,
        }
    }

    fn queue(&self) -> std::sync::MutexGuard<'_, Queue> {
        self.queue.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// what was just recorded. a backlog is cut short when the output has
    /// stalled, so the delay never builds up
    pub fn push(&self, samples: &[f32]) {
        let mut queue = self.queue();
        queue.samples.extend(samples.iter().copied());
        if queue.samples.len() > self.limit {
            let excess = queue.samples.len() - self.keep;
            let excess = excess - excess % self.channels;
            queue.samples.drain(..excess);
            queue.target = None;
            queue.reads = 0;
        }
    }

    /// moves what fits into `out`, returning how much did
    fn take(&self, out: &mut [f32], primed: &mut bool) -> usize {
        let channels = self.channels;
        let mut queue = self.queue();
        if !*primed {
            if queue.samples.len() < self.cushion {
                return 0;
            }
            *primed = true;
            queue.target = None;
            queue.reads = 0;
            queue.level = queue.samples.len() as f64;
        }

        // follow the level, and once it has settled, hold it there
        let available = queue.samples.len() as f64;
        queue.level += (available - queue.level) * SMOOTHING;
        queue.reads += 1;
        if queue.target.is_none() && queue.reads >= SETTLE_READS {
            queue.target = Some(queue.level);
        }
        let mut written = 0;
        if let Some(target) = queue.target {
            let drift = queue.level - target;
            if drift.abs() > self.tolerance {
                queue.correcting = true;
            } else if drift.abs() < self.settled_within {
                queue.correcting = false;
            }
            if queue.correcting && queue.slip_due == 0 && out.len() >= channels {
                if drift > 0.0 && queue.samples.len() >= channels * 2 {
                    // the recorder runs ahead: one frame goes unplayed
                    queue.samples.drain(..channels);
                } else if drift < 0.0 {
                    // the output runs ahead: the last frame plays twice
                    let last = queue.last.clone();
                    out[..channels].copy_from_slice(&last);
                    written = channels;
                }
                queue.slip_due = self.slip_spacing;
                queue.slips += 1;
            }
        }

        let wanted = out.len() - written;
        let taken = wanted.min(queue.samples.len());
        let taken = taken - taken % channels;
        for (slot, sample) in out[written..written + taken].iter_mut().zip(queue.samples.drain(..taken)) {
            *slot = sample;
        }
        written += taken;
        if written >= channels {
            let last = out[written - channels..written].to_vec();
            queue.last.copy_from_slice(&last);
        }
        queue.slip_due = queue.slip_due.saturating_sub(written / channels);
        if written < out.len() {
            *primed = false;
        }
        written
    }

    pub fn clear(&self) {
        let mut queue = self.queue();
        queue.samples.clear();
        queue.target = None;
        queue.reads = 0;
    }

    #[cfg(test)]
    fn level(&self) -> usize {
        self.queue().samples.len()
    }

    #[cfg(test)]
    fn slips(&self) -> u64 {
        self.queue().slips
    }
}

/// a live buffer as an endless source: silence while nothing arrives
pub(crate) struct LiveSource {
    buffer: Arc<LiveBuffer>,
    channels: u16,
    rate: u32,
    cancel: Arc<AtomicBool>,
    dsp: DspProcessor,
    spectrum: SharedSpectrum,
    analysis_samples: [f32; ANALYSIS_SAMPLE_COUNT],
    analysis_index: usize,
    analysis_enabled: bool,
    primed: bool,
}

impl LiveSource {
    pub(crate) fn new(
        buffer: Arc<LiveBuffer>,
        rate: u32,
        channels: u32,
        cancel: Arc<AtomicBool>,
        dsp: SharedDsp,
        spectrum: SharedSpectrum,
    ) -> Self {
        Self {
            buffer,
            channels: channels.clamp(1, u16::MAX as u32) as u16,
            rate: rate.max(1),
            cancel,
            dsp: DspProcessor::new(dsp),
            spectrum,
            analysis_samples: [0.0; ANALYSIS_SAMPLE_COUNT],
            analysis_index: 0,
            analysis_enabled: false,
            primed: false,
        }
    }

    /// as LocalTrackSource does: what is about to play, a run at a time
    fn analyse(&mut self, mut samples: &[f32]) {
        while !samples.is_empty() {
            if self.analysis_index == 0 {
                self.analysis_enabled = self.spectrum.is_enabled();
            }
            let taken = samples
                .len()
                .min(ANALYSIS_SAMPLE_COUNT - self.analysis_index);
            if self.analysis_enabled {
                self.analysis_samples[self.analysis_index..self.analysis_index + taken]
                    .copy_from_slice(&samples[..taken]);
            }
            self.analysis_index += taken;
            samples = &samples[taken..];
            if self.analysis_index == ANALYSIS_SAMPLE_COUNT {
                if self.analysis_enabled {
                    self.spectrum
                        .queue_interleaved_f32(self.analysis_samples, 1.0);
                }
                self.analysis_index = 0;
            }
        }
    }
}

impl Iterator for LiveSource {
    type Item = f32;

    fn next(&mut self) -> Option<Self::Item> {
        let mut sample = [0.0];
        (self.read(&mut sample) == 1).then_some(sample[0])
    }
}

impl BlockSource for LiveSource {
    fn read(&mut self, out: &mut [f32]) -> usize {
        if self.cancel.load(Ordering::Relaxed) {
            return 0;
        }
        let taken = self.buffer.take(out, &mut self.primed);
        out[taken..].fill(0.0);
        // whole frames only, so the equalizer's channels stay in step
        let frames = taken - taken % usize::from(self.channels);
        self.dsp.equalize(&mut out[..frames]);
        self.analyse(&out[..]);
        out.len()
    }
}

impl Source for LiveSource {
    fn current_span_len(&self) -> Option<usize> {
        None
    }

    fn channels(&self) -> rodio::ChannelCount {
        self.channels
    }

    fn sample_rate(&self) -> rodio::SampleRate {
        self.rate
    }

    fn total_duration(&self) -> Option<Duration> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nothing_plays_until_the_cushion_is_there() {
        let buffer = LiveBuffer::new(1_000, 1);
        let mut primed = false;
        let mut out = [0.0; 8];
        buffer.push(&[1.0; 20]);
        assert_eq!(buffer.take(&mut out, &mut primed), 0);
        buffer.push(&[1.0; 40]);
        assert_eq!(buffer.take(&mut out, &mut primed), 8);
        assert!(primed);
    }

    #[test]
    fn running_dry_waits_for_a_new_cushion() {
        let buffer = LiveBuffer::new(1_000, 1);
        let mut primed = false;
        let mut out = [0.0; 100];
        buffer.push(&[1.0; 50]);
        assert_eq!(buffer.take(&mut out, &mut primed), 50);
        assert!(!primed);
        buffer.push(&[1.0; 10]);
        assert_eq!(buffer.take(&mut out, &mut primed), 0);
    }

    #[test]
    fn a_stalled_output_has_its_backlog_cut_short() {
        let buffer = LiveBuffer::new(1_000, 1);
        buffer.push(&[0.5; 1_000]);
        assert_eq!(buffer.level(), 60);
    }

    /// a recorder pushing 10ms at a time and an output taking 1024 frames,
    /// at 44.1kHz stereo, for `seconds`, the recorder's clock `ratio` times
    /// the output's. returns the levels seen, in ms, and whether it ever ran dry
    fn run(ratio: f64, seconds: f64) -> (Vec<f64>, bool, u64) {
        let rate = 44_100.0;
        let buffer = LiveBuffer::new(44_100, 2);
        let mut primed = false;
        let mut out = vec![0.0; 2_048];
        let chunk = 441;
        let (mut produced, mut consumed) = (0.0_f64, 0.0_f64);
        let mut levels = Vec::new();
        let mut dry = false;
        let mut time = 0.0;
        while time < seconds {
            time += 0.001;
            while produced < time * rate * ratio {
                buffer.push(&vec![0.25; chunk * 2]);
                produced += chunk as f64;
            }
            while consumed + 1_024.0 < time * rate {
                consumed += 1_024.0;
                let was_primed = primed;
                if buffer.take(&mut out, &mut primed) < out.len() && was_primed && time > 1.0 {
                    dry = true;
                }
                levels.push(buffer.level() as f64 / 2.0 / rate * 1_000.0);
            }
        }
        (levels, dry, buffer.slips())
    }

    #[test]
    fn matched_clocks_never_slip() {
        let (_, dry, slips) = run(1.0, 60.0);
        assert!(!dry);
        assert_eq!(slips, 0);
    }

    #[test]
    fn a_fast_recorder_is_held_without_a_cut() {
        // 200 parts per million: far more drift than real clocks show
        let (levels, dry, slips) = run(1.000_2, 600.0);
        assert!(!dry);
        assert!(slips > 0);
        let late = &levels[levels.len() - 1_000..];
        assert!(late.iter().all(|level| *level < 100.0), "the delay grew to {:?}", late.iter().cloned().fold(0.0, f64::max));
    }

    #[test]
    fn a_slow_recorder_is_held_without_running_dry() {
        let (_, dry, slips) = run(0.999_8, 600.0);
        assert!(!dry);
        assert!(slips > 0);
    }
}
