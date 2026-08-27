# Why the Dattorro plate, and not the other four

NVGT shipped one reverb: `audio_freeverb_node`, a wrapper around Philip Bennefall's verblib, which is itself a cleanup of Jezar's public domain Freeverb from 2000. It is fine. It is also 8 parallel comb filters into 4 allpasses, and that architecture has a ceiling you can hear.

This document records why the replacement is a Dattorro plate rather than Zita-rev1, an Airwindows port, convolution, or Freeverb3, and what the new node actually costs.

## The constraint that decided it

NVGT is zlib licensed. That is not a detail, it is the first filter, and it eliminates two of the five candidates before any listening happens.

| Candidate | License | Verdict |
| --- | --- | --- |
| Dattorro plate | Algorithm published in a paper, implementation written here | Clear |
| Airwindows | MIT | Clear |
| Convolution | Depends on the FFT chosen; pffft and kissfft are permissive | Clear |
| Zita-rev1 | GPL-2+ | Cannot be vendored |
| Freeverb3 | GPL | Cannot be vendored |

An algorithm is not copyrightable; a particular expression of it is. Implementing Dattorro's plate from the description in his paper is the ordinary, legitimate path, and it is the same path every commercial plate has taken since 1997.

## The integration surface was already the right shape

The reason the effort estimate came in low is that NVGT had already done the hard part. Three facts made this a contained job:

1. `dep/ma_reverb_node.c` is 85 lines. The whole contract a reverb has to satisfy is one `process_pcm_frames` callback taking interleaved float in and writing interleaved float out at the same channel count, with `MA_NODE_FLAG_CONTINUOUS_PROCESSING` so the tail keeps running after the input stops.
2. `freeverb_node_impl` in `src/sound_nodes.cpp` is about 25 lines of parameter forwarding onto an abstract interface.
3. `reverb3d::create` takes a plain `audio_node*`. Anything that is a node drops into the 3D path with no changes to the spatializer, the attenuation model, or the send volumes.

So the work was: write the DSP, wrap it in a vtable, forward the parameters, register the properties. No change to the audio graph, no change to `reverb3d`, no change to any existing script.

## What the plate does that comb filters cannot

Freeverb's problem is structural, not a matter of tuning.

**Comb filters have a fixed, visible resonance comb.** Eight parallel combs at 1116, 1188, 1277, 1356, 1422, 1491, 1557 and 1617 samples produce eight harmonic series of peaks that never move. The ear latches onto those peaks. On a short transient this reads as a metallic, faintly pitched ring, and it is why Freeverb has a reputation for sounding "cheap" on percussive material. Adding more combs does not fix it, it just adds more fixed peaks.

**The plate has no fixed peaks, by design.** The tank is a figure of eight loop rather than parallel resonators, and the two allpasses at the head of each half have delay lengths that are continuously modulated by a slow oscillator. The resonances wander, so no single frequency ever accumulates enough to be heard as a pitch. This is the single largest audible difference and it is the whole reason the topology exists.

**Freeverb's stereo image is faked.** verblib runs two identical banks whose delay lengths differ by a constant 23 samples (`verblib_stereospread`), then blends them with a width control. The plate reads seven taps per ear from six different points in the tank, arranged so each ear reads mostly from the far half. The decorrelation is real, produced by the structure rather than applied afterwards.

**Freeverb has no predelay and no input conditioning.** The plate has both: an adjustable predelay, a one-pole input lowpass, and four allpass diffusers that smear the input before it ever reaches the tank. Predelay in particular is the parameter that lets a reverb sit behind a sound rather than smearing it, and games that want a sense of room size at close range need it.

**Freeverb's `room_size` is a feedback coefficient, not a size.** It changes how long the tail lasts. It does not change the delay lengths, so a "small room" and a "large hall" have identical modal structure and differ only in decay time. The plate separates these: `decay` controls tail length, `size` retunes every delay in the tank.

## The new parameters

| Property | Range | What it does |
| --- | --- | --- |
| `predelay` | 0 to 0.25 s | Gap before the reverb starts |
| `bandwidth` | 0 to 1 | How much high frequency reaches the tank |
| `decay` | 0 to 0.999 | Tail length |
| `damping` | 0 to 1 | How fast highs are lost as the tail decays |
| `size` | 0.1 to 2 | Scale factor on every tank delay |
| `input_diffusion_1`, `input_diffusion_2` | 0 to 0.99 | How much the input is smeared before the tank |
| `decay_diffusion_1` | 0 to 0.79 | Smearing in the modulated tank allpasses |
| `decay_diffusion_2` | 0 to 0.99 | Smearing in the second tank allpasses |
| `modulation_depth` | 0 to 1 | How far the tank allpasses wander |
| `modulation_rate` | 0 to 20 Hz | How fast they wander |
| `wet`, `dry`, `width` | 0 to 1 | Mix, as on freeverb |
| `frozen` | bool | Holds the tank at unity and shuts the input |
| `decay_time_in_frames` | read only | Estimated tail length |

