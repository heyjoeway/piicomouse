<img
    alt="'SLOP ADVISORY: ARTIFICIAL CONTENT' (This project was 99% vibe-coded.)"
    src="./slopadvisory.png"
    width="200"
/>

# Piicomouse

Turns a Wii Remote into a airmouse-style remote using a Pi Pico W 2. HEAVILY WIP. Currently intended for Android TV.

## Attribution and Credits

- Wii homebrew, emulation, and hardware-hacking communities: public documentation, protocol notes, and reverse-engineering efforts that informed practical understanding of Wii Remote behavior.
    - In particular, the [WiiBrew wiki](https://wiibrew.org/wiki/Wiimote) and all contributors.
- Raspberry Pi Foundation and contributors: Raspberry Pi Pico SDK, CYW43 integration, board support, and toolchain ecosystem used by this project.
- TinyUSB contributors (project led by Ha Thach): USB device stack used for composite HID and CDC functionality.
- BTstack by BlueKitchen GmbH and contributors: Bluetooth Classic HID host stack used via Pico SDK integration.
- Nintendo: Wii, Wii Remote, and related names are trademarks of Nintendo. This is an unofficial, community project and is not affiliated with or endorsed by Nintendo.

Because this project was primarily vibe-coded, there may be gaps in documentation and attribution. If you contributed to the project or recognize work that should be credited, *please reach out so we can acknowledge your contributions properly.*

## Quick Start

1. Download the [latest release](https://github.com/heyjoeway/piicomouse/releases/latest).
2. Put your Pico W 2 into bootloader mode by holding the BOOTSEL button while plugging it into your computer.
3. Copy the downloaded UF2 file to the RPI-RP2 drive that appears.
4. Once flashing is complete, hold the BOOTSEL button for 1 second to enter pairing mode for 60 seconds.
5. Press the sync button on your Wii Remote.

You'll know it's working if only the rightmost LED of the Wii Remote is lit.

## Development

[Follow the standard Getting Started guide for Raspberry Pi Pico C/C++ development to set up your environment.](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html) Compile and flash as documented in section 4.1.

## Mappings

- IR: Relative mouse
    - TODO: Absolute mouse mode. Android TV has compatibility issues with current implementation.
- A:
    - When pointing at screen: Left click
    - When not pointing at screen: Gamepad A
- D-pad: Gamepad D-pad
- Home: HID AC Home
- B + Home: HID AC TV Input
- Minus: HID AC Back
- 1: Volume Up
- 2: Volume Down
- Plus: Mute
- Power (hold): HID Sleep
- Connect: Wake via mouse nudge (if host supports)

Remote automatically disconnects after 5 minutes of inactivity.