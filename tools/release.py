#!/usr/bin/env python3
"""Подготовить собранную прошивку к раздаче экранам по воздуху.

Запуск из папки проекта после сборки (idf.py build):

    python tools/release.py

Создаёт release/fw/ с двумя файлами:
    esp32-panel.bin  - прошивка
    version.json     - версия, размер и контрольная сумма

Эти два файла кладутся на сервер в папку, которую nginx отдаёт как
/panel/fw/ (см. README). Экраны проверяют её раз в час и при старте,
а также по кнопке «Проверить обновление».
"""
import hashlib
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OUT = ROOT / "release" / "fw"
NAME = "esp32-panel.bin"
WB_DIR = "/mnt/data/esp32-panel-fw/"


def main() -> int:
    desc_path = BUILD / "project_description.json"
    if not desc_path.exists():
        print("Нет build/project_description.json - сначала соберите проект (idf.py build).")
        return 1
    desc = json.loads(desc_path.read_text(encoding="utf-8"))
    version = desc.get("project_version", "")
    app_bin = BUILD / desc.get("app_bin", NAME)
    if not app_bin.exists():
        print(f"Нет файла прошивки {app_bin}")
        return 1

    data = app_bin.read_bytes()
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(app_bin, OUT / NAME)
    info = {
        "version": version,
        "file": NAME,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "built": datetime.now().strftime("%Y-%m-%d %H:%M"),
    }
    (OUT / "version.json").write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"Версия {version}, {len(data) // 1024} КБ -> {OUT}")
    print("Выложить на контроллер (Git Bash или PowerShell):")
    print(f"  scp release/fw/{NAME} release/fw/version.json root@192.168.1.77:{WB_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
