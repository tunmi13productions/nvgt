# set_impulse_response
Puts everything in the mixer through convolution reverb, using a recording of a real space or device.

`bool mixer::set_impulse_response(const string&in response, float dry = 1.0, float wet = 0.35, const string&in response_right = "", float spread = 1.0);`

## Arguments:
* const string&in response: the impulse response file, loaded like any sound (from sound_default_pack if set). An empty string turns the reverb off.
* float dry = 1.0: the level of the original sound, as a linear gain.
* float wet = 0.35: the level of the reverb, as a linear gain.
* const string&in response_right = "": a second mono file for the right channel, making a stereo response from two mono ones.
* float spread = 1.0: how wide the reverb is. 1 follows the sound's pan, 0 keeps the reverb centered wherever the sound is, and -1 swaps it to the other side.

## Returns:
bool: true if the response was loaded (or the reverb turned off), false otherwise.

## Remarks:
Impulse responses aren't made louder or quieter when they are loaded, so the right wet level depends on the recording. Most are saved as loud as they can be, which makes them much louder than the sound going into them, often by 10 to 25 dB; start with a low wet level and raise it.

`bool mixer::set_impulse_response_params(float dry, float wet, float spread = 1.0);` changes dry, wet and spread without loading the response again, and is cheap enough to call every frame.

Long responses cost more to run, so they are cut to 1.5 seconds by default. [set_impulse_response_quality](#set_impulse_response_quality) changes that for responses loaded afterwards.
