# volume, pan and pitch
The basic controls of a sound or mixer.

1. `float volume;`
2. `float pan;`
3. `float pitch;`

## Remarks:
* volume: in decibels, from -100 (silent) to 0 (full volume).
* pan: from -100 (left) to 100 (right), 0 being centered. Panning turns the far side down by a decibel per step, as BGT did.
* pitch: a percentage from 5 to 500, where 100 is the original speed. Speed changes along with pitch; for pitch alone, use the pitchshift effect with set_fx.

On a mixer these apply on top of the settings of everything in it, so a mixer at pitch 200 plays all its sounds twice as fast.
