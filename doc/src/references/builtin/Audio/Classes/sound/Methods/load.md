# load
Loads a sound from a file, from memory, or through script callbacks.

1. `bool sound::load(const string&in filename, const pack_interface@ pack = sound_default_pack, bool allow_preloads = true);`
2. `bool sound::load(sound_close_callback@ close_cb, sound_length_callback@ length_cb, sound_read_callback@ read_cb, sound_seek_callback@ seek_cb, const string&in user_data, const string&in preload_filename = "");`
3. `bool sound::load(string& data, uint size, const string&in preload_filename = "", bool legacy_encrypt = false);`

## Arguments (1):
* const string&in filename: the name or path of the file to load. A URL (http, https or ftp) is streamed as [load_url](#load_url) would.
* const pack_interface@ pack = sound_default_pack: the pack to load from, either a pack_file or a legacy pack. If it is null or not open, the file is loaded from disk.
* bool allow_preloads = true: whether a copy of this file already decoded for another sound may be reused.

## Arguments (2):
* sound_close_callback@ close_cb: called when the sound is done with the data.
* sound_length_callback@ length_cb: returns how many bytes of data there are.
* sound_read_callback@ read_cb: fills its buffer with up to length bytes, and returns how many it gave.
* sound_seek_callback@ seek_cb: moves to the given byte offset.
* const string&in user_data: passed to every callback.
* const string&in preload_filename = "": if given, the decoded audio is kept under this name, and a later load with the same name reuses it instead of calling the callbacks again.

## Arguments (3):
* string& data: an encoded file held in memory.
* uint size: how many bytes of data to use.
* const string&in preload_filename = "": if given, the decoded audio is kept under this name for reuse, as above.
* bool legacy_encrypt = false: whether the data is encrypted with BGT's legacy sound encryption.

## Returns:
bool: true if the sound was loaded, false otherwise.

## Remarks:
Files up to 30 seconds long are decoded completely when loaded; longer ones are decoded as they play. To always decode as it plays, use [stream](#stream).

The callbacks are all called during load, on the calling thread, and read the data in full. Their signatures are:
> void sound_close_callback(string user_data);
> uint sound_length_callback(string user_data);
> int sound_read_callback(string &out data, uint length, string user_data);
> bool sound_seek_callback(uint offset, string user_data);
