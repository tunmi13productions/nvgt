# mixer
Groups sounds, and other mixers, so they can be controlled together.

`mixer();`

## Remarks:
Send a sound or mixer through a mixer with its set_mixer method. The mixer's volume, pan and pitch then apply on top of the sound's own, its effects (see set_fx) process everything in it as one, and positioning it with set_position moves everything in it together. Mixers can be nested as deeply as needed, but a mixer can't be put inside itself or one of its own children.

A mixer has the same volume, pan, pitch, slide, set_fx, set_position, set_hrtf, set_mixer and set_send members as a sound, plus [set_impulse_response](#set_impulse_response) for convolution reverb.

A sound plays through one mixer, but it can also send copies of itself to others with [set_send](#set_send). That's how one reverb mixer can serve many sounds, each sending as much of itself into the room as it should.

A new mixer plays through [sound_default_mixer](#sound_default_mixer).

## Example:
```
void main() {
	mixer ambience;
	ambience.volume = -10;
	ambience.set_fx("lowpass:2000:0:0.7"); // Everything in this mixer sounds muffled.
	sound rain;
	rain.load("rain.ogg");
	rain.set_mixer(ambience);
	rain.play_looped();
	wait(3000);
	ambience.slide_volume(-100, 2000); // Fade out everything in the mixer at once.
	wait(2000);
}
```
