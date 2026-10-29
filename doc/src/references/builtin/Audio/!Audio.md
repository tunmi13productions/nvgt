# Audio
NVGT's sound engine plays audio through SDL3, mixes it in a tree of mixers, and uses Steam Audio for 3D sound: HRTF, occlusion, and reverb from real geometry.

The pieces fit together like this:
* A [sound](#sound) plays one file, stream, URL, or audio pushed to it from the script.
* A [mixer](#mixer) groups sounds and other mixers. Its volume, pan, pitch and effects apply to everything in it. Every sound and mixer belongs to [sound_default_mixer](#sound_default_mixer) unless it is given another.
* Effects such as filters, reverbs, echoes and pitch shifting are added to any sound or mixer with [set_fx](#set_fx), using effect strings like `"lowpass:800:0:0.7"`.
* A [sound_environment](#sound_environment) holds 3D geometry, and sounds created from it are simulated within it.
* [audio_decoder](#audio_decoder) reads raw samples out of a file without playing it, and [microphone](#microphone) records.

Sounds load from the filesystem, a [pack_file](#pack_file), or a legacy `pack`, whichever [sound_default_pack](#sound_default_pack) points at, and can be decrypted with `sound_default_decryption_key`. Wav, ogg vorbis, opus, flac and mp3 are supported.

Files up to 30 seconds long are decoded once and kept in memory, so loading the same file again is nearly free. Longer files are decoded as they play.
