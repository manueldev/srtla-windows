CC = gcc
VERSION=$(shell git rev-parse --short HEAD)
CFLAGS=-g -O2 -Wall -DVERSION=\"$(VERSION)\"

ifeq ($(OS),Windows_NT)
    CFLAGS += -D_WIN32
    LDFLAGS += -lws2_32
endif

all: srtla_send srtla_rec

srtla_send: srtla_send.o common.o
	$(CC) srtla_send.o common.o -o srtla_send $(LDFLAGS)

srtla_rec: srtla_rec.o common.o
	$(CC) srtla_rec.o common.o -o srtla_rec $(LDFLAGS)

clean:
	rm -f *.o srtla_send srtla_rec
