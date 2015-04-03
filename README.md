# Name #

`p2jaudio`


# Author #

Elias Vanderstuyft (Elias.vds`[at]`gmail.com)

Parts (for jack) are based on the code of the sampler program called '`specimen`',
and (for pulse-simple) on the code example '`parec-simple.c`' on 'freedesktop.org'.
For expansion using the standard pulse lib (so not pulse-simple),
I suggest to look at the '`pavucontrol`' source code as a guiding line.


# Description #

A simple daemon/tool to pipe audio of PulseAudio Source devices to Jack Output ports.

The name is chosen in accordance with the name of the program '`a2jmidi`', where 'p' stands for 'pulse'.

It can be handy when recording from multiple soundcards at the same time,
however, each device will have its own latency, so realtime manipulation of the signal,
e.g. live performances, is not really recommended.


# More Info #

An approximation of latency for a pipe is reported after the benchmark.

To select a PulseAudio Source device, you can use programs like '`pavucontrol`':
run this program and then select in the '`pavucontrol`' program the 'Record' tab,
select 'Show Applications', then '`p2jaudio`' will be listed, now select 'record from `<desired_Source_device>'`,
after this, quit 'p2jaudio' (by keyboardinterrupt `Ctrl-C` for example) and restart it.
Of course you can run multiple instances of '`p2jaudio`'.

For example if there are 2 usb micro's displayed as 2 soundcards,
will latencies of respectively 5ms and 3ms, you can use Ardour (as recorder) to
capture each micro on a different track, and after recording the song,
you'll have to shift the first micro track with 2ms back in time to align the 2 micros appropriately.
(Or if you're using tracks of realtime signals as well,
you may want to shift both micros respectively 5ms and 3ms back in time.)


# Dependencies #

  * `pulseaudio` (and libs) `>= 0.9.21`
  * `jack-audio-connection-kit` (and libs) `>= 1.9.4`
To compile, you'll need the corresponding `devel` packages as well.


# Install #

Run '`make`' in this directory.
Now you're ready to run '`./p2jaudio`'.

If you want to install it to '`/usr/bin`', for now you'll have to do it manually:
  * Run '`sudo cp p2jaudio /usr/bin/p2jaudio`' (for Debian) or otherwise
> run '`su -c "cp p2jaudio /usr/bin/p2jaudio"`'.
  * Now you can run p2jaudio by running '`p2jaudio`' in the terminal.


# Todo #

In order of importance, the first one is the most important.
  * Clean up unnecessary locks and mutexvariables. And clean up code.
  * Use enumerations for '`bufferUnderrunSide`' (or just '`side`'), '`benchmarkStatus`', '`state`' and '`todo`' variables.
  * Write more documentation in the code and optionally function headers with preconditions.
  * Bug testing.
  * Improve performance. (1 low latency pipe (48000Hz) @ 1024periodSize uses in total 1% CPU on an Intel i5, and 3% @ 128periodSize)
  * Create the ability to pass latency and bufferSize cmd arguments, so that no benchmark is required.
  * Use `pulse` instead of `pulse-simple` to allow automatic creation of pipes and detection of NAME and NUM\_CHANNELS.
> > This delivers the advantage of less cpu-usage and memory-usage per pipe, and it can be used to synchronise pipes (to the maximum latency of all pipes). Then 'p2jaudio' may have to become a daemon.
  * Implement '`./configure`'.
  * Implement '`make install`'.
  * Create header file.
  * Optionally create the opposite of this program: so something like '`j2paudio`'.

# Bugs #

  * There is a possible deadlock somewhere in the pulse-start and/or -process thread.
> > This will prevent 'p2jaudio' to start up. So to quit it, you'll have to kill it with signal 9.