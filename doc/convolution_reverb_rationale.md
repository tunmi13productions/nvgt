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

## Known limitations

**Algorithmic latency of one partition.** The uniformly-partitioned overlap-add design means the
wet signal for a given input sample is not available until a full partition (default 1024 frames,
~21ms at 48kHz) has been buffered and processed. This is the standard cost of this convolution
scheme, not a bug; smaller `partition_size` values trade CPU (more, smaller FFTs) for lower
latency.

**No mid-playback resizing of the partition size.** `partition_size` is fixed at node construction,
same as the plate node's inability to change `size` without a click; swap partition size by
creating a new node.

**No SIMD, no real-valued FFT.** The FFT operates on complex data even though the input is real,
so it does about twice the work a real-input FFT (like pffft or kissfft) would need. This was an
explicit tradeoff to avoid vendoring a third dependency for a first working version; the license
concern raised in `plate_reverb_rationale.md` (pffft/kissfft are both permissive) still applies if
this becomes the bottleneck for long impulse responses.

**`load_ir` blocks the calling thread for the full precompute**, proportional to impulse response
length. It is not real-time safe with respect to *its own caller* even though it never blocks the
audio thread; call it before attaching the node to a sound that is about to play, not from
somewhere latency-sensitive.
