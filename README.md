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

Press the `.` key while the `morsed` window has focus to inject an audible test
tone at the first specified frequency. If `.` does not trigger a tone on your
keyboard layout, the comma, keypad `.` or even the space bar can be used
instead. This can be used as a simple Morse key to verify decoding without
external audio equipment. A log message is printed whenever the period key is
pressed so you can confirm it is being detected.

The `morsed` window title includes the build date and time so you can confirm
which binary version is running.

The decoder assumes an initial speed of 15 words per minute to estimate
the lengths of dits and dahs.

## Graphical interface

`make` also builds `morsed-gui`, a graphical application based on the original sine wave detector. It automatically locks onto up to five sine waves and displays the decoded Morse code for each active channel.

Run it with:

```
./morsed-gui
```
