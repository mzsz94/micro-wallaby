#!/usr/bin/env python3
"""Create a portable simulator ZIP from an explicit source allowlist."""

from pathlib import Path
import zipfile


SOURCE_FILES = (
    "README.md", "index.html", "style.css", "app.mjs", "model.mjs",
    "model.test.mjs", "serve.py", "start.command", "package.json", "package.py",
    "voice-control.mjs", "voice-control.test.mjs", "alert.css",
    "render-layout.mjs", "render-layout.test.mjs",
    "demo-control.mjs", "demo-control.test.mjs", "demo-render.mjs",
)


def main():
    root = Path(__file__).resolve().parent
    destination = root / "dist" / "micro-wallaby-simulation.zip"
    destination.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in SOURCE_FILES:
            archive.write(root / name, f"micro-wallaby-simulation/{name}")
    print(destination)


if __name__ == "__main__":
    main()
