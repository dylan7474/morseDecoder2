# Makefile for real-time Morse decoder
# Requires SDL2 development libraries


CC = gcc
TARGET = morsed
SRCS = main.c
CFLAGS = -Wall -O2 `sdl2-config --cflags`
LDFLAGS = `sdl2-config --libs` -lm

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
