#!/usr/bin/env bash
# Stop: si hubo cambios en el firmware, compila con idf.py y, si falla,
# devuelve el error a Claude (exit 2) para que lo corrija antes de dar la
# tarea por terminada. Se salta solo si no hay cambios de firmware o si
# idf.py no esta en el PATH (abrir Claude desde una terminal de ESP-IDF).
input=$(cat)
case "$input" in *'"stop_hook_active":true'*|*'"stop_hook_active": true'*) exit 0 ;; esac

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0
git status --porcelain -- 'firmware/*.c' 'firmware/*.h' 'firmware/*/CMakeLists.txt' \
  'firmware/CMakeLists.txt' 'firmware/sdkconfig.defaults' 'firmware/partitions.csv' \
  'firmware/*Kconfig*' 2>/dev/null | grep -q . || exit 0
command -v idf.py >/dev/null 2>&1 || exit 0

out=$(idf.py -C firmware build 2>&1)
if [ $? -ne 0 ]; then
  echo "idf.py build fallo. Ultimas lineas:" >&2
  echo "$out" | tail -n 25 >&2
  exit 2
fi
exit 0
