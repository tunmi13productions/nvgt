# logging_level
The lowest severity the log currently records, which can be changed while the game runs.

`log_level logging_level;`

## Remarks:
Setting this affects every channel that does not have a level of its own. Use `set_log_channel_level` to single one out.
