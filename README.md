README
------

Warning:
Automatically exported from "code.google.com/p/p2jaudio",
so some things might be wrongly converted.

Name
----
p2jaudio

Author
------
Elias Vanderstuyft (Elias.vds[at]gmail.com)
	Parts (for jack) are based on the code of the sampler program called 'Specimen',
and (for pulse-simple) on the code example 'parec-simple.c' on 'freedesktop.org'.
For expansion using the standard pulse lib (so not pulse-simple),
I suggest to look at the 'pavucontrol' source code as a guiding line.

Description
-----------
A simple daemon/tool to pipe audio of PulseAudio Source devices to Jack Output ports.
The name is chosen in accordance with the name of the program 'a2jmidi', where 'p' stands for 'pulse'.
It can be handy when recording from multiple soundcards at the same time,
however, each device will have its own latency, so realtime manipulation of the signal,
e.g. live performances, is not really recommended.

More Info
---------
An approximation of latency for a pipe is reported after the benchmark.
To select a PulseAudio Source device, you can use programs like 'pavucontrol':
run this program and then select in the 'pavucontrol' program the 'Record' tab,
select 'Show Applications', then 'p2jaudio' will be listed, now select 'record from <desired_Source_device>',
after this, quit 'p2jaudio' (by keyboardinterrupt Ctrl-C for example) and restart it.
Of course you can run multiple instances of 'p2jaudio'.
For example if there are 2 usb micros displayed as 2 soundcards,
will latencies of respectively 5ms and 3ms, you can use Ardour (as recorder) to
capture each micro on a different track, and after recording the song,
you'll have to shift the first micro track with 2ms back in time to align the 2 micros appropriately.
(Or if you're using tracks of realtime signals as well,
you may want to shift both micros respectively 5ms and 3ms back in time.)

Dependencies
------------
* pulseaudio (and libs) >= 0.9.21
* jack-audio-connection-kit (and libs) >= 1.9.4
To compile, you'll need the corresponding devel packages as well.

Install
-------
Run 'make' in this directory.
Now you're ready to run './p2jaudio'.
If you want to install it to '/usr/bin', for now you'll have to do it manually:
	Run 'sudo cp p2jaudio /usr/bin/p2jaudio' (for Debian) or otherwise
	run 'su -c "cp p2jaudio /usr/bin/p2jaudio"'.
	Now you can run p2jaudio by running 'p2jaudio' in the terminal.

Bugs
----
* There is a possible deadlock somewhere in the pulse-start and/or -process thread.
  This will prevent 'p2jaudio' to start up. So to quit it, you'll have to kill it will signal 9.
