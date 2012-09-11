CC = gcc
CFLAGS = -Wall -I.

LIBS = -lm -lpthread -ljack -lpulse -lpulse-simple

DEPS = 
OBJ = p2jaudio.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

p2jaudio: $(OBJ)
	gcc -o $@ $^ $(CFLAGS) $(LIBS)
