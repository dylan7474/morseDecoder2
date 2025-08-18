# Makefile for real-time Morse decoder
# Requires SDL2 development libraries

CC = gcc
TARGET = morsed
SRCS = main.c
GUI_TARGET = morsed-gui
GUI_SRCS = sample.c
CFLAGS = -Wall -O2 `sdl2-config --cflags`
LDFLAGS = `sdl2-config --libs` -lm
GUI_LDFLAGS = `sdl2-config --libs` -lm -lfftw3 -lSDL2_ttf

all: $(TARGET) $(GUI_TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

$(GUI_TARGET): $(GUI_SRCS)
	$(CC) $(CFLAGS) $(GUI_SRCS) -o $(GUI_TARGET) $(GUI_LDFLAGS)

clean:
	rm -f $(TARGET) $(GUI_TARGET)
