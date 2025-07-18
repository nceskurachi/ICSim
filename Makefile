CC=gcc
CFLAGS=-I/usr/include/SDL2 -Wall -Wextra
LDFLAGS=-lSDL2 -lSDL2_image

all: icsim controls

icsim: icsim.o lib.o
	$(CC) $(CFLAGS) -o icsim icsim.c lib.o $(LDFLAGS)

controls: controls.o
	$(CC) $(CFLAGS) -o controls controls.c $(LDFLAGS)

lib.o:
	$(CC) lib.c

clean:
	rm -rf icsim controls icsim.o controls.o

format:
	clang-format -i $(SRC)

tidy:
	clang-tidy $(SRC) -- -I. $(CFLAGS)

cppcheck:
	cppcheck --enable=all --inconclusive --force --std=c99 --language=c --quiet \
	  --suppress=missingIncludeSystem \
	  *.c *.h

lint: tidy cppcheck format

check: all lint

