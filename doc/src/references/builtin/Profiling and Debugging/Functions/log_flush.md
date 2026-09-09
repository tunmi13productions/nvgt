# log_flush
Pushes anything still buffered out to the log file.

`void log_flush();`

## Remarks:
Each message is flushed as it is written, so this is rarely needed. It exists for the moment before you deliberately do something that might not come back.
