# NU-87 macOS flash wrapper

This standard-library-only wrapper maps Zephyr's two AmebaD images into the
three fixed files expected by Realtek's macOS uploader. It is dry-run by
default and cannot open the serial port unless `--write` is present.

First validate the images at the conservative 115,200 baud setting:

```sh
python3 tools/nu87_flash/nu87_flash.py \
  --build-dir ../build/nu87-fixture-a \
  --port /dev/cu.usbserial-10
```

To write the board, hold **BOOT**, press and release **RESET**, release
**BOOT**, then immediately add `--write` to the command. On Apple Silicon the
wrapper runs Realtek's x86-64 uploader through Rosetta. If macOS says that the
x86-64 program cannot run, install Rosetta using Apple's normal system prompt.

After the first successful hardware run at 115,200, `--baud 921600` and
`--baud 1500000` may be tested. Only those three rates are accepted.

The write path downloads and SHA-256-verifies the pinned Realtek 1.1.3 archive.
For offline use, pass the same unmodified archive with
`--archive /path/to/ameba_d_tools_macos-1.1.3.tar.gz`. Only the uploader and
flash-loader members are read; the archive is never extracted wholesale.

The input mapping is:

| Zephyr image | Flash address |
| --- | ---: |
| `images/bootloader_all.bin` | `0x000000` |
| `images/km0_km4_app.bin` | `0x014000` |

The wrapper rejects empty images, boot/app overlap, images beyond the 4 MiB
flash boundary, unexpected archives/loaders, unsupported ports, and any vendor
run that lacks both a zero exit code and the explicit success message.

Run the host-only tests with:

```sh
python3 -m unittest discover -s tools/nu87_flash/tests -v
```
