# sound
Plays one piece of audio: a file, a URL, data from memory, or samples the script pushes to it as it goes.

`sound();`

## Remarks:
A new sound plays through [sound_default_mixer](#sound_default_mixer). Use set_mixer to send it through a different [mixer](#mixer).

Volume is in decibels, from -100 (silent) to 0 (full), on the same curve BGT used. Pan runs from -100 (left) to 100 (right), and pitch is a percentage, from 5 to 500, where 100 is the original speed. Changing pitch changes speed along with it, as playing a tape faster would; to change pitch alone, use the `pitchshift` effect with [set_fx](#set_fx).

A sound that is let go of while playing stops. To play a sound and forget about it, use [sound_play](#sound_play).
