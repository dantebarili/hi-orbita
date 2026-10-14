#!/usr/bin/env bash
# PreToolUse guard: pide confirmacion explicita antes de tocar archivos
# que ya causaron un bug real (sdkconfig.defaults) o que definen pines
# sujetos a los guardrails de hardware de CLAUDE.md (audio_capture.h), y
# bloquea la edicion de archivos generados por idf.py.
input=$(cat)
path=$(echo "$input" | grep -o '"file_path":"[^"]*"' | head -1 | sed 's/"file_path":"//;s/"$//')

# El JSON trae las barras de Windows duplicadas (\\): pasarlas a '/'.
path=$(echo "$path" | sed 's#\\#/#g')

case "$path" in
  */firmware/sdkconfig|*/firmware/sdkconfig.old|*/firmware/build/*|*/firmware/managed_components/*|*/firmware/dependencies.lock)
    echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"Archivo generado (sdkconfig, build/, managed_components/, dependencies.lock): esta en .gitignore y se regenera con idf.py. Los cambios de configuracion van en sdkconfig.defaults."}}'
    ;;
  *sdkconfig.defaults)
    echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask","permissionDecisionReason":"sdkconfig.defaults ya causo un bug real (conflicto USB-Serial-JTAG/UART0, ver orbita-obsidian/arquitectura.md §4). Confirma el cambio antes de aplicarlo."}}'
    ;;
  *audio_capture.h)
    echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask","permissionDecisionReason":"audio_capture.h define los pines I2S. Revisa orbita-obsidian/orbita_pinout.md (pines reservados y asignados) antes de aplicar."}}'
    ;;
esac
