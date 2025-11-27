.phony:all

all: datstreamer pattern

pattern: pattern.c
	$(CC) -o pattern pattern.c

datstreamer: datstreamer.c
	$(CC) -o datstreamer datstreamer.c

install: datstreamer
	strip datstreamer
	cp -vf ./datstreamer ~/bin


