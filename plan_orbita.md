# Plan Órbita — wake word, AEC/pipeline y prototipo alfa

Actualizado 2026-10-26. Reemplaza a `plan_prototipo_alfa.md` (borrado hoy; sigue en el historial: `git show 1a655e4:plan_prototipo_alfa.md`). Se sacó lo ya hecho (S1-S3, Paso 0, S7 parlante, recepción del downlink, integración del 15/10) y lo que dejó de aplicar (el alfa ya no es "sin wake word en el chip").

## Estado hoy

- **Hecho:** captura continua + sockets `orbita-pcm-v2` con retención y reconexión; servidor con STT/TTS (Deepgram por defecto, ElevenLabs alternativa) y chat de voz con la nube; parlante (`orbita_playback`) con tono local y respuesta por red validados; AFE con beamforming (SE) + VAD en la rama `prototipo-alfa`.
- **En curso:** wake word on-device (microWakeWord) en la rama `wakeword-banco`: componente `orbita_wakeword`, prueba bit a bit en placa PASA. Corre **fuera del AFE**, sobre la captura.
- **Sin empezar:** AEC, barge-in, LED, NFC, pruebas largas.

## Línea 1 — Wake word (Dante) → mergear

- [ ] Terminar la wake word y dejar el criterio de "listo para mergear" escrito (falsos positivos/negativos en las tomas; CPU y RAM interna medidas con el resto del pipeline).
- [ ] Mergear `wakeword-banco` a la rama de trabajo, **después** de comprobar que no pisa RAM ni core del AFE (ver Línea 2). Ojo: el merge puede esperar a que la Línea 2 tenga primeras mediciones; las dos se prueban en paralelo.
- [ ] Anotar en `orbita-obsidian` cómo se alimenta la wake word (mic crudo, sin AFE) y actualizar la FSM del vault, que hoy no contempla wake word en el chip.

## Línea 2 — AEC + pipeline de audio (Dante, en paralelo, antes del merge)

Objetivo: pipeline usable y demostrable a inversores con la placa actual (2 mics + parlante). Usar un solo mic es desperdicio: el AFE aprovecha los dos.

- [ ] **AGC y AEC offline en la PC** (ítem del 22/10, pasado del plan viejo): sobre tomas crudas L y R (WAV de 2 canales de `test.c`) a 0,5 / 1 / 2 m, con y sin ruido. AGC después de la captura (no tocar la ganancia/shift de captura); medir SNR, no volumen, porque el AGC sube ruido y voz juntos. AEC: reproducir un audio conocido por parlante, grabar mic + referencia (mismas muestras) y probar un AEC offline (speexdsp/WebRTC) para estimar el retardo referencia→mic. Terminado = tabla SNR (o eco residual) antes/después por distancia y decisión de qué entra al firmware.
- [ ] **Medir RAM interna** con AFE + parlante + Wi-Fi (S4: el recorte de `orbita_ws` ya está; falta el número y cargarlo en `arquitectura.md` §1, sumando DMA de salida ≈ 4 KB y pila de `playback` 4 KB).
- [ ] **AEC dentro del AFE** (`aec_init = true`, formato `"MMR"`, referencia = audio mandado al parlante). Resolver lo abierto:
  - de dónde sale la referencia (`orbita_playback` no la expone todavía);
  - alinear contando muestras (escritas al I2S vs. índice de captura), no con reloj;
  - achicar el DMA de salida (hoy 120 ms, es retardo de la referencia) hasta el mínimo con `underruns = 0` con Wi-Fi activo, y/o línea de retardo de la referencia;
  - `filter_length` (unidad sin verificar) y convergencia del filtro entre respuestas.
