# set_hrtf
Chooses whether this sound or mixer is placed with Steam Audio's HRTF when it is positioned.

`void set_hrtf(bool enable = true);`

## Arguments:
* bool enable = true: whether to use HRTF.

## Remarks:
HRTF only applies while [sound_global_hrtf](#sound_global_hrtf) is also on, and only once the sound has been positioned with [set_position](#set_position). Sounds and mixers made by a [sound_environment](#sound_environment) have it on already.
