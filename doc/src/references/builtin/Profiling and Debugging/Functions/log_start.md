# log_start
Starts writing a log file, in the manner of Python's logging module.

`bool log_start(string filename = "errors.log", log_level level = LOG_WARN);`

## Arguments:
* string filename = "errors.log": the file to write to. A relative name is resolved against the directory of your script or your compiled game.
* log_level level = LOG_WARN: the lowest severity worth recording.

## Returns:
bool: true if the log was opened, false if no writable location could be found.

## Remarks:
Everything NVGT itself reports goes into the same file, so a log started this way also collects sound loading failures, miniaudio's own complaints, compilation errors and unhandled exceptions.

If the directory holding your program cannot be written to, which is normal for a game installed under Program Files or inside a macOS bundle, the log falls back to the user's preferences directory. Read `log_path` to find out where it actually landed.

You do not have to call this at all to get crash reports. A crash is always recorded, see the "Error logs and crash reports" topic.
