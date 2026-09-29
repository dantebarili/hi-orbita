#!/usr/bin/env bash
# PreToolUse guard: pide confirmacion explicita antes de tocar archivos
# que ya causaron un bug real (sdkconfig.defaults) o que definen pines
# sujetos a los guardrails de hardware de CLAUDE.md (audio_capture.h).
input=$(cat)
path=$(echo "$input" | grep -o '"file_path":"[^"]*"' | head -1 | sed 's/"file_path":"//;s/"$//')

case "$path" in
  *sdkconfig.defaults)
    echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask","permissionDecisionReason":"sdkconfig.defaults ya causo un bug real (conflicto USB-Serial-JTAG/UART0, ver firmware/NOTAS_SERIAL.md). Confirma el cambio antes de aplicarlo."}}'
    ;;
  *audio_capture.h)
    echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask","permissionDecisionReason":"audio_capture.h define los pines I2S. Revisa los guardrails de pines en CLAUDE.md (flash 26-32, PSRAM 33-37, JTAG 19/20, UART0 43/44, strapping 0/3/45/46) antes de aplicar."}}'
    ;;
esac