`decay_diffusion_1` is capped at 0.79 rather than 1.0 because the tank rings audibly as it approaches unity. That limit is in the paper.

## Measured cost

All figures from `dep/plateverb.h` compiled with MSVC at `/O2`, stereo, measured against verblib on the same machine and the same input.

**CPU.** 200 seconds of stereo audio at 48 kHz:

```
freeverb  0.360 s  ->  0.180% of one core,  556x realtime
plate     0.436 s  ->  0.218% of one core,  459x realtime
```

The plate costs 1.21x what freeverb costs. In absolute terms that is roughly two thousandths of a core per instance, so a game can run many of them.

**Memory.** Freeverb's buffers are fixed inline arrays sized for 4x44100 regardless of the actual rate, so a `ma_reverb_node` is about 800 KB whatever you do. The plate allocates once from the miniaudio allocation callbacks, sized to the actual device rate:

```
 22050 Hz   150 KB
 44100 Hz   299 KB
 48000 Hz   325 KB
192000 Hz  1300 KB
```

At the rates games actually run, the plate uses less memory than the node it sits beside, and it is the only one of the two that scales down at low rates.

**Level.** This one mattered more than it looks. `reverb3d` computes send volumes against defaults of -7 and -5 dB that were tuned by ear against freeverb. If the plate were louder, every existing 3D reverb setup would need retuning. Measured on a 2 second noise burst plus tail, at 48 kHz:

```
source                     rms -21.59 dB
freeverb (node defaults)   rms -25.27 dB
plate    (node defaults)   rms -25.30 dB
```

A `plateverb_input_gain` constant of 0.625 trims the tank so the two match within 0.03 dB. Swapping the node in `reverb3d` changes the character and leaves the level alone.

## What was verified

A standalone harness (compiled at `/W4`, no warnings) runs an impulse through the tank at 22050, 44100, 48000 and 192000 Hz and asserts:

- the response is audible and bounded, and never produces NaN
- the tail decays rather than sustaining, and raising `decay` lengthens it
- damping removes energy
- both size extremes stay bounded, including size 0.1 with decay 0.999, damping 0, full modulation and maximum diffusion all at once, which is where an out of bounds read into a shortened delay line would show up
- freeze sustains and reports an infinite decay time
- mono works and three channels is rejected

`dep/ma_plate_node.c` compiles warning-free, and the new `plate_reverb_node_impl` adds no warnings to `src/sound_nodes.cpp`.

## Known limitations

**`size` clicks when changed during playback.** It retunes twelve delay lines and fourteen output taps at once. Every plate implementation behaves this way, commercial ones included; size is a setting, not a sweep. `decay`, `damping`, `bandwidth`, `wet`, `dry` and `width` are all safe to change while audio runs, and those are the ones games actually automate when a player moves between spaces.

**Minimum predelay is two samples, not zero.** The interpolated read needs to stay clear of the write position. At 48 kHz that is 42 microseconds.

**Stereo input is summed.** A plate is mono in, stereo out. There is no `input_width` equivalent because the tank has one input; the stereo image comes from the tap arrangement instead.

## What was not done

The changelog was left alone. `doc/src/appendix/Changelog.md` is version scoped and its newest entry is 0.89.1-beta from October 2024, and the recent commits on this branch have not been adding to it either. If this goes upstream it wants an entry under whatever version it lands in.

There is no reference documentation page, because there is not one for `audio_freeverb_node` or any other audio node either. `test/interact/sound_plate_verb.nvgt` is the working example, and it mirrors `sound_verb.nvgt` so the two can be compared by ear directly.

## If this is not enough later

The thing a plate structurally cannot do is frequency dependent decay time, where bass rings longer than treble the way it does in a real hall. `damping` is a per pass lowpass, which is a coarse approximation of it. Getting that properly means a feedback delay network with separate low and mid RT60 and a crossover, which is what Zita-rev1 does and why it sounds the way it does. That is the natural follow up, and it has to be written from scratch for the same license reason.

Convolution remains worth adding as a second node rather than a replacement, since it has no parametric controls at all and cannot answer a script that wants to vary room size at runtime. It also pairs naturally with the existing HRTF work, since it is the same partitioned FFT machinery.
