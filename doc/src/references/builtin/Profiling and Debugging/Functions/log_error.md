# log_error
Writes a message to the log at the LOG_ERROR level, meaning something failed.

`void log_error(string message);`

## Arguments:
* string message: the text to record.

## Remarks:
The script file and line you called from are recorded with the message, so a log line points back at the code that produced it.

Nothing happens if no log is running or if the level is below the current threshold.
