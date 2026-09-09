# log_info
Writes a message to the log at the LOG_INFO level, meaning a normal event worth a record.

`void log_info(string message);`

## Arguments:
* string message: the text to record.

## Remarks:
The script file and line you called from are recorded with the message, so a log line points back at the code that produced it.

Nothing happens if no log is running or if the level is below the current threshold.
