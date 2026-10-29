# sound_netstream_buffered and sound_netstream_buffer_size
How far sounds streaming from a URL read ahead of what is playing.

1. `uint sound_netstream_buffered;`
2. `uint sound_netstream_buffer_size;`

## Remarks:
sound_netstream_buffered is how many bytes the most recently active URL stream has waiting, read only. sound_netstream_buffer_size is the most a stream will read ahead, in bytes, 512 KB by default, and applies to streams opened afterwards.

A live stream is sent at about the speed it plays, so a bigger buffer only helps if the server sends faster than that. If sound_netstream_buffered stays near zero, raising the size won't change anything. Watching it is a good way to wait for a stream to build up some headroom before playing it.
