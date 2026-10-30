# dry_volume
The volume of a sound's or mixer's own output, apart from its sends.

`float dry_volume;`

## Remarks:
In decibels, from -100 to 0, on top of volume. Unlike volume, it doesn't change what the sound sends to other mixers with [set_send](#set_send), so a sound at a dry_volume of -100 is heard only through its sends. That suits a convolution reverb made from a real recording, which already contains the sound's own direct arrival: playing the dry sound as well would double it.

## Example:
```
void main() {
	mixer hall;
	hall.set_impulse_response("hall.wav", 0.0, 1.0);
	sound s;
	s.load("C:/windows/media/ding.wav");
	s.set_send(hall);
	s.dry_volume = -100; // Only the hall is heard.
	s.play_wait();
	wait(1500);
}
```
