---
name: fsm-check
description: Verifica que una decisión de diseño (firmware, comunicación, o integración de frentes) sea consistente con la FSM y los puntos abiertos documentados en orbita-obsidian/órbita..md. Usar antes de proponer o aceptar cambios que toquen eventos dev_/srv_, transiciones de estado, o el contrato entre el frente Audio y el frente Comunicación.
user-invocable: false
---

CLAUDE.md establece que `orbita-obsidian/órbita..md` es la fuente de verdad de la arquitectura (la FSM, sus estados, transiciones `dev_`/`srv_`, y los puntos abiertos). Ninguna decisión de diseño debe tomarse de memoria si toca esa capa.

Antes de proponer o validar un cambio que:
- agregue, quite, o modifique un evento `dev_*` o `srv_*`,
- cambie una transición de estado o agregue un estado nuevo,
- toque el contrato entre el frente Audio (wake word, filtrado) y el frente Comunicación (WebSockets, backend),
- o proponga un comportamiento durante envío/interrupción (ej. wake word a mitad de un envío, `dev_fin_segmento`),

leer `orbita-obsidian/órbita..md` completo (no solo buscar el término puntual) y verificar:

1. Si el cambio ya está contemplado en la FSM tal como está documentada.
2. Si contradice una transición o un punto ya cerrado.
3. Si toca un punto marcado como abierto/sin decidir — en ese caso, señalarlo explícitamente en vez de asumir una resolución.

Si la propuesta contradice el documento, o si el documento queda desactualizado tras la conversación, avisar explícitamente al usuario en la respuesta — no corregir el archivo por cuenta propia sin que el usuario lo pida (es la fuente de verdad, la edita el equipo).
