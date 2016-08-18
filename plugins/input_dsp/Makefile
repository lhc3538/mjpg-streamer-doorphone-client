###############################################################
#
# Purpose: Makefile for "M-JPEG Streamer"
# Author.: Tom Stoeveken (TST)
# Version: 0.3
# License: GPL
#
###############################################################

CC = gcc

OTHER_HEADERS = ../../mjpg_streamer.h ../../utils.h ../output.h ../input.h

CFLAGS += -O2 -DLINUX -D_GNU_SOURCE -Wall -shared -fPIC
#CFLAGS += -DDEBUG
LFLAGS += -lpthread -ldl

all: input_dsp.so

clean:
	rm -f *.a *.o core *~ *.so *.lo

# pictures will not get cleaned automatically, it may be possible
# that other users do not have the required tools for re-building installed
# required tools are:
#  * convert (ImageMagick)
#  * od, sed, tr

input_dsp.so: $(OTHER_HEADERS) input_dsp.c
	$(CC) $(CFLAGS) -o $@ input_dsp.c

