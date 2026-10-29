# set_position
Positions the sound relative to a listener.

`bool set_position(float listener_x, float listener_y, float listener_z, float sound_x, float sound_y, float sound_z, float rotation = 0.0, float pan_step = 1.0, float volume_step = 1.0);`

## Arguments:
* float listener_x, listener_y, listener_z: where the listener is.
* float sound_x, sound_y, sound_z: where the sound is.
* float rotation = 0.0: the direction the listener faces, in radians, turning clockwise from north (positive y).
* float pan_step = 1.0: how far the sound pans per unit of distance to the side.
* float volume_step = 1.0: how many decibels quieter the sound gets per unit of distance.

## Returns:
bool: true if the position was set, false otherwise.

## Remarks:
x runs east, y north and z up. This works on mixers too, positioning everything in them as one.

When [sound_global_hrtf](#sound_global_hrtf) is on and the sound's set_hrtf(true) was called, Steam Audio places the sound with HRTF, so it can be heard above, below and behind. Otherwise it is panned, and volume falls with distance on the curve pan_step and volume_step describe.

A sound created by a [sound_environment](#sound_environment) is positioned the same way, but its listener is the environment's, so it is heard through the environment's walls and rooms.
