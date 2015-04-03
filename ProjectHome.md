A simple daemon/tool to pipe audio of PulseAudio Source devices to Jack Output ports.

The name is chosen in accordance with the name of the program '`a2jmidi`', where '`p`' stands for '`pulse`'.

It can be handy when recording from multiple soundcards at the same time,
however, each device will have its own latency, so realtime manipulation of the signal,
e.g. live performances, is not really recommended.