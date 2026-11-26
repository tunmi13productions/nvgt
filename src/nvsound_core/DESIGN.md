# nvsound core: design

A new core for nvsound, NVGT's sound engine. It keeps everything that decides what a sound sounds like, and replaces how the engine is wired to its threads. Golden Crayon 2.0 binds it to Python, and NVGT can use it in place of today's core.

## Why

Today's core renders the whole graph on the audio thread while holding `g_graph_mutex`. The script thread takes the same lock to create, change, move and destroy sounds. Around it are 86 mutexes, over 100 places that take locks, and a spinlock per sound for its parameters. That causes three kinds of trouble:

- **Glitches.** Whenever the script thread holds the lock too long (loading, tearing down a map, destroying a mixer), the audio thread misses its deadline, and you hear a click or a dropout.
- **Freezes.** Two threads each holding a lock the other wants both stop. `open_output()` already has a comment about one of these, with SDL's stream.
- **Races the lock doesn't cover.** `play()`, `stop()` and `seek()` move a source's read position from the script thread while the audio thread may be reading it. `reset_phonon_state()` touches Steam Audio state the audio thread is using.

## What stays exactly as it is

The code that decides the sound. It's taken over unchanged, and the reference renders (Golden Crayon's `client_py/tests/audio_reference/`) check that the result is the same sample for sample.

- `nvsound_effects.cpp`: every `set_fx` effect and the `set_fx` string rules.
- `nvsound_convolver.cpp`: the convolution reverb.
- `nvsound_phonon.cpp`: HRTF through Steam Audio.
- `nvsound_decode.cpp` and `nvsound_audio_decoder.cpp`: the decoders and resampling.
- The math in today's `nvsound.cpp`: `slider`, `spatialize()` (the legacy pan and volume steps), `stereo_gains()`, `apply_gain()`'s ramps, sends one block late, a pitched mixer resampling its children, and the order of every stage in a node's render. They're moved, not rewritten.

## The one rule

**The audio thread never waits and never allocates.** It takes no lock, it doesn't touch the heap, and it calls nothing that might. Everything else in this design follows from that.

## Threads

- **Script threads** (the game, or Python): own the API objects that scripts hold (`sound`, `mixer`). They build anything that needs memory, then hand it over.
- **The audio thread:** SDL's device callback, or the script itself in offline mode. It alone owns the graph: nodes, parents and children, effect chains, sends, sources and Steam Audio state.
- **Workers:** the stream feeder, and loaders for files and impulse responses. They decode, and they hand results over the same way the script does.

## How the two sides talk

1. **Commands, script to audio.** Each command is a small, trivially destructible struct: a type, a node pointer, a few numbers, and at most one pointer to something already built. Producers append to a pending vector under a mutex that only producers ever wait on. At the start of each block, the audio thread `try_lock`s it and swaps the vector with its own empty one, whose capacity stays put. If the lock is busy, the commands wait one block. The audio thread never waits.
2. **Garbage, audio to script.** Anything the audio thread is finished with goes into a fixed-size, lock-free single-producer ring, and a script thread deletes it later. That covers a removed node, an old source, a replaced effect and an old impulse response. Nothing is ever freed on the audio thread. The ring is preallocated, and if it's ever full, the item waits in a preallocated overflow list for the next block.
3. **Status, audio to script.** Each node shares a status block with its API object, made of atomics: playing, ended, position in frames, sliding flags and length. Getters read these, so asking never waits either.

Commands are applied at the start of a block, before rendering it. A change made between two blocks takes effect at the next block, which is what happens today. In offline mode the script runs between blocks, so the order is the same as today, and renders match exactly.

## Objects

