# log_path
The full path of the log file currently being written, or an empty string when no log is open.

`string log_path;`

## Remarks:
Read this after `log_start` to find out where the log really went. The requested name is only a preference, and an unwritable directory sends the log to the user's preferences directory instead.

This is also the path a crash report is written to, so it is the file to ask a player for.
