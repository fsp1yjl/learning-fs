CC       = gcc
CFLAGS   = -Wall -O2 -std=c99
FUSE_CFLAGS = `pkg-config --cflags fuse3`
FUSE_LIBS   = `pkg-config --libs fuse3`

UTILS_SRCS = fs_utils.c
UTILS_OBJS = fs_utils.o

all: mkmyfs myfs

mkmyfs: mkmyfs.c fs_def.h fs_utils.h $(UTILS_SRCS)
	$(CC) $(CFLAGS) -o $@ mkmyfs.c $(UTILS_SRCS)

myfs: myfs.c fs_def.h fs_utils.h $(UTILS_SRCS)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ myfs.c $(UTILS_SRCS) $(FUSE_LIBS)

clean:
	rm -f mkmyfs myfs disk.img *.o

.PHONY: all clean
