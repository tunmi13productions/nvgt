# set_log_channel_level
Sets the severity threshold for one named channel, leaving every other channel alone.

`void set_log_channel_level(string channel, log_level level);`

## Arguments:
* string channel: the channel name, for example "nvgt.sound" or "nvgt.sound.miniaudio".
* log_level level: the lowest severity that channel should record.

## Remarks:
Channel names are hierarchical and separated with dots, so setting a level on "nvgt.sound" also applies to "nvgt.sound.miniaudio" unless that channel has a level of its own.

NVGT writes to these channels: "nvgt" for general engine errors, "nvgt.compiler" for compilation diagnostics, "nvgt.script" for unhandled exceptions, "nvgt.sound" for the sound system and "nvgt.sound.miniaudio" for miniaudio's own output. Your own script messages go to "app" unless you make a `logger` with a different name.

Turning "nvgt.sound.miniaudio" down to LOG_WARN is worth doing if you run the whole log at LOG_DEBUG, since miniaudio describes every device it examines.
