# logger
A named channel to write log messages through, so that messages from different parts of your game can be told apart and turned down separately.

`logger(string name = "app");`

## Arguments:
* string name = "app": the channel name. Dots make a hierarchy, so "game.net.session" is beneath "game.net".

## Remarks:
A logger does not open anything. It writes into whatever log `log_start` or your configuration opened, and does nothing at all while no log is running.

Making a logger for the same name twice gives you two objects that write to the same channel, which is deliberate. Name them after the part of the game they belong to and set levels per channel while you are chasing something.

## Example:
```
logger@ net = logger("game.net");

void main() {
	log_start("errors.log", LOG_DEBUG);
	set_log_channel_level("game.net", LOG_TRACE);
	net.info("connecting");
	net.trace("sent 42 bytes");
}
```
