# Simple Terminal Emulator in C

[Demo](https://denizbasgoren.github.io/terminal/demo.gif)

## Features
- Made to be as simple as possible, 600 lines of C only
- Works on Linux only
- No colors, no rich text support
- No copy, paste, select text support
- Uses raylib
- Implements only a few basic CSI escape codes
- No character repeat when keys are held down
- 80x20 size, but configurable
- No unicode support. 8-bit ascii only

## How to compile

Make sure you have raylib installed. Then,

```c
clang main.c -g -lraylib -o terminal
./terminal
```