- [ ] **Decidir beamforming**: SE ya está activo en la rama. Confirmar que realmente actúa (el 12/10 la salida era el mic 0 idéntico al crudo) y que convive con AEC (con 2 mics SE tiene prioridad sobre NS).
- [ ] **Barge-in**: VAD como disparador de `play_stop` propio, con contador con fuga (+1 con voz, −2 sin voz, corte a ≈ 0,5-1 s, ignorar ~300 ms tras `STARTED`), umbrales en Kconfig. Acordar el evento `barge_in` con Juani.
- [ ] **Banco de pruebas con la prueba de ElevenLabs** (STT en la nube): medir transcripción y funcionamiento a distintas distancias (p. ej. 0,5 / 1 / 2 / 3 m), con y sin parlante sonando, con y sin AEC; tabla de WER/aciertos de "Órbita" + latencia por etapa. Es el material para mostrar a inversores.
- [ ] Comparar crudo vs. AFE en las mismas tomas (`tools/comparar_canales.py`) para decidir si el AFE rompe o ayuda a la voz que llega al servidor.
- [ ] Con AFE, `ws://` en el alfa (TLS no entra por RAM).

## Línea 3 — Prototipo alfa (ordenar lo que falta)

**Dante**
- [ ] Terminar el código del firmware: cerrar S9 (Línea 2), **A6 reenvío** (tercer cursor del ring; el `ready` trae `last_index`) y la deuda mínima: A9 (`I2S_ISR_IRAM_SAFE` + DMA con Wi-Fi), A16 (limitar `ESP_LOGE` de lectura I2S), A5b (`_Static_assert` de `ORBITA_MSG_SLOTS * 512` y de `ORBITA_SLOT_SAMPLES == ORBITA_AFE_FETCH_FRAMES`), A10 mínimo (guarda de `s_rx_chan == NULL`).
- [ ] Pruebas en placa: corte corto de Wi-Fi (~10 s) y corte largo (> 40 s) sin watchdog.

**Juani**
- [ ] **LEDs**: LED de conversación (se enciende con `orbita`, se apaga con `conversation_end`); fijar el GPIO con `pin-map-check`.
- [ ] **NFC**: integración y pin(es) en el pinout.
- [ ] **Servidor**: `ready` con `last_index` y reenvío (mitad servidor de A6); medir velocidad real del downlink y prebuffer; cliente simulado de la placa; simulación de la conversación (LLM todavía no disponible).

**Juntos**
- [ ] Unir ramas y probar todo junto: **S5 prueba larga** de 1 h con Wi-Fi y cortes de 5/10/20 s (`dma_overflows = 0`, `ring_in_overruns = 0`, `ram_int` y `pila_sink` estables) + estrés del pipeline (A15, máximos en `arquitectura.md` §3.7).
- **Terminado cuando:** una hora seguida con el servidor transcribiendo, un corte de red de 20 s sin pérdida o con el hueco bien marcado, "Órbita" dicha al equipo (wake word + LED) y la respuesta sonando por el parlante sin cortarse sola (AEC), sin reiniciar.

## Línea 4 — Placa v2 (solo si hay quien pague más)

- [ ] **XMOS XVF3800 + 4 micrófonos**: preprocesamiento en el chip (AEC, beamforming, NS, VAD) y el ESP recibe audio ya limpio. Lo que se aprende de la Línea 2 (mediciones por distancia) pasa a ser la línea de base para comparar.
- [ ] Si el XVF3800 hace el AEC, el AFE del ESP y la referencia del parlante dejan de hacer falta: dejar la Línea 2 modular (la referencia y el AFE aislados) para poder sacarlos.

## Deuda y producción (no bloquea el alfa)

- Anotar en `orbita-obsidian/arquitectura.md` §2 la pila de `playback`; verificar el MAX98357A contra su datasheet (`SD`, `GAIN`); A11 (ICS-43434 a 16 kHz).
- A13: borrar `wav_pack_chunk_24bit`, contadores `volatile` → `stdatomic`. A16: `#define` nombrados para stack/prioridad/core y liberar `afe_config`.
- Decisiones abiertas con Juani: ring in lleno (descartar el más viejo), capacidad del ring out (≈ 32,8 s vs. ~60 s), VAD como compuerta de envío (política de privacidad con el cliente), cómo abrir una conversación con un LLM.
- Producción: aprovisionamiento por NVS, `wss://`, `nvs_flash_init` sin `NO_FREE_PAGES`, pasa-altos en la placa final, `arranque_fallo` en bucle (A10).
