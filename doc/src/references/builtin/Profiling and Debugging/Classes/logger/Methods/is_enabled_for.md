# is_enabled_for
Asks whether a message at this level would actually be recorded.

`bool logger::is_enabled_for(log_level level);`

## Arguments:
* log_level level: the severity to test.

## Returns:
bool: true if a message at that level would be written.

## Remarks:
Worth checking before building an expensive string that nothing is going to read.
