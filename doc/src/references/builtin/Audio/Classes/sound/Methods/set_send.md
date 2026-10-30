# set_send
Sends a copy of a sound or mixer into another mixer, as well as through its own.

`bool set_send(mixer@ target, float volume = 0, bool after_distance = false);`

## Arguments:
* mixer@ target: the mixer to send to.
* float volume = 0: the level of the copy in decibels, from -100 to 0, on top of the sound's own volume.
* bool after_distance = false: whether the copy is also made quieter by distance, as set by set_position.

## Returns:
bool: true if the send was added or changed, false otherwise, for example if it would feed back into the sound or mixer it comes from.

## Remarks:
Where set_mixer decides where a sound plays, a send plays it somewhere else as well. The usual use is a shared reverb: many sounds each send some of themselves to one mixer carrying a reverb with no dry signal, so one reverb serves them all, and each sound's own send level decides how much of it reaches the room.

Calling set_send again with the same mixer changes that send rather than adding another, and the change is smoothed, so a send can be adjusted every frame, for example to follow the sound's distance from the listener. A sound or mixer can have any number of sends, to different mixers.

The copy is taken after the sound's effects, volume and pan, including the pan from set_position. By default it isn't made quieter with distance, so that the game can choose how the reverb falls off itself; set after_distance to true to have it follow the distance attenuation as the sound does.

What a mixer receives through sends is mixed in before its effects, so its reverb, filters and impulse response process it, and it arrives one block (256 frames, about 5 milliseconds) after the sound's own output. A send that would let audio return to where it came from, such as from a mixer to one of its own children or around a chain of sends, is refused, as is moving a mixer with set_mixer where that would do the same.

A send keeps its mixer alive, even after the script has let go of it, until the send is removed with [remove_send](#remove_send) or the sound or mixer it's on is destroyed. [get_send](#get_send) reads a send's volume back.

## Example:
```
void main() {
	mixer room;
	room.set_fx("freeverb:0:1:0.7:0.5:1"); // The room's reverb, with no dry signal of its own.
	sound s;
	s.load("C:/windows/media/ding.wav");
	s.set_send(room, -6);
	s.play_wait(); // The ding, and the room.
	s.set_send(room, -20);
	s.play_wait(); // Further from the room's walls.
	s.remove_send(room);
	s.play_wait(); // Dry.
	wait(1500);
}
```
