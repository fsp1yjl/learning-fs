CC = gcc
CFLAGS = -Wall -O2 -std=c99
FUSE_CFLAGS = `pkg-config --cflags fuse3`
FUSE_LIBS = `pkg-config --libs fuse3`

all: mkmyfs myfs

mkmyfs: mkmyfs.c fs_def.h
	$(CC) $(CFLAGS) -o mkmyfs mkmyfs.c

myfs: myfs.c fs_def.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o myfs myfs.c $(FUSE_LIBS)

clean:
	rm -f mkmyfs myfs disk.img
