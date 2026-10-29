# microphone
Records from an input device, for the script to read the samples as they come in.

`microphone(int device = -1, uint sample_rate = 0, uint channels = 0);`

## Arguments:
* int device = -1: the index of the device in [sound_input_devices](#get_sound_input_devices), or -1 for the one sound_input_device is set to (the system default unless changed).
* uint sample_rate = 0, uint channels = 0: the format to record in. 0 uses the device's own; asking for one, such as 48000 Hz mono for opus voice chat, converts to it.

## Methods:
* `float[]@ read(uint frame_count = 0)`: takes up to frame_count frames of interleaved float samples from what has been recorded, or everything waiting if 0.
* `void reset()`: throws away everything waiting to be read.
* `bool start()` and `bool stop()`: starts and stops recording. A new microphone starts recording at once.
* `bool set_device(int device)`: switches to another input device.

## Properties:
* `uint available_read`: how many frames are waiting.
* `uint channels`, `uint sample_rate`: the format being recorded.
* `bool active`: whether it is recording.
* `int device`: the device in use.
* `float volume`: a linear gain applied to what's recorded, where 1 is unchanged.

## Remarks:
If the device can't be opened, creating the microphone raises an exception.

Up to 2 seconds of audio wait to be read. If the script falls further behind than that, the oldest audio is dropped, so reading again always gives what was said most recently.
