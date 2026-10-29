# push_memory
Feeds audio to the sound a piece at a time, for audio the script produces or receives as it goes, such as voice chat.

1. `bool sound::push_memory(const string&in data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0);`
2. `bool sound::push_memory(const float[]@ data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0);`

## Arguments:
* data: the next piece of audio. As a string it is either raw 32 bit float samples or part of an encoded file, as explained below; as an array it is float samples.
* bool end_stream = false: true once nothing more will be pushed, so the sound can end when it has played everything.
* int pcm_rate = 0: the sample rate of raw samples. Only the first push needs it.
* int pcm_channels = 0: the number of channels in raw samples, interleaved. Only the first push needs it.

## Returns:
bool: true if the data was accepted, false otherwise.

## Remarks:
If the first push gives a sample rate, the sound takes raw samples. If it doesn't, the data is an encoded file such as an ogg, pushed in pieces, and the first push must contain enough of the file (at least 768 bytes) for its format to be recognized.

Pushing never blocks and nothing pushed is dropped. While the sound has played everything it was given, it plays silence until more arrives. Its length is however much audio is waiting to be played.

Pushing into a sound that has a file loaded closes the file and starts a new push stream.
