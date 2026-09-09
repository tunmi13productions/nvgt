# level
The lowest severity this channel records.

`log_level logger::level;`

## Remarks:
Setting this is the same as calling `set_log_channel_level` with this logger's name. Channels below it in the hierarchy inherit the level unless they set one of their own.
