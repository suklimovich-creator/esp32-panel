#!/usr/bin/env python3
"""Упаковать уже собранную прошивку для публикации через wb-svg-panel.

Сборка выполняется отдельно кнопкой в VS Code. Этот скрипт проект не собирает:

    python tools/release.py

Результат — release/esp32-panel-X.Y.Z.bin. Версия читается из esp_app_desc_t
внутри самого образа, поэтому старая сборка не получит новое имя случайно.
"""

import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OUT = ROOT / "release"
PROJECT = "esp32-panel"
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432


def image_info(path: Path) -> tuple[str, str]:
    with path.open("rb") as fh:
        head = fh.read(APP_DESC_OFFSET + 256)
    if len(head) < APP_DESC_OFFSET + 256:
        raise ValueError("слишком короткий файл прошивки")
    if struct.unpack_from("<I", head, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        raise ValueError("в файле нет описания приложения ESP-IDF")

    def text(start: int, size: int) -> str:
        return head[start:start + size].split(b"\0", 1)[0].decode("ascii")

    # esp_app_desc_t: magic(4), secure_version(4), reserv1(8),
    # затем version[32] и project_name[32].
    version = text(APP_DESC_OFFSET + 16, 32)
    project = text(APP_DESC_OFFSET + 48, 32)
    if project != PROJECT:
        raise ValueError(f"это прошивка {project!r}, а не {PROJECT}")
    parts = version.split(".")
    if len(parts) != 3 or any(not p.isdigit() or (len(p) > 1 and p[0] == "0")
                              for p in parts):
        raise ValueError(f"версия в прошивке должна иметь вид X.Y.Z: {version!r}")
    return version, project


def main() -> int:
    desc_path = BUILD / "project_description.json"
    if not desc_path.exists():
        print("Нет build/project_description.json — сначала соберите проект в VS Code.")
        return 1
    desc = json.loads(desc_path.read_text(encoding="utf-8"))
    app_bin = BUILD / desc.get("app_bin", PROJECT + ".bin")
    if not app_bin.exists():
        print(f"Нет файла прошивки {app_bin}")
        return 1
    try:
        version, _project = image_info(app_bin)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"ОШИБКА: {exc}", file=sys.stderr)
        return 1

    name = f"{PROJECT}-{version}.bin"
    OUT.mkdir(parents=True, exist_ok=True)
    target = OUT / name
    shutil.copyfile(app_bin, target)
    remote = f"/mnt/data/wb-svg-panel/{name}"
    print(f"Версия {version}, {target.stat().st_size // 1024} КБ -> {target}")
    print("Публикация на контроллере (прошивка и OTA не запускаются скриптом):")
    print(f"  scp release/{name} root@192.168.1.77:/mnt/data/wb-svg-panel/")
    print(f"  ssh root@192.168.1.77 \"python3 /mnt/data/wb-svg-panel/tools/firmware_publish.py {remote}\"")
    print("Проверка манифеста: curl http://192.168.1.77/panel/fw/firmware.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
