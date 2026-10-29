# audio_decoder
Reads the raw samples out of an audio file or stream, without playing anything.

`audio_decoder();`

## Methods:
* `bool open(const string&in filename, const pack_interface@ pack_file = sound_default_pack, uint sample_rate = 0, uint channels = 0)`: opens a file. A sample_rate or channels of 0 keeps the file's own; anything else converts to it.
* `bool open(datastream@ stream, uint sample_rate = 0, uint channels = 0)`: decodes a datastream. A stream that can't seek, such as an internet response, is decoded as it is read.
* `float[]@ read(uint64 frame_count = 0)`: the next frame_count frames as interleaved float samples, fewer at the end, and none once it's over. 0 reads advised_read_frame_count frames.
* `bool seek_frames(uint64 frame_index)` and `bool seek_milliseconds(float ms)`: moves to a point in the audio, if the source can seek.
* `uint64 skip_frames(uint64 frame_count)` and `float skip_milliseconds(float ms)`: moves forward, reading through the audio if the source can't seek. Returns how far it moved.
* `bool set_looping(bool looping)`: whether reading starts over at the end.
* `bool close()`: closes the file or stream.

## Properties:
* `uint channels`, `uint sample_rate`: the format read gives back.
* `uint64 cursor_frames`, `float cursor_milliseconds`: how far reading has got.
* `uint64 length_frames`, `float length_milliseconds`: the length, or 0 when it isn't known, as for a live stream.
* `bool active`: whether there is more to read.
* `bool looping`, `uint advised_read_frame_count`.

## Example:
```
void main() {
	audio_decoder d;
	if (!d.open("music.ogg", sample_rate: 48000, channels: 1)) return;
	float loudest = 0;
	float[]@ samples;
	while ((@samples = d.read(4096)).length() > 0) {
		for (uint i = 0; i < samples.length(); i++) {
			if (abs(samples[i]) > loudest) loudest = abs(samples[i]);
		}
	}
	alert("peak", "The loudest sample is " + loudest);
}
```
