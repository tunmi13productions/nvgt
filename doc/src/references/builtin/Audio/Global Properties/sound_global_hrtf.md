# sound_global_hrtf
Whether positioned sounds can use Steam Audio's HRTF.

`bool sound_global_hrtf;`

## Remarks:
It is true by default. A sound or mixer is placed with HRTF only while this is on and its own set_hrtf(true) has been called; otherwise positioned sounds are simply panned. A [sound_environment](#sound_environment) needs it on.

Changes take effect at once. If Steam Audio can't be loaded, sounds are panned as if this were off.
