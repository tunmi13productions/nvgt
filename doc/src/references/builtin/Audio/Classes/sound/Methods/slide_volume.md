# slide_volume
Moves the volume of a sound or mixer smoothly to a new value, over a given time.

`bool slide_volume(float volume, uint time);`

## Arguments:
* float volume: the volume to end at, in decibels (-100 to 0).
* uint time: how long the slide takes, in milliseconds.

## Returns:
bool: true if the slide started, false otherwise.

## Remarks:
slide_pan and slide_pitch work the same way for pan and pitch. Setting the property directly cancels a slide in progress, and the sliding, volume_sliding, pan_sliding and pitch_sliding properties tell whether one is still running.
