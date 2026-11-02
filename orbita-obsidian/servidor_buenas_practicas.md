# Servidor — lo que funcionó en el de prueba

Resumen para el servidor definitivo. Código: `tools/servidor_wakeword.py` + `tools/ws_audio_cloud.py`; config en `tools/cloud_config.py`. Detalle y mediciones en `arquitectura.md` §6. Estado al 01/11/2026.

## Detectar voz y fin de frase
- **Medir el nivel en banda de voz (pasabanda 150–3400 Hz), mandar al STT el audio crudo.** El mic tiene mucho ruido grave: en banda ancha la voz casi no supera el fondo; filtrado, la SNR pasa de 3–6 dB a > 20 dB.
- **Umbrales relativos al piso de ruido**, no fijos: inicio = 2 × piso (mín. 25), cierre = 2,5 × piso (mín. 35). Dos umbrales distintos (histéresis) para que el ruido no mantenga la frase abierta.
- **Márgenes:** 0,5 s antes del inicio y 0,5 s después de la última voz, para no comerse la primera ni la última sílaba.
- Sacar las marcas de ElevenLabs STT (`[ruido]`, `[interferencia]`) y descartar el turno si no queda texto.

## Tiempos actuales
| Qué | Valor | Dónde |
|---|---|---|
| Voz confirmada | 0,3 s sobre el umbral | `ONSET_BLOCKS` |
| Fin de frase | 0,8 s de silencio (antes 1,2) | `END_QUIET_BLOCKS` |
| Volver a escuchar tras la respuesta | apenas llega `play_done` (antes 0,5 s de silencio) | `SETTLE_QUIET_S` |
| Fin de conversación | 8 s sin voz → `conversation_end` | `IDLE_END_S` |
| Pre-roll al detectar "Órbita" | 3 s (firmware) | `AUDIO_WS_PREROLL_SLOTS` |
| Timeout local de la conversación | 60 s sin actividad del servidor (firmware) | `AUDIO_WS_CONV_TIMEOUT_S` |
| Heartbeat | ping cada 10 s, sin pong en 3 s = caída (firmware) | `orbita_ws.c` |

## LLM
- **Sin razonamiento** (`thinkingLevel: "minimal"`) y `maxOutputTokens` 300: con "low" el razonamiento se come los tokens y la respuesta sale cortada.
- **Prompt de voz:** máximo dos frases, sin listas, markdown ni emojis. Historial de 12 mensajes.
- **Modelo principal + respaldo:** si tarda > 6 s o da 429/500/503, prueba el siguiente (`GEMINI_RESPALDO`). Los modelos gratis se saturan por momentos (de 0,8 s a 13 s el mismo día).
- **Medir antes de cambiar de modelo.** Hoy: `gemini-3.5-flash-lite` ~0,8 s. `2.5-*` no está disponible para cuentas nuevas; `3.7`/`3.8-flash` no aceptan `minimal`.

## Voz de salida (TTS) y parlante
- Pedir el audio **ya en PCM 16 kHz, 16 bit** (`pcm_16000` en ElevenLabs), el formato del parlante: sin conversiones.
- **Cadena antes de mandar al parlante** (`normalize()`):
  1. **Pasaaltos 200 Hz**: el parlante chico no da graves pero gasta excursión en ellos y distorsiona.
  2. **Limitador** en bloques de 5 ms, con transiciones suaves: picos 6 dB más bajos (`PLAY_PEAK_CUT_DB`).
  3. **Compensación** al nivel promedio original: mismo volumen percibido (−0,3 dB medido), ~13 ms de cómputo.
- Subir volumen sin limitador (+3 dB) saturaba el parlante. El firmware ya está en ganancia 1,0 y solo atenúa: el volumen se ajusta en el servidor (`PLAY_GAIN_DB`).

## Registro
- **Latencia por etapa en cada turno** (`stt_s`, `llm_s`, `tts_s`, `send_s`, `total_s`) en `cloud_*.jsonl`. Sin esto no se sabe qué recortar.
- **Cada activación de la wake word se guarda** (WAV + fila en `activaciones.csv` con lo transcrito) para etiquetar reales/falsas y reentrenar. Nunca subir esos audios a un repo.

## Lecciones del prototipo
- Cortes del WebSocket cada 20–30 s con reconexión en ~1 s **no son reinicios** (un reinicio tarda varios segundos): eran pérdidas de paquetes por un **cable USB malo**. Mirar cuánto tarda en reconectar antes de culpar al firmware.

## Para el servidor definitivo
- **Dictado médico:** el cierre de 0,8 s va a cortar en las pausas para pensar (usar 1,5–2 s o cierre por comando), y el timeout local de 60 s del firmware corta un dictado largo: hace falta un "sigo escuchando" del servidor. Acordarlo en la FSM.
- **Streaming** de STT (con fin de frase del proveedor) y de LLM/TTS por oración: es lo que más latencia saca (~1,5–2,5 s).
- `wss://` y timestamp de envío en el ESP para medir el tramo mic → servidor.
