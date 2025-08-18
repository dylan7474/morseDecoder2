# Morse Decoder

This application captures audio from the system microphone and decodes Morse code in real time using simple Goertzel-based tone detection. Provide one or more target tone frequencies on the command line and the decoder will print the decoded characters for each channel.

## Building

Run `./configure` to verify that the required tools and SDL2 development files are installed. Then build with:

```
make
```

## Usage

```
./morsed <freq> [<freq> ...]
```

Each `<freq>` is the frequency in hertz to monitor. Press `Ctrl+C` to quit. A line containing `[space]` indicates a detected word gap.
