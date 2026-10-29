# set_mixer
Sends a sound or mixer through another mixer.

`bool set_mixer(mixer@ mixer = null);`

## Arguments:
* mixer@ mixer = null: the mixer to play through. null sends it straight to the output, past sound_default_mixer.

## Returns:
bool: true if the mixer was changed, false otherwise, for example if a mixer was set as its own parent or ancestor.
