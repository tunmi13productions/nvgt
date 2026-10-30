# remove_send
Stops sending a sound or mixer to a mixer.

`bool remove_send(mixer@ target = null);`

## Arguments:
* mixer@ target = null: the mixer to stop sending to. null removes every send.

## Returns:
bool: true if anything was removed, false if there was no such send.

## Remarks:
See [set_send](#set_send).
