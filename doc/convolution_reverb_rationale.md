# Convolution reverb: what it is, and what it is not

`plate_reverb_rationale.md` flagged convolution as the natural second reverb node, since it is the
same partitioned FFT machinery the phonon HRTF path already uses, and does not compete with the
plate on license grounds: an FFT is math, not a copyrightable expression, and the algorithm here
(uniformly partitioned overlap-add, Cooley-Tukey radix-2) is written from scratch.

## Where the algorithm came from

A friend of the project's maintainer had already built and field-tested a partitioned convolution
reverb inside a different application ("fastplay"), and gave permission to bring it into NVGT.
That code turned out not to be a miniaudio node at all: it was a raw stereo float buffer processor
hooked into the BASS audio library's `BASS_ChannelSetDSP` callback, decoding impulse responses
through BASS's own stream reader. The core DSP loop — overlap-save block buffering, a frequency
delay line of FFT'd partitions, block-synchronous accumulation and inverse FFT, overlap-add output
— was sound and is preserved here essentially unchanged. Everything around it was rewritten:

- **The node itself** (`dep/ma_convolution_node.c`/`.h`) follows the same shape as
  `ma_plate_node.c`: a `ma_node_base`, a `process_pcm_frames` vtable entry, `MA_NODE_FLAG_CONTINUOUS_PROCESSING`
  so the tail keeps running once the input goes quiet.
- **Channel count is generalized**, not hardcoded to stereo. The node runs at whatever channel
  count the engine is using; a mono impulse response is broadcast to every channel, and if the
  impulse response has fewer channels than the engine, the last one is reused for the remainder.
- **Impulse response loading goes through `audio_decoder`** instead of BASS, so it gets every
  format, pack file, and resampling support the rest of the sound system already has, arriving
  pre-matched to the engine's sample rate and channel count before a single partition is built.
- **The FFT is real/imaginary float arrays in C**, not `std::complex`, because the node lives in
  the same plain-C dependency layer as the plate and reverb nodes.

## Loading an impulse response is not real-time safe by nature, so it is made safe explicitly

Unlike the plate and freeverb nodes, this one's buffer sizes are not known until an impulse
response is loaded, so they cannot be carved from one heap block at init time the way
`ma_plate_node` does. `ma_convolution_node_set_ir()` therefore builds the entire new partitioned
spectrum — one FFT per partition per channel — into freshly allocated scratch memory first, and
only takes the node's mutex to swap the new buffer pointers in and free the old ones. The audio
thread takes the same mutex around each `process_pcm_frames` call. Contention only happens on the
rare call to `load_ir`/`clear_ir`, so in practice the lock is uncontended and the swap itself is a
handful of pointer copies, not the FFT work.

## What was verified

`load_ir` was pointed at real files through the normal test harness (`test/interact/sound_convolution_verb.nvgt`,
which mirrors `sound_verb.nvgt` and `sound_plate_verb.nvgt`, the working examples for the other two
reverbs) to confirm playback, wet/dry sweeping, and the reverb3d attachment path all work exactly
as they do for the plate and freeverb nodes, since `convolution_reverb_node` is a plain `audio_node`
and needed no changes to `reverb3d` or the spatializer to plug in.

The DSP core was cross-checked independently of NVGT itself: a standalone harness includes
`ma_convolution_node.c` directly (so it can drive the static `process_pcm_frames` callback without
a running audio device) and compares the streamed partitioned-FFT output against a plain O(n·m)
reference convolution, across mono and stereo, single- and multi-partition impulse responses,
channel-count mismatches (mono IR broadcast, stereo IR, 4-channel IR truncated to stereo), and an
IR spanning many dozens of partitions. All six cases matched the reference to float rounding error
(worst case ~3e-5 against reference magnitudes up to ~9), with no NaNs or infinities anywhere.
`dep/ma_convolution_node.c` also compiles clean at `/W4`, and the C++ wrapper in
`sound_nodes.cpp`/`sound.cpp` builds and links into the real stub binaries with no new warnings.

## Cost controls (second revision)

The first version gave a game no way to bound what convolution costs, and Golden Crayon, which put a node on every sound standing inside a convolution space, ran straight into that. The node was rewritten around the techniques Hide and Seek's legacy_sound convolver already uses:

- **Tail cap.** `max_ir_seconds` cuts the response at that length with a 10 ms fade, and the decoder never reads past it. Cost and memory are both linear in response length, and a 20 second preset file was otherwise 60 MB and dozens of times the work of a 1.5 second one.
- **Reduced-rate wet path.** `ir_rate_divisor` (1, 2, 4 or 8) runs the convolution at the device rate divided by that. The input is low-passed with a sixth-order Butterworth and decimated, the response is low-passed forwards and back (keeping its timing) and decimated to match, and the output is zero-stuffed and low-passed again. 2 is roughly half the work and loses what is above 0.4 times the reduced rate (9.6 kHz at 48 kHz with a divisor of 2). Off by default, because a node used as an insert with dry at 0 would take the direct sound's top end with it.
- **Idle sleep.** Once the input has been silent for longer than the response takes to ring out, the delay line holds nothing but zeros, so the node clears its state and stops convolving until the input comes back. Exact, not an approximation.
- **Cheaper inner loop.** Bit-reversal and twiddle tables are built once per response instead of calling sin/cos per FFT stage, the partition sum uses SSE on x86, and denormals are flushed for the length of each callback.
- **Background loading.** `load_ir_async` decodes and transforms on a worker thread, one load at a time across all nodes, and swaps the finished state in with a single pointer write under the node's lock. Whatever was loaded keeps playing until then. `ir_loading` and `ir_load_failed` report progress. A new load, `clear_ir` and the destructor cancel one in flight. The worker is never joined, because a node can be destroyed while the graph mutex is held and the worker's decoder needs that mutex; it shares a small ref-counted block with the node instead, and touches the node only under that block's own mutex, which the destructor takes to detach it.

Everything a loaded response needs now lives in one allocation that is built completely before the swap and freed after the lock is released, so the audio thread never allocates or frees. The design switched from overlap-add to overlap-save, which needs no pending-tail buffer. Checked with a standalone harness against a direct time-domain convolution under irregular callback sizes: max error about 3e-6 at full rate, the cut and fade match, a node that slept and woke again still matches exactly, and a half-rate node returns a 400 Hz sine at unity level. On the development machine one second of stereo audio through a 1.5 second response costs about 28 ms at full rate and 10 ms at half rate.

## Known limitations

**Algorithmic latency of one partition.** The wet signal lags the input by exactly `partition_size` frames (default now 512, about 11 ms at 48 kHz), at any rate divisor. Total cost barely depends on the partition size, since the partition sum is about twice the response length in multiply-adds per sample either way, so the smaller default is nearly free.

**No mid-playback resizing of the partition size.** `partition_size` is fixed at node construction, same as the plate node's inability to change `size` without a click; swap partition size by creating a new node.

**No real-valued FFT.** The FFT still operates on complex data even though the input is real, about twice the work a real-input FFT would need. The license concern raised in `plate_reverb_rationale.md` (pffft/kissfft are both permissive) still applies if this becomes the bottleneck.

**`load_ir` still blocks its caller** for the decode and precompute. `load_ir_async` exists for anything latency-sensitive; `load_ir` remains for one-off callers that want the result immediately, like a preview that reports a bad file.
