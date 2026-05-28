This project was 100% vibe-coded. 

# Piicomouse

Turns a Wii Remote into a airmouse-style remote. HEAVILY WIP. Currently intended for Android TV.

## Attribution and Credits

- Wii homebrew, emulation, and hardware-hacking communities: public documentation, protocol notes, and reverse-engineering efforts that informed practical understanding of Wii Remote behavior.
    - In particular, the [WiiBrew wiki](https://wiibrew.org/wiki/Wiimote) and all contributors.
- Raspberry Pi Foundation and contributors: Raspberry Pi Pico SDK, CYW43 integration, board support, and toolchain ecosystem used by this project.
- TinyUSB contributors (project led by Ha Thach): USB device stack used for composite HID and CDC functionality.
- BTstack by BlueKitchen GmbH and contributors: Bluetooth Classic HID host stack used via Pico SDK integration.
- Nintendo: Wii, Wii Remote, and related names are trademarks of Nintendo. This is an unofficial, community project and is not affiliated with or endorsed by Nintendo.

Because this project was primarily vibe-coded, there may be gaps in documentation and attribution. If you contributed to the project or recognize work that should be credited, *please reach out so we can acknowledge your contributions properly.*

## Features

- Bluetooth Classic HID host for Wii Remote
- Hard-coded target Wii Remote address for fast reconnects
- Direct connect with retry loop (no inquiry delay when target is known)
- Wii IR camera parsing with normalization and dropout handling
- Runtime pointer or digitizer mode switching
- Keyboard mappings for navigation and media/system controls
- 5-minute inactivity auto-disconnect to save Wii Remote battery

## Hardware and Software

- Raspberry Pi Pico 2 W
- Pico SDK 2.2.0
- TinyUSB device stack
- BTstack Classic via CYW43

Board is configured as:
- pico2_w

## Build and Flash

## Prerequisites

[Follow the standard Getting Started guide for Raspberry Pi Pico C/C++ development to set up your environment.](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html) Compile and flash as documented in section 4.1.

## Connection Behavior

- Target Wii Remote address is hard-coded in wiimote initialization.
- On startup and after disconnect, firmware attempts direct HID connect.
- If connect fails, it retries periodically.
- Pairing uses legacy PIN mode with SSP disabled.

## USB HID Behavior

ONLY RELATIVE MOUSE IS CURRENTLY TESTED WORKING

Three HID interfaces are presented:
1. Pointer (relative mouse)
2. Digitizer (absolute coordinates)
3. Keyboard

Mouse and keyboard reports are both active while connected. Pointer vs digitizer determines which pointing report type is sent.

## Wii Remote Control Mapping

## Pointer and Click

- IR movement controls pointer movement in Pointer mode
- A: left click
- Minus: no mouse click mapping (reserved for keyboard Back behavior)

## Keyboard Mappings

- D-pad Up: Arrow Up
- D-pad Down: Arrow Down
- D-pad Left: Arrow Left
- D-pad Right: Arrow Right
- 1: Volume Up
- 2: Volume Down
- Home: Keyboard Home key (used as Android Home equivalent on many hosts)
- Plus: Mute
- Minus: Escape (used as Android Back equivalent on many hosts)

## Mode Switching and Hotkeys

- B + 1: switch to Pointer mode
- B + 2: switch to Digitizer mode
- Home + Up/Down: cycle IR sensitivity profile

## Inactivity Timeout

- If no Wii input activity is seen for 5 minutes, firmware disconnects the Wii Remote.
- Button and IR activity both reset the inactivity timer.

## Known Notes

- Android behavior can vary by device and launcher. Home/Back mappings rely on host interpretation of keyboard Home and Escape.
- If your Android TV does not react to a specific key as expected, remapping to an alternate keycode may be needed.

## Project Files

- main.c: startup and BTstack run loop
- wiimote.c: Bluetooth HID host, Wii report parsing, mapping logic
- usb_descriptors.c: TinyUSB composite descriptors and HID send wrappers
- tusb_config.h: TinyUSB configuration
- btstack_config.h: BTstack settings

## Future Ideas

- Add a physical Sync button on Pico for pairing/re-pair workflow
- Optional dynamic device discovery mode
- Per-device mapping profiles