- **The API object** (`sound`, `mixer`) is what scripts hold and reference count. It keeps the values scripts read back (volume, pan, pitch, the loaded filename and the effect list as text), so a getter answers what the script last set, as now. It owns its node until it hands the node to the audio thread with an "add" command, and from then on it only sends commands about it.
- **The node** belongs to the audio thread and is only touched there. When the API object's last reference goes, it sends "destroy". The audio thread detaches the node, re-parents a mixer's children to the master as today, and retires it to the garbage ring.
- **`sound_play()`'s fire-and-forget sounds** stay alive until their status says ended, as today.

## What moves to the audio thread

- `play`, `pause`, `stop` and `close` change the playing and ended flags by command. Moving a source's read position (`seek`, the restart in `play`, `stop`'s rewind, `set_loop`) stays on the script thread, as today. The sources were built for that: the PCM source with atomics, where the script's seek wins, and streams with locks only the script and feeder sides wait on. Keeping it there matches today exactly and keeps decoder seeks off the audio thread.
- Volume, pan and pitch, set or slid, become commands. A slide starts on the frame clock when it's applied, which offline is the same frame as today.
- `set_mixer`, sends, `set_position`, `set_hrtf` and `dry_volume` become commands. The checks that refuse loops (`reaches()`, `is_ancestor_of()`) run on the script side against a mirror of the graph's shape that the script side keeps. The audio thread applies only changes that already passed.
- `reset_phonon_state()` becomes a command.

## Effects

- **A new effect** is built and configured on the script thread, then handed over in a command, ready to insert at its index. The audio thread just links it in.
- **Reconfiguring an effect in place** (the same `$id`) keeps its state, so a filter moved every frame doesn't click. Most effects only recompute numbers when reconfigured, and for those the parsed arguments go in the command and `configure()` runs on the audio thread. DirectX reverb, echo and chorus reallocate their delay lines when their size changes. For those, the script side builds a configured copy instead, and the audio thread swaps it in and retires the old one. That's the one place behavior can differ from today, and only for those three effects when their delay size changes. Golden Crayon uses none of them.
- **Removing** an effect is a command, and the removed effect goes to garbage.

## Convolution

The response is built on a worker or the script thread. The convolver's `set_ir()` splits into "build a state" and "swap it in". The swap is a command, and the old state goes to garbage. The convolver's own mutex, which the audio thread waits on today for that swap, goes away.

## Streams

The feeder's ring becomes a lock-free ring with one writer (whoever feeds, under the decoder's mutex, which only feeders and the script take) and one reader (the audio thread). A seek can't reset a ring the audio thread may be reading, so it fills a fresh ring and hands it over, and the audio thread switches to it at its next read and retires the old one. The new ring is filled straight away, as today's is after a seek. In offline mode `feed_streams_now()` still tops every stream up before each block, so renders stay exact.

Push sources (voice chat, speech, `push_memory` with a sample rate) get the same treatment: chunks in a linked queue only producers lock, walked by the audio thread, which retires the chunks it finishes. `clear()` becomes a request the audio thread carries out, fading out what was waiting over the fade length and dropping the rest.

## The device

There's one output stream at a time. Changing device destroys the old stream (SDL waits for its callback to finish) before the new one opens. No lock is held during that wait, so the deadlock in today's `open_output()` can't happen. Losing a device, or the system default changing, is SDL's job, since a stream on the default device follows it.

## Offline mode

The same as on the fork's `nvsound-offline-render` branch: `render_block()` applies the pending commands, renders and advances the clock. NVGT's virtual clock and `feed_streams_now()` carry over unchanged.

## Room for simulated acoustics later

Golden Crayon will later add Steam Audio simulation (ray-traced reflections, transmission, pathing) as a mode maps opt into. The core leaves room for it:
- Per-node simulation results arrive the same way commands do, from the simulation thread, as values the audio thread applies at a block boundary.
- Steam Audio's direct and reflection effects are stages a node can carry, like `set_fx` effects.

`sound_environment` isn't part of the first version. Golden Crayon doesn't use it, and the simulated mode will redesign it around this core.

## The host

