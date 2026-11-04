#!/bin/sh
# Снимок пульта на компьютере без платы: тот же main/pads.c и LVGL 9.2.2.
#   sh tools/sim/build.sh <путь к lvgl 9.2.2> <путь к cJSON>
#   tools/sim/sim tools/sim/ac.json tools/sim/ac.txt ac.ppm [шторка: 7 режим, 8 вентилятор, 9/10 жалюзи]
# ac.json - плитка из /api/screen/<имя>, ac.txt - значения «топик=значение».
set -e
LV=${1:?путь к lvgl}; CJ=${2:?путь к cJSON}
D=$(cd "$(dirname "$0")" && pwd); M=$D/../../main
mkdir -p "$D/obj"; cd "$D/obj"
for f in inter-regular inter-semibold inter-light; do cp "$M/fonts/$f.ttf" . && ld -r -b binary -o $f.o $f.ttf; done
find "$LV/src" -name '*.c' | while read c; do
  o=$(echo "$c" | md5sum | cut -c1-12).o; [ -f "$o" ] || gcc -O1 -c -DLV_CONF_INCLUDE_SIMPLE -I"$D/stub" -I"$LV" -I"$LV/src" "$c" -o "$o"
done
gcc -O1 -DLV_CONF_INCLUDE_SIMPLE -I"$D/stub" -I"$LV" -I"$LV/src" -I"$M" -I"$CJ" "$D/sim.c" "$CJ/cJSON.c" ./*.o -lm -o "$D/sim"
echo "готово: $D/sim"
