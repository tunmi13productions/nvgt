# get_send
Returns the volume of a sound's or mixer's send to a mixer.

`float get_send(mixer@ target) const;`

## Arguments:
* mixer@ target: the mixer the send goes to.

## Returns:
float: the send's volume in decibels, or -100 if there is no send to that mixer.

## Remarks:
See [set_send](#set_send).
