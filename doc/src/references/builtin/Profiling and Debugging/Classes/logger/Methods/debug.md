# debug
Writes a message to this logger's channel, meaning detail that only matters while you are chasing a problem.

`void logger::debug(string message);`

## Arguments:
* string message: the text to record.

## Remarks:
The script file and line you called from are recorded alongside the message.
