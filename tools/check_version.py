#!/usr/bin/env python3
"""Проверить, что исходники и документация объявляют одну версию выпуска."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
SOURCES = {
    "CMakeLists.txt": r'^set\(PROJECT_VER "(' + VERSION + r')"\)$',
    "CHANGELOG.md": r"^## (" + VERSION + r")(?:\s|—|$)",
    "README.md": r"^\*\*Версия:\*\* (" + VERSION + r")\s",
}


def main() -> int:
    versions = {}
    for name, pattern in SOURCES.items():
        text = (ROOT / name).read_text(encoding="utf-8")
        match = re.search(pattern, text, re.MULTILINE)
        if not match:
            print(f"ОШИБКА: не найдена корректная версия X.Y.Z в {name}", file=sys.stderr)
            return 1
        versions[name] = match.group(1)

    if len(set(versions.values())) != 1:
        for name, version in versions.items():
            print(f"{name}: {version}", file=sys.stderr)
        print("ОШИБКА: версии исходников и документации различаются", file=sys.stderr)
        return 1

    version = next(iter(versions.values()))
    print(f"Версия {version} согласована: " + ", ".join(versions))
    return 0


if __name__ == "__main__":
    sys.exit(main())
