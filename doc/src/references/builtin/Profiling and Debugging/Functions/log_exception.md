# log_exception
Writes an error to the log along with the script call stack that produced it.

`void log_exception(string message);`

## Arguments:
* string message: the text to record.

## Remarks:
This is the one to reach for inside a catch block. It records where the failure actually happened rather than just that it happened.
