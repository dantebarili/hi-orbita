# Plan de contenido — "Construyendo Órbita"

Building in public. Promesa: dos estudiantes de electrónica que todavía no cursaron Embebidos construyen un aparato que escucha al médico. Cada video cierra con un problema abierto.

**Objetivo:** seguidores y autoridad técnica (hasta tener demo extremo a extremo). **Cadencia:** arrancar con 2 reels/semana, subir a 3. Mismo video en IG, TikTok y Shorts. 1 posteo de LinkedIn por fase. Filmar por tandas (2-3 h = 3 videos).

## Estructura de cada reel
- 0-2 s: resultado o fallo a la vista, algo en movimiento (placa, gráfico, LED), entendible sin sonido.
- Medio: una idea y un número.
- Cierre: "en el próximo probamos X" (un solo CTA).
- Hook < 12 palabras + texto en pantalla de 3-5 palabras. Generar 10 hooks y probar 2-3.
- Formatos que rotan: Fail del día · Antes/después · Explicado en 30 s · Demo.

## Reglas
- "Como si fuera ahora" = re-ejecutar la prueba real frente a cámara. No inventar fechas ni resultados.
- Lo en curso (wake word, AEC) se muestra como en curso.
- Nunca mostrar `cloud_config.py`, claves de API, audios de terceros ni nada que insinúe pacientes reales.
- Nombrar a Juani (avisarle antes).
- Privacidad como argumento solo cuando la política de captura esté decidida.
- No afirmar números sin verificar (p. ej. el precio de la placa).
- Si decimos una frecuencia, se cumple.

## Fases, temas y hooks

| Fase | Hito real | Videos (tema → hook · texto) |
|---|---|---|
| 0. Presentación | Visión | 1. Qué es Órbita → *"Estoy construyendo un aparato para que el médico deje de tipear."* · NO ESCRIBE MÁS. 2. Quiénes somos → *"No cursamos Embebidos. Igual lo estamos haciendo."* · SIN SABER |
| 1. Que escuche | 2 mics, WAV, RMS en vivo | 3. Primer sonido → *"Esto es lo primero que oyó mi placa."* · PRIMER AUDIO. 4. Dos micrófonos → *"Con un mic escucha ruido. Con dos, elige."* · 1 MIC VS 2. 5. 10 min de ruido → *"Grabé 10 minutos de silencio. Mirá lo que apareció."* · SILENCIO ≠ SILENCIO |
| 2. Medir en serio | SNR por distancia, FFT, espectrogramas | 6. Voz a 0,5/1/2 m → *"¿A cuántos metros deja de entenderte?"* · DISTANCIA. 7. Espectrograma → *"Así se ve tu voz."* · TU VOZ, DIBUJADA |
| 3. Que no se rompa | Ring buffer, protocolo, reconexión WebSocket, prueba 10 min | 8. Bug de transferencia → *"Se congeló todo. La causa era tonta."* · FAIL. 9. Ring buffer → *"Así no se pierde ni un sonido."* · SIN PÉRDIDAS. 10. Cortar el Wi-Fi → *"Le corté internet al aparato."* · ¿SOBREVIVE? |
| 4. Que hable | STT/TTS Deepgram, parlante, latencias | 11. Primera respuesta → *"El aparato me contestó."* · HABLÓ. 12. Demora → *"Cada etapa tarda esto. Una es la culpable."* · LATENCIA |
| 5. "Órbita" (en curso) | Wake word propia (microWakeWord) | 13. Entrenar la palabra → *"Le enseñé a una placa una palabra."* · TINYML. 14. Falsos positivos → *"Se activa con cualquier cosa. Lo arreglo."* · FAIL #2. 15. Funciona → *"Dije 'Órbita' y se prendió."* · FUNCIONÓ |
| 6. Lo difícil (en curso) | AEC, beamforming, barge-in | 16. Eco → *"El parlante se escucha a sí mismo."* · EL ECO. 17. Beamforming → *"Dos micrófonos apuntando a mi boca."* · ENFOCA. 18. Interrumpir → *"¿Se puede interrumpir a un robot?"* · BARGE-IN |
| 7. Hacia producto | PCB KiCad, placa v2 | 19. Protoboard → PCB → *"Este es el prototipo. Esta, la placa que lo reemplaza."* · PCB. 20. Privacidad → solo con la política decidida |

## Calendario de las primeras 2 semanas

| Día | Video | Formato | Meta |
|---|---|---|---|
| Sem 1 · Lun | 1 · Qué es Órbita | Cara a cámara + placa | Seguidores |
| Sem 1 · Jue | 3 · Primer sonido | Demo | Guardados |
| Sem 2 · Lun | 2 · Quiénes somos | Historia | Seguidores |
| Sem 2 · Jue | 4 · Dos micrófonos | Explicado en 30 s | Compartidos |

(2 por semana; si se sostiene, sumar el 5 y el 6 como tercer reel semanal.)

## Producción
Cada video, ~2 h de edición con el formato fijo. El cuello de botella es filmar pruebas reales: empezar por los videos 1-7, cuyo material ya existe.

## Origen
Método adaptado de las skills `going-viral`, `viral-hook-writer`, `series-planner`, `content-calendar` y `reel-scripter` (Ootto-AI/claude-content-skills, MIT), instaladas en `~/.claude/skills/`.
