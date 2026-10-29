# sound_play
Loads and plays a sound in one call, keeping it alive until it finishes even if the script doesn't hold on to it.

`sound@ sound_play(const string&in path, const vector&in position = vector(FLOAT_MAX, FLOAT_MAX, FLOAT_MAX), float volume = 0.0, float pan = 0.0, float pitch = 100.0, mixer@ mix = null, const pack_interface@ pack_file = sound_default_pack, bool autoplay = true);`

## Arguments:
* const string&in path: the file to play.
* const vector&in position: where to place the sound, relative to a listener at the origin facing north, as set_position would. Leave it out for a sound that isn't positioned.
* float volume = 0.0, float pan = 0.0, float pitch = 100.0: the sound's settings.
* mixer@ mix = null: the mixer to play through, or null for sound_default_mixer.
* const pack_interface@ pack_file = sound_default_pack: the pack to load from.
* bool autoplay = true: whether to start playing at once.

## Returns:
sound@: the sound, or null if it couldn't be loaded.

## Remarks:
This suits menu clicks, footsteps and other one-shot sounds. The handle can be kept to change the sound while it plays, or ignored.

A sound made with autoplay false is only kept alive by its handle; if the script lets go of it without playing it, it is released.

## Example:
```
void main() {
	sound_play("C:/windows/media/ding.wav");
	sound_play("C:/windows/media/chord.wav", pitch: 150, volume: -6);
	wait(1500);
}
```
