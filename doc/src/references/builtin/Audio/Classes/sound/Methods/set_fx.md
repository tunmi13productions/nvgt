# set_fx
Adds, changes or removes an effect on a sound or a mixer.

`bool set_fx(const string&in fx, int index = -1);`

## Arguments:
* const string&in fx: the effect string, described below.
* int index = -1: where in the chain to put a new effect. -1, or any index past the end, adds it at the end.

## Returns:
bool: true if the effect was added, changed or removed, false otherwise, for example if the effect is unknown or given too few parameters.

## Remarks:
An effect string is the effect's name followed by its parameters, separated by colons, such as `"lowpass:800:0:0.7"`. It can begin with an id starting with $, such as `"$muffle:lowpass:800:0:0.7"`, which lets the effect be changed or removed later:
* Setting an effect with an id that's already in use changes that effect in place, which is how to adjust an effect every frame without clicks. The new effect must be the same type; lowpass, highpass and bandpass count as one type.
* An id on its own, such as `"$muffle"`, removes every effect whose id starts with it.
* An empty string removes the effect at index, or every effect if index is -1.

Effects run in order, after pitch, and before positioning, volume and pan. Every effect is available on both sounds and mixers. These are the effects, with their parameters in order; parameters in brackets are optional:

### Filters
* `lowpass:frequency:bandwidth:q`, `highpass:frequency:bandwidth:q` and `bandpass:frequency:bandwidth:q`: frequency in Hz. Bandwidth is in octaves and is used if it isn't 0; otherwise q sets how sharp the filter is (0.7 is gentle).

### Volume and movement
* `volume:level`: level from 0 to 1, on the same decibel curve as sound volume (0.8 is -20 dB).
* `lvolume:gain`: a linear gain, where 1 is unchanged.
* `rotate:rate`: carries the sound from side to side and back, rate times a second. A negative rate turns the other way.
* `damp:target:quiet:rate:gain:delay`: automatic gain. Raises quiet audio toward target (0 to 1), never amplifying anything below quiet, adjusting at rate (0 to 1) from a starting gain, and waiting delay seconds of quiet before raising it again.
* `compressor2:gain:threshold:ratio:attack:release`: gain is makeup gain in dB, threshold in dB (-60 to 0), ratio 1 or more, attack and release in milliseconds. Both channels are compressed together.

### Reverbs
* `freeverb:dry:wet:room_size:damping:width[:freeze]`: dry 0 to 1, wet 0 to 3, the rest 0 to 1. A sixth parameter of 1 freezes the reverb's tail.
* `plate:dry:wet:size:decay:damping[:width[:predelay]]`: a plate reverb. Size 0.1 to 2, decay 0 to 0.999, damping and width 0 to 1, predelay in seconds up to 0.25.
* `reverb:input_gain:reverb_mix:reverb_time:hf_ratio`: DirectX's reverb. Gains in dB, time in milliseconds.
* `i3DL2reverb:room:room_hf:rolloff:decay_time:decay_hf_ratio:reflections:reflections_delay:reverb:reverb_delay:diffusion:density:hf_reference`: DirectX's I3DL2 reverb, with levels in millibels and times in seconds.

### Modulation and delay
* `echo4:dry:wet:feedback:delay:stereo`: dry and wet -2 to 2, feedback -1 to 1, delay in seconds up to 6. Stereo 1 makes each repeat cross to the other side.
* `chorus:dry:wet:feedback:min_delay:max_delay:rate`: the delay travels between min_delay and max_delay (milliseconds) at rate milliseconds per second. A sweep of a few milliseconds makes a flanger.
* `phaser:dry:wet:feedback:rate:range:frequency`: rate in cycles per second (0 to 10), range in octaves (0 to 10), sweeping up from frequency (up to 1000 Hz). Equal dry and wet give the deepest notches.
* `autowah:dry:wet:feedback:rate:range:frequency`: the same sweep moving a resonant filter.

### Distortion and pitch
* `distortion:drive:dry:wet:feedback:volume`: drive 0 to 5, dry and wet -5 to 5, volume 0 to 2.
* `pitchshift:factor[:semitones]`: changes pitch without changing length. Factor 2 is an octave up and 0.5 an octave down; semitones are applied on top. It delays the sound by about 37 milliseconds.

The parameters mean what they meant to BASS_FX and DirectX, so effect strings from BGT and the legacy engine keep working, though the sound of each effect is NVGT's own.

## Example:
```
void main() {
	sound s;
	s.load("C:/windows/media/ding.wav");
	s.set_fx("$muffle:lowpass:600:0:0.7");
	s.set_fx("$room:freeverb:1:0.5:0.6:0.5:1");
	s.play_wait();
	s.set_fx("$muffle"); // Remove the filter, keep the reverb.
	s.play_wait();
}
```