Everything the core needs from whoever embeds it is one `host` interface: cache keys and opening files (NVGT's packs and decryption, or plain files), URL streams, BASS era decryption, `wait()`, error logging, and the clock that follows the audio in offline mode. NVGT's is in `nvgt_adapter.cpp`, and Golden Crayon's Python module will have its own.

## Bindings

- **C++ API:** the `nvsound` namespace, with the classes and functions NVGT registers today, so the NVGT adapter is thin.
- **NVGT adapter:** the AngelScript registration, kept the same as today, so the same `.nvgt` scripts run on both cores. That's how the reference renders compare them, including the map walks through 1.x's own code.
- **Python:** nanobind, with the same names, for Golden Crayon 2.0.

## Order of work

1. The command queue, the garbage ring and the status block, with unit tests that check the audio side never takes a lock or allocates. Done.
2. Nodes, mixers, the render path, and parameters and slides, moved from today's code. The engine scenes `dry`, `pan`, `volume`, `pitch`, `slides` and `position` must match. Done: NVGT builds on the new core with `nvsound_core=1`, and those six renders are byte for byte the same as today's core.
3. Effects, convolution, HRTF and sends. `lowpass`, `cuts`, `freeverb`, `plate`, `convolution`, `hrtf` and `sends` must match. Done: all 20 references, the five map walks included, are byte for byte the same as today's core. A checking build (`nvsound_count_allocs=1`, with `NVSOUND_REPORT` set) counted no locks and no allocations on the audio thread across all of them. Its lock count only sees the core's own mutex type. The stream ring's `std::mutex`, which the audio thread still takes to read a stream, is step 4.
4. Streams and the lock-free ring. `stream` and `crowd` must match. Done, with push sources too: both have lock-free versions in `nvsound_decode.cpp` that only the new core asks for (`use_lockfree_sources`), so today's core keeps its own. A seek fills a fresh ring and hands it over. Push chunks go in a queue only producers lock, and `clear()`'s fade is done by the audio thread. A `push` scene joined the references. All 21 match, and the checking build, whose allocation counter now proves itself at startup, counted no locks or allocations on the audio thread. `clear()` isn't covered by a reference, since scripts can't call it. Speech uses it.
5. The NVGT adapter, so all 20 references can run, the map walks included. Started early, in step 2, since it's how the renders are compared.
6. The device and microphone, and the failures in the error logs: switching or losing devices, the Default device, an impulse response loading while sound plays, many notes at once, and sounds piling up across map changes. Done:
   - Opening and closing the device is serialised by a mutex only script threads and the janitor take. A chosen device that disappears moves sound to the default device instead of leaving it silent. SDL's removal event raises a flag, and the janitor reopens the stream. `device_loss.nvgt` checks it on the checking build by posting that event while sound plays.
   - Both cores time every device callback against its budget (`nvsound_timing.h`), printed at shutdown with `NVSOUND_REPORT`. The new core also counts the sounds and mixers that exist.
   - `stress.nvgt` and `stress.py` run two loads in real time on SDL's dummy driver with 10 ms buffers. "busy" is 48 moving sources through three reverbs and a convolution room, a map change every second, an impulse response loaded every 0.7 s and 24 notes every quarter second. "churn" is about 23,000 sounds made and destroyed a second, mixers torn down with children in them, and every filter and send changed every pass.
   - What they showed, on this machine. Under churn, neither core ever ran over budget, and both cores' worst callback was 1 to 2 ms of 10. Under busy, both ran over on about 1 to 1.5 percent of callbacks, about equally, because that's simply a lot to render, not because of the lock. Today's core's lock isn't what causes dropouts under these loads. The new core's gains are what it rules out rather than anything measured here: it can't deadlock on a lock it doesn't have, it has no races between the script and the audio thread, and it survives losing its device. No sounds piled up: after 470,000 made and destroyed, one node remained.
   - The microphone is unchanged. It runs on SDL's own capture thread, not the render path, and holds a lock only to copy captured audio.
7. nanobind.
