#!/usr/bin/env python3
"""Регрессионные снимки реального LVGL. Запуск после build.sh; результат вне git."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SIM = ROOT / 'tools/sim'
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else '/tmp/esp32-weather-clock').resolve()
OUT.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TZ='Europe/Moscow', SIM_TIME='1795294740')  # 21.11.2026, 23:59 МСК
base = dict(line.split('=', 1) for line in (SIM / 'weather.txt').read_text().splitlines() if '=' in line)
prefix = '/devices/weather/controls/'
cases = {
    'normal': {},
    'flat': {'temp_series': ','.join(['-10'] * 24)},
    'extremes': {'temperature': '-39.9', 'temp_series': ','.join(map(str, [-40, 40] * 12)),
                 'description': 'Очень длинное описание погоды, которое должно сокращаться'},
    'gaps': {'temp_series': '-12,nan,-10,-8,nan,-15', 'forecast_start': '2026-11-21T23:00:00+03:00'},
    'empty': {'temp_series': '', 'code_series': '', 'cloud_series': '', 'precip_prob_series': ''},
    'two': {'temp_series': '-1,1'},
}
def pixel(path, x, y):
    """RGB одной точки PPM без внешних зависимостей."""
    data = path.read_bytes()
    header = b'P6 480 480 255\n'
    assert data.startswith(header), path
    offset = len(header) + (y * 480 + x) * 3
    return tuple(data[offset:offset + 3])


def bright_count(path, box, threshold=80):
    """Число светлых пикселей в прямоугольнике, без внешних зависимостей."""
    data = path.read_bytes()
    header = b'P6 480 480 255\n'
    assert data.startswith(header), path
    x0, y0, x1, y1 = box
    count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = len(header) + (y * 480 + x) * 3
            count += max(data[offset:offset + 3]) > threshold
    return count


# Длинный заголовок вместе с самой широкой температурой проверяет границу шапки.
long_layout = OUT / 'weather-long-title.json'
document = json.loads((SIM / 'weather-clock.json').read_text())
document['pages'][6]['tiles'][0]['title'] = 'Очень длинное название погодного элемента'
long_layout.write_text(json.dumps(document, ensure_ascii=False))

subprocess.run([str(SIM / 'sim'), '--weather-scale-check'], check=True)
count = 0
for case, updates in cases.items():
    values = dict(base)
    values.update({prefix + k: v for k, v in updates.items()})
    data = OUT / (case + '.txt')
    data.write_text(''.join(k + '=' + v + '\n' for k, v in values.items()))
    layout = long_layout if case == 'extremes' else SIM / 'weather-clock.json'
    for page in range(11):
        dest = OUT / f'{case}-{page}.ppm'
        subprocess.run([str(SIM / 'sim'), '--screen', str(layout),
                        str(data), str(dest), str(page)], env=env, check=True, stdout=subprocess.DEVNULL)
        assert dest.read_bytes().startswith(b'P6 480 480 255\n'), dest
        assert dest.stat().st_size == 480 * 480 * 3 + len(b'P6 480 480 255\n'), dest
        count += 1
    full = OUT / f'{case}-full.ppm'
    subprocess.run([str(SIM / 'sim'), '--screen', str(layout),
                    str(data), str(full), '6', '-', '-', '-', '6'],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    if case == 'normal':
        # Сетка доходит до рамки, а крупный текст занимает новые области.
        for point in ((2, 108), (477, 108), (1, 452)):
            assert max(pixel(full, *point)) > 50, (point, pixel(full, *point))
        for box in ((300, 15, 335, 45),      # температура 56 px
                    (70, 65, 465, 105),     # описание 17 px ниже шапки
                    (0, 90, 50, 150),       # подписи осей 15 px
                    (280, 457, 470, 461)):  # нижние показатели 17 px
            assert bright_count(full, box), box
    if case == 'extremes':
        # Между сокращённым заголовком и -39.9° остаётся пустой зазор.
        assert bright_count(full, (276, 15, 286, 45)) == 0
    count += 1
subprocess.run([str(SIM / 'sim'), '--screen', str(SIM / 'clock.json'), str(SIM / 'clock.txt'),
                str(OUT / 'clock-check.ppm'), '0', '-', '-', '-', '-', 'clock-check'], env=env, check=True)
print(f'OK: {count} снимка 480×480, геометрия, крупный текст и зазор шапки; '
      f'clock-check — 7 размеров × 3 режима. Визуальная проверка: {OUT}')
