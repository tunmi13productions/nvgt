# sound_output_device and sound_input_device
Which devices sound plays through and microphones record from.

1. `uint sound_output_device;`
2. `uint sound_input_device;`

## Remarks:
sound_output_device is an index into [get_sound_output_devices](#get_sound_output_devices): 0 is no sound, 1 follows the system default (the default setting), and real devices start at 2. Sounds already playing move to the new device.

sound_input_device is an index into [get_sound_input_devices](#get_sound_input_devices), or 0xffffffff (the default) to follow the system default. It applies to microphones opened afterwards.
