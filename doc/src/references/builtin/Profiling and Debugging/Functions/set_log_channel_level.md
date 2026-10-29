# set_log_channel_level
Sets the severity threshold for one named channel, leaving every other channel alone.

`void set_log_channel_level(string channel, log_level level);`

## Arguments:
* string channel: the channel name, for example "nvgt.sound".
* log_level level: the lowest severity that channel should record.

## Remarks:
Channel names are hierarchical and separated with dots, so setting a level on "nvgt" also applies to "nvgt.sound" unless that channel has a level of its own.

NVGT writes to these channels: "nvgt" for general engine errors, "nvgt.compiler" for compilation diagnostics, "nvgt.script" for unhandled exceptions, and "nvgt.sound" for the sound system. Your own script messages go to "app" unless you make a `logger` with a different name.
