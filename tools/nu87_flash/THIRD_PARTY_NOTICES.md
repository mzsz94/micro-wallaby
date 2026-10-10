# Third-party notices

The wrapper itself contains no Realtek binary. During an authorized `--write`
run it can download two runtime files from Realtek's AmebaD Arduino tools:

- Project: [AmebaD Arduino](https://github.com/Ameba-AIoT/ameba-arduino-d)
- License: [MIT](https://github.com/Ameba-AIoT/ameba-arduino-d/blob/master/LICENSE)
- Package: `ameba_d_tools_macos-1.1.3.tar.gz`
- Pinned upstream commit: [`d2865db726d651f086579c65853a76acbe6336e3`](https://github.com/Ameba-AIoT/ameba-arduino-d/commit/d2865db726d651f086579c65853a76acbe6336e3)
- Pinned package URL: <https://raw.githubusercontent.com/Ameba-AIoT/ameba-arduino-d/d2865db726d651f086579c65853a76acbe6336e3/Arduino_package/release/ameba_d_tools_macos-1.1.3.tar.gz>
- Package SHA-256: `fec90f971a7f22b4e6ffbe770d25da4bd52cbd92cc8d473620d8b86ac1040713`
- Flash-loader SHA-256: `9307121385cb390dfd2da64da2c6c515f17b5a9556b3d04021487c9b9f220b55`

The downloaded files remain in a temporary directory for the duration of the
flash operation and are then removed. Their upstream license and notices apply.
