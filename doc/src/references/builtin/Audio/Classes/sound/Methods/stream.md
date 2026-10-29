# stream
Opens a file to be decoded as it plays, rather than all at once.

`bool sound::stream(const string&in filename, const pack_interface@ pack = sound_default_pack);`

## Arguments:
* const string&in filename: the name or path of the file to stream.
* const pack_interface@ pack = sound_default_pack: the pack to stream from, either a pack_file or a legacy pack. If it is null or not open, the file is read from disk.

## Returns:
bool: true if the file was opened and its format recognized, false otherwise.

## Remarks:
Streaming suits long files such as music, which would take a lot of memory to keep decoded. [load](#load) already streams any file longer than 30 seconds, so this is only needed to stream a shorter one.
