# exception
Writes an error along with the script call stack that produced it.

`void logger::exception(string message);`

## Arguments:
* string message: the text to record.

## Remarks:
Use this inside a catch block, where the call stack is the part worth keeping.
