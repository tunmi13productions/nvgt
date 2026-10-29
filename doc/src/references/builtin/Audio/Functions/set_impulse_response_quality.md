# set_impulse_response_quality
Sets how much work convolution reverb may cost, for impulse responses loaded from now on.

`void set_impulse_response_quality(float tail_seconds = 1.5, int rate_divisor = 1);`

## Arguments:
* float tail_seconds = 1.5: responses longer than this are cut to it, with a short fade. 0 keeps them whole.
* int rate_divisor = 1: runs the reverb at the sample rate divided by this (1, 2, 4 and so on), which makes it cheaper and dulls its highest frequencies.

## Remarks:
Convolution costs more the longer the response is, and most of what's heard of a room's reverb is in its first second or two. Calling this with no arguments restores the defaults.
