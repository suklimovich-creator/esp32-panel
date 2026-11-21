#!/usr/bin/env python3
"""Регрессионные снимки реального LVGL. Запуск после build.sh; результат вне git."""
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
subprocess.run([str(SIM / 'sim'), '--weather-scale-check'], check=True)
count = 0
for case, updates in cases.items():
    values = dict(base)
    values.update({prefix + k: v for k, v in updates.items()})
    data = OUT / (case + '.txt')
    data.write_text(''.join(k + '=' + v + '\n' for k, v in values.items()))
    for page in range(11):
        dest = OUT / f'{case}-{page}.ppm'
        subprocess.run([str(SIM / 'sim'), '--screen', str(SIM / 'weather-clock.json'),
                        str(data), str(dest), str(page)], env=env, check=True, stdout=subprocess.DEVNULL)
        assert dest.read_bytes().startswith(b'P6 480 480 255\n'), dest
        assert dest.stat().st_size == 480 * 480 * 3 + len(b'P6 480 480 255\n'), dest
        count += 1
    subprocess.run([str(SIM / 'sim'), '--screen', str(SIM / 'weather-clock.json'),
                    str(data), str(OUT / f'{case}-full.ppm'), '6', '-', '-', '-', '6'],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    count += 1
subprocess.run([str(SIM / 'sim'), '--screen', str(SIM / 'clock.json'), str(SIM / 'clock.txt'),
                str(OUT / 'clock-check.ppm'), '0', '-', '-', '-', '-', 'clock-check'], env=env, check=True)
print(f'OK: {count} снимка 480×480; clock-check — 7 размеров × 3 режима. Визуальная проверка: {OUT}')
