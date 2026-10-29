# sound_environment
A 3D space with walls and materials, which Steam Audio uses to simulate how sounds in it are heard: how loud they are with distance, how much a wall muffles them, and how a room makes them ring.

`sound_environment();`

## Methods:
* `sound@ new_sound()`: a sound placed in this environment. Load and play it as usual, and position it with set_position.
* `mixer@ new_mixer()`: a mixer placed in this environment, positioning everything in it as one.
* `bool add_box(const string&in material, float minx, float maxx, float miny, float maxy, float minz, float maxz)`: adds a solid box of the given material, such as a wall or a floor.
* `bool add_material(const string&in name, float absorption_low, float absorption_mid, float absorption_high, float scattering, float transmission_low, float transmission_mid, float transmission_high, bool replace_if_existing = false)`: defines a material. Absorption is how much sound its surface soaks up, transmission how much passes through it, each at low, mid and high frequencies from 0 to 1.
* `void set_listener(float x, float y, float z, float rotation)`: where the listener is and which way they face, in radians clockwise from north.
* `void update()`: asks for the simulation to catch up with recent changes now rather than on its next pass.

## Remarks:
These materials exist from the start: air, generic, brick, concrete, ceramic, gravel, carpet, glass, plaster, wood, metal and rock.

The simulation runs on a thread of its own, so moving sounds and the listener never waits for it. The direct path (distance, occlusion and what passes through walls) updates about every 10 milliseconds, and room reverb about 20 times a second.

Walls muffle rather than silence what's behind them: thin wood or glass lets a fair amount through, and thick concrete very little.

Sounds in an environment need [sound_global_hrtf](#sound_global_hrtf) to be on.

## Example:
```
void main() {
	sound_environment env;
	env.set_listener(0, 0, 0, 0);
	env.add_box("concrete", 3, 4, -10, 10, 0, 5); // A wall to the east.
	sound@ s = env.new_sound();
	s.load("radio.ogg");
	s.set_position(0, 0, 0, 8, 0, 0); // Behind the wall, so it is heard muffled.
	s.play_looped();
	wait(5000);
}
```
