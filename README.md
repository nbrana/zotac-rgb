# zotac-rgb

Control the RGB lighting of Zotac GeForce RTX 50-series cards on Linux, without FireStorm.

Developed and tested on a Zotac RTX 5090 (subsystem `19da:1761`) with the proprietary NVIDIA driver.

## Build

    cc -O2 -Wall -o zotac-rgb zotac-rgb.c

## Usage

    ./zotac-rgb info                      # show current state
    ./zotac-rgb set -c ff0000             # primary colour red, all zones
    ./zotac-rgb set -z 1 -c blue -B 50    # zone 1 only, 50% brightness
    ./zotac-rgb set -m 9 -s 20            # effect mode / speed
    ./zotac-rgb on | off

Options for `set`: `-z ZONE` (0/1), `-c`/`-C` primary/secondary colour (hex or name),
`-m MODE`, `-B 0-100`, `-s SPEED`. Unspecified fields are preserved.
`raw REG VAL` and `commit` are available for experimenting.

The NVIDIA I2C bus (`/dev/i2c-N`, "NVIDIA i2c adapter") is auto-detected; override with `-b N`.
You need read/write access to it (`i2c-dev` module loaded; add yourself to the `i2c` group or use a udev rule).

## Protocol

Recovered by disassembling ZOTAC FireStorm 5.0.0.017E, which uses `NvAPI_I2CRead/Write` on port 1.
The controller is the I2C device at 7-bit address `0x4B` (FireStorm's `0x96`):

| Register    | Meaning                                  |
|-------------|------------------------------------------|
| `0x10`      | global enable (assumed 1=on, 0=off)      |
| `0x20`      | zone select (0..1)                       |
| `0x21`      | LED index select (0..2)                  |
| `0x22`      | effect mode                              |
| `0x23-0x25` | colour 1 (R,G,B)                         |
| `0x26-0x28` | colour 2 (R,G,B)                         |
| `0x29`      | brightness 0-100                         |
| `0x2a`      | speed                                    |
| `0x2b-0x2f` | extra effect parameters (preserved)      |
| `0x17` <- 1 | apply the selected zone/index            |

FireStorm waits ~3 ms between transactions; so does this tool.

## Status / caveats

- Colour writes verified by readback on one card. `off`/`on` and the mapping from effect names
  to mode numbers are not verified. FireStorm uses modes 0-11, 23, 24, 31-34.
- Other Zotac models use different controllers/addresses (FireStorm also knows 0x49 and 0x4A);
  this tool only implements the 0x4B variant.
- Writing to I2C devices on a GPU can misbehave if pointed at the wrong chip. Use at your own risk.
