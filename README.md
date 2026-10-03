
A lightweight, read-only system monitor for macOS, written in C++17 with Raylib. It displays CPU and memory usage and lists running processes.

## Features

- System-wide CPU and memory usage
- CPU and memory usage for each accessible process
- Search processes by name or PID
- Processes sorted by CPU usage, then memory usage
- Scrollable process list
- System data collected once per second on a background thread

The app only displays system information. It does not terminate processes or change their priorities.

## Requirements

- macOS 11 or later
- CMake 3.16 or later
- A C++17-compatible compiler
- Raylib 5.0 or later

Install the dependencies with Homebrew:

brew install cmake pkg-config raylib
