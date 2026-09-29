#!/usr/bin/env bash
# PostToolUse: recuerda explicar patrones de C nuevos/poco frecuentes
# despues de editar firmware en C, por CLAUDE.md ("Quienes somos" / C oxidado).
input=$(cat)
path=$(echo "$input" | grep -o '"file_path":"[^"]*"' | head -1 | sed 's/"file_path":"//;s/"$//')

case "$path" in
  *.c|*.h)
    echo '{"hookSpecificOutput":{"hookEventName":"PostToolUse","additionalContext":"Recordatorio (CLAUDE.md): el equipo tiene C oxidado y no curso Sistemas Embebidos todavia. Si este cambio introdujo un patron nuevo o poco frecuente (punteros, structs, macros del preprocesador, gestion de memoria, un driver/periferico nuevo), agrega una explicacion breve de la sintaxis/concepto en la respuesta antes de seguir."}}'
    ;;
esac
