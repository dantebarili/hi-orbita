# Servidor — lo que funcionó en el de prueba

Resumen para el servidor definitivo. Código: `tools/servidor_wakeword.py` + `tools/ws_audio_cloud.py`; config en `tools/cloud_config.py`. Detalle y mediciones en `arquitectura.md` §6. Estado al 02/11/2026.

## STT (decisión 02/11)
**Prueba (`tools/`): en vivo con `scribe_v2_realtime`.** Deepgram y el STT por frase entera se sacaron.
- **Una sesión de STT por turno:** se abre con la wake word o al terminar la respuesta, y se cierra al llegar el texto final. No se paga mientras piensa el LLM ni mientras suena el parlante (ElevenLabs cobra por audio enviado, silencio incluido). Tiempo de apertura: se registra (`stt_open_ms`), sin medir todavía.
- **Cierre de frase por ElevenLabs** (`commit_strategy=vad`, `STT_VAD_SILENCE_S` = 0,8 s; acepta 0,192–3 s). Sin detector de voz propio. Red de seguridad: si no cierra en 15 s, se usa el último parcial.
- **`previous_text`:** contexto de < 50 caracteres, solo en el primer envío de cada sesión. 1.ª sesión: `STT_CONTEXT`; después: el final de la última respuesta (así se entiende un "¿y la dosis?").
- **Formato:** `pcm_16000` mono (el recomendado): es la salida del beamformer, sin conversiones. Envíos de 0,1–1 s; el ESP manda ~128 ms por mensaje.
- ElevenLabs junta ~2 s antes de transcribir: los cubre el pre-roll de 3 s que manda el ESP con la wake word.
- La sesión se cierra sola tras 15 s sin mensajes (no se paga). Si falla, se reintenta a los 2 s y se pierde lo dicho hasta ahí.
- Sacar las marcas (`[ruido]`, `[interferencia]`) y descartar el turno si no queda texto (también si solo dijo "Órbita").
- **No implementado, disponible:** `keyterms` (palabras clave con costo extra; en vivo hasta 50 de 20 caracteres, en diferido hasta 1000 de 50): remedios, "Órbita".

## Tiempos actuales
| Qué | Valor | Dónde |
|---|---|---|
| Fin de frase (lo decide ElevenLabs) | 0,8 s de silencio | `STT_VAD_SILENCE_S` |
| Volver a escuchar tras la respuesta | apenas llega `play_done` | `_turn` |
| Fin de conversación | 8 s sin texto del STT → `conversation_end` | `IDLE_END_S` |
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
**Modelos de STT de ElevenLabs** (doc, 02/11):

| Modelo | Tipo | Para qué |
|---|---|---|
| `scribe_v2_realtime` | En vivo (WebSocket) | Pedidos urgentes ("Órbita, …"): ~150 ms de mediana al texto final |
| `scribe_v2_medical` | Diferido (archivo) | Transcripción de la consulta: −35 % de errores en audio clínico vs `scribe_v2`, mismos idiomas (español incluido), mismo precio; separa hablantes (médico/paciente) |
| `scribe_v2` | Diferido | General; español ≤ 5 % de WER |
| `scribe_v1` | — | Deprecado |

- **Esquema:** consulta en diferido con `scribe_v2_medical`, **todo junto al final del turno** (mejor separación de hablantes; el resumen llega después); pedidos urgentes en vivo. Coincide con `fsm.md` (resumen continuo + interrupción urgente). Depende de la política de captura (hoy el ESP solo manda tras la wake word).
- **Datos de salud:** BAA (HIPAA) y modo sin retención (`enable_logging=false`) solo en plan Enterprise. Revisar requisitos legales en Argentina (datos sensibles) antes de usar pacientes reales.
- **Conexión:** desde el servidor con la API key (`xi-api-key`), nunca desde el ESP; el server igual necesita el audio (diferido, registro, LLM).
- **Dictado médico:** 0,8 s va a cortar en las pausas para pensar (usar 1,5–2 s o cierre por comando), y el timeout local de 60 s del firmware corta un dictado largo: hace falta un "sigo escuchando" del servidor. Acordarlo en la FSM.
- **Streaming de LLM/TTS por oración:** lo que más latencia queda por sacar.
- `wss://` y timestamp de envío en el ESP para medir el tramo mic → servidor.
