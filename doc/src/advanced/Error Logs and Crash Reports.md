# Error logs and crash reports
NVGT can keep a log file in the manner of Python's logging module, and it writes a crash report whether or not you asked for one.

This exists because a game that dies without saying anything is nearly impossible to support. A player cannot describe a crash they did not see, and you cannot attach a debugger to a machine you do not own. A file the player can send you closes that gap.

## Crash reports need no setup
If the program faults, NVGT writes a report to `errors.log` next to your game before the process dies. You do not have to enable anything, and a player does not have to do anything but find the file.

A report looks like this.

```
NVGT crash report.
Time: 2026-09-08 17:22:44
Version: NVGT 0.90.0-dev, commit 9958b6e2
Reason: access violation, the program read or wrote memory it does not own (EXCEPTION_ACCESS_VIOLATION), code 0xc0000005
Fault address: 0x00007ff73087df4d
Accessed address: 0x0000000000000004
Thread: 25244

Script call stack, 4 frames, innermost first.
Frame 0: void inner(), file crash_test.nvgt, line 8.
Frame 1: void middle(), file crash_test.nvgt, line 14.
Frame 2: void access_violation(), file crash_test.nvgt, line 21.
Frame 3: void main(), file crash_test.nvgt, line 60.

Native stack. This part is for whoever maintains NVGT rather than for you.
Frame 0: nvgt.exe plus 0xaadf4d, FindProcess plus 0x660fd.
Frame 1: nvgt.exe plus 0x116663.
End of crash report.
```

The script call stack is the part you will use. It names the function, the file and the line the game was on when it died, which is the traceback that was missing before.

The native stack below it is for whoever maintains NVGT itself. Module names and offsets are recorded even when no debugging symbols are present, so a stack from a player's machine is still worth something.

On Windows the player also gets a message box naming the file, so they know there is something to send you.

Some faults get caught by code further up the stack and never reach the point where the process dies. Those are recorded too, and the report is written out when the program discovers that something went wrong. That is the class of failure that used to leave nothing at all behind.

## Turning on the log
Crash reports cover the moment things die. A log covers everything leading up to it.

From a script:

```
void main() {
	log_start("errors.log", LOG_WARN);
	log_info("game starting");
}
```

From the command line while developing:

```
nvgt --log=debug mygame.nvgt
nvgt --log=trace --log-file=D:/logs/session.log mygame.nvgt
```

Baked into a compiled game, so that a build you ship logs without the player configuring anything:

```
#pragma config logging.enabled 1
#pragma config logging.level warning
```

## Where the file goes
A relative filename is resolved against the directory of your script, or of your compiled game. If that directory cannot be written to, which is normal for a game installed under Program Files or living inside a macOS bundle, the log goes to the user's preferences directory instead. Read the `log_path` property to find out where it actually landed, and put that path in front of the player rather than guessing.

## Levels
The levels are the familiar ones, from least to most detail: `LOG_CRITICAL`, `LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG`, `LOG_TRACE`. `LOG_OFF` records nothing.

A level is a floor. At `LOG_WARN` you get warnings, errors and criticals, and nothing below that.

## Channels
Every message belongs to a named channel, and channels form a hierarchy separated by dots. Your own messages go to `app` unless you make a `logger` with a name of your own.

NVGT writes to these:

* `nvgt` for general engine errors, which is where every internal error message ends up.
* `nvgt.compiler` for compilation errors and warnings.
* `nvgt.script` for unhandled exceptions and unexpected termination.
* `nvgt.sound` for the sound system, including every failed load with the reason it failed.
* `nvgt.sound.miniaudio` for miniaudio's own output, which explains a great deal about why a device or a file would not open.

A level set on a channel applies to everything beneath it. This matters most for `nvgt.sound.miniaudio`, which describes every audio device on the machine in detail at `LOG_DEBUG`. Turn it down when you want the rest of the log at debug:

```
set_log_channel_level("nvgt.sound.miniaudio", LOG_WARN);
```

## Configuration properties
These can go in a config file beside NVGT, in a `.properties`, `.ini` or `.json` file named after your script, or in a `#pragma config` directive that travels inside a compiled game.

* `logging.enabled` opens the log at startup. Any value at all, including none, counts as on.
* `logging.file` names the file, default `errors.log`.
* `logging.level` sets the starting level, default `warning`.
* `logging.directory` overrides where a relative filename is resolved against.
* `logging.channels` takes a comma separated list of `name=level` pairs, for example `nvgt.sound=debug,nvgt.sound.miniaudio=warning`.
* `logging.console` also writes each message to standard error.
* `logging.source` adds the C++ file and line to the pattern, which is only useful when debugging NVGT itself.
* `logging.pattern` replaces the message format outright. It takes Poco pattern specifiers.
* `logging.max_size` caps the file, default `5 M`. A log already larger than this when the game starts is begun again rather than archived, so there is only ever one file.
* `logging.rotation` turns dated archives back on if you want them, default `never`. Set a size such as `1 M` and you also get `logging.archive` and `logging.purge_count`.
* `logging.crash` turns crash reporting off if set to a false value. It is on by default.
* `logging.crash_alert` turns off the message box that names the log file after a crash.

## Repeated lines
A game can fail the same way every frame. A missing sound in a loop will try to load thousands of times a second, and writing every attempt would bury everything else and fill the disk.

Consecutive identical lines are counted rather than written. The first one appears, then a note at ten, a hundred, a thousand and so on, then a total when something else happens:

```
error nvgt.sound: could not load sounds/foley/none.ogg: Unknown error (-1)
error nvgt.sound: the line above has repeated 1000 times so far
error nvgt.sound: the line above repeated 49999 times in total
```

Nothing is lost, the count is exact, and a flood that used to take three megabytes takes a few hundred bytes.

## Turning it on from inside the game
Everything the configuration properties do can be done from a script instead, which is what you want for an option a player can switch on when asked to reproduce a bug.

```
void set_logging(bool on) {
	if (on) log_start("errors.log", LOG_INFO);
	else log_stop();
}
```

`logging_level` can be changed at any point while the game runs, and `set_log_channel_level` narrows it to one subsystem. Read `log_running` to show the current state in a menu, and `log_path` to tell the player which file to send. Save the setting wherever you save the rest of them, and call `log_start` during startup if it was on last time.

## What to ask a player for
One file. `errors.log`, from the folder the game is installed in. If it is not there, it went to their preferences directory, and printing `log_path` somewhere in your game saves that conversation.
