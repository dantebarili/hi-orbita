# TODO — Frente Audio

Actualizado 2026-10-11.

Orden: **A (flujo de audio confiable) → B (AFE integrado y medido) → C (listo para sockets)**. A+B = H1 (experimento de las tres fuentes), A+B+C = H2 (mergeable, transporte enchufable). El entrenamiento de la wake word corre en paralelo y no bloquea. A5–A8 solo hacen falta para los sockets (H2).

## Objetivo: prototipo 1 = audio mono continuo por sockets (en pocos días)

Sin wake word ni AFE: el equipo manda **todo el tiempo**. Camino mínimo, en orden: (1) tomas crudas de test; (2) **A14** limpiar `main.c` y pasar captura + ring a un componente, borrando la prueba de test; (3) ring de salida chico (A5 v1: chunks mono con número de secuencia y contador de pérdida, descartar el más viejo; sin urgente, pre-roll ni tabla de segmentos); (4) interfaz de sink + `uart_sink` y `throttled_sink` para probar sin red (A7); (5) robustez de streaming continuo (A9/A10); (6) prueba larga con WiFi activo (A12); (7) el sink de socket del compañero. Estimación gruesa de mi parte: 4–6 días de trabajo concentrado más 2–4 de integración; el transporte del compañero no está estimado. Decidir con él: mono desde cuál mic (L o mezcla), tamaño de mensaje (~100 ms) y header del cable.

## Hecho y validado en el chip

- **A0** flash QIO 80 MHz, CPU 240 MHz, tick 1 ms, particiones con 2 slots OTA + `model` + `clips` (`arquitectura.md` §1). **A1** contador de overflows del DMA (probado forzando la falla). **A2** captura en tarea propia (core 1, prio 6). **A3** `audio_capture`/`wav_writer` en `components/orbita_audio/`. **A4** chunk = 1024 frames = chunk de `feed`, con chequeo contra el AFE al arrancar.
- **Pipeline:** captura → ring 1 (8 chunks, NOSPLIT) → `tarea_feed` → AFE → `tarea_fetch` (core 1; el AFE interno corre en core 0). Toma de 60 s: bytes completos, todos los `overruns` y `dma_overflows` en 0, `feed`=940, `fetch`=1880 (2 por feed), stacks libres captura 3188 / feed 3144 / fetch 1904 B.
- **Test de audio** (borrable, en `main/test.c`): WAV de 2 canales (ch0 mic crudo L, ch1 salida del AFE), 64 bloques = 2,048 s. Scripts: `tools/orbita_serial.py` (guarda en `tools/prueba_espectrogramas/`, pide `Fin:` con `'s'` si no la ve) y `tools/espectrograma.py` (espectrogramas crudo / AFE / celu, `--alinear`, SNR por banda).
- Diseño cerrado de A5/A6 (rings, header de 16 B, tabla de segmentos, pipelining) en `arquitectura.md` §3.4–3.6. Vocabulario fijo: chunk / mensaje / segmento.
- Reviewer del pipeline (2026-10-11): sin bloqueantes para la prueba; lo que queda está en A9, A10 y A14.

## Medido del AFE (2 mics, `AFE_TYPE_SR`, LOW_COST)

- `feed` = 1024 muestras/canal × 2 = 64 ms (~20 ms de cómputo); `fetch` = 512 muestras mono = 32 ms; `data_size` en bytes. Latencia de la tubería: **64 ms** exactos.
- Buffer interno 50 frames = 1,6 s: si no se hace `fetch`, **sobrescribe sin frenar** y solo avisa por log. Memoria mayormente en PSRAM. Pipeline SE(BSS) + VAD WebRTC, **sin NS** (con 2 mics el beamforming desplaza a NS).
- **Hallazgo:** con la config por defecto la salida del AFE era **el mic 0 sin procesar** (idéntico bit a bit al crudo, solo 64 ms después; SNR igual). Con micrófono cerca y cuarto silencioso, celu y ESP dieron espectrogramas casi iguales (SNR 41,6 vs 40,2 dB, una toma).
- Probado: `fixed_first_channel=false` + VAD como disparador → **sigue idéntico** (31170 de 31744 muestras iguales; solo difieren los últimos 36 ms, artefacto fijo del cierre de toma). VAD WebRTC modo 0 marca voz en el 100 % de los bloques (en un tramo de 0,5 s había mucho grave bajo 50 Hz; el offset de continua de tomas de 20 s resultó despreciable: la hipótesis del filtro de continua no se sostiene). `AFE_TYPE_VC` trabaja con frames de 10 ms (feed de 160). NS en `AFE_TYPE_SR` no se activa (pipeline vacío; `feed` baja a 512): el NS del AFE solo existe en VC.
- El código del AFE sigue en `audio_afe.c` (config del experimento NS-solo); se activa con `ORBITA_PIPELINE_USA_AFE 1` en `pipeline.h`.

## Retomar acá

**Decisión (2026-10-11): el AFE y el NS quedan en pausa.** El AFE entregó el mic 0 sin procesar en todas las configs probadas; `esp-sr` desaconsulta el NS para reconocimiento de voz; y no hay evidencia de que el ruido dañe la detección. Se decide con datos de la wake word, no en el chip.

1. **Grabar con el firmware en `ORBITA_PIPELINE_USA_AFE=0`** (WAV de 2 canales: mic L y mic R crudos, 2,048 s) junto con el celu: distancias, con y sin ruido continuo, varias voces. Es lo que necesita la comparación de las tres fuentes (hoy: celu, ESP-L, ESP-R).
2. **Entrenar y evaluar la wake word con audio crudo** (ruido mezclado como aumento): falsas activaciones por hora y rechazos.
3. **Solo si el ruido resulta un problema:** probar NS y beamforming **offline en la PC** sobre las tomas L+R, y portar a la placa lo que ayude. Alternativas ya relevadas: NS WebRTC directo (`esp_ns.h`, 10 ms, sin AFE), `mase_process` (BSS directo, mics a 50 mm), NSNet; plan B: crudo a Edge Impulse.
4. Medir CPU por core y RAM interna/PSRAM con el pipeline que quede (hoy sin AFE sobra margen), y que el WAV no tenga texto adentro.

## Decisiones por hablar con el compañero (contexto en `arquitectura.md` §3.4–3.6)

1. ¿Quién cierra el pedido urgente? Provisorio: el equipo, tras ≥ X ms sin `VOZ` (`dev_fin_segmento`); el backend puede responder antes si procesa en streaming.
2. Recibir mientras se envía (necesario para el pipelining híbrido). Impacta la FSM.
3. Tamaño y criterio del segmento de fondo (pausa de voz con máximo; el número lo define el backend).
4. Qué resumen de `VOZ` lleva el header del cable.
5. Eventos `srv_` nuevos para falsos negativos y positivos de la wake word.
6. Id de segmento y de sesión; `srv_respuesta_recibida` con el id; retomar vs. reenviar el segmento entero.
7. FSM: sacar "buffer lleno" de los motivos de `dev_fin_segmento` y definir el arco de la wake word en `server_back`. Actualizar `órbita..md`.
8. Largo máximo de una respuesta de `audio_play` (afina el ring 2).
9. Política del ring 1 lleno: hoy descarta el chunk nuevo (hueco de 64 ms que afecta la ventana de la wake word); evaluar descartar el más viejo.

## Fase A — pendiente

**Terminado =** `overruns=0` y `dma_overflows=0` en toda captura; el consumidor se entera de un hueco (discontinuidad marcada, nunca se rellena en silencio); el consumidor es intercambiable (UART hoy, socket mañana) sin tocar captura ni ring. No tocar la ganancia/shift de captura (picos en −25 a −30 dBFS): invalida lo grabado.

- [ ] **A5. Ring 2 de slots** (§3.4): slots en PSRAM + 3 cursores + cola de segmentos; v1 sin urgente ni pre-roll. **A6.** Header de 16 B + tabla de segmentos, revisado con el compañero.
- [ ] **A7. Sink:** abrir / recibir chunk / cerrar. `uart_sink` (lo que hace hoy `tarea_envio`), `throttled_sink` (consumo a ritmo configurable, se puede "cortar") y luego el socket. En `fetch` la copia al ring es siempre con timeout 0 (nunca bloquear).
- [ ] **A8. Control:** `main.c` traduce comandos (`'g'`, luego la FSM) a llamadas; la captura hace solo I2S → ring 1.
- [ ] **A9.** `CONFIG_I2S_ISR_IRAM_SAFE` (escrituras a flash pueden bloquear la ISR del I2S) y más descriptores DMA (8–12). Verificar en qué core queda la ISR del I2S cuando entre WiFi.
- [ ] **A10. Errores:** si falla un init/`malloc`/`create`, loguear y reiniciar (hoy queda mudo); timeouts finitos en USB-JTAG y en la espera de `sender_done`; guarda de `s_rx_chan == NULL`; `uart_write` con resultado; framing con secuencia en el UART (sirve también para sockets).
- [ ] **A11.** ICS-43434 soporta 16 kHz (verificar en datasheet; el reviewer duda).
- [ ] **A12. Validación:** captura de 10 min con contadores en 0; corte simulado con `throttled_sink` de 5/10/20 s (valida el tamaño del ring 2).
- [ ] **A13. Menores:** borrar `wav_pack_chunk_24bit`, capacidad de `out_buf` en `wav_pack_chunk_16bit`, contadores `volatile` → `stdatomic`.
- [x] **A14 (2026-10-12, compila; falta probar en el chip).** `main.c` quedó en 17 líneas. Captura (+ AFE opcional con `ORBITA_PIPELINE_USA_AFE` en `pipeline.h`) vive en `components/orbita_pipeline/` y entrega chunks por callback (`on_chunk`, `on_afe_block`). La prueba de test quedó aislada en `main/test.c/.h` (se borra sacando esos dos archivos, la línea de `CMakeLists.txt` y 3 llamadas de `app_main`).

## Fase B — AFE medido (cierra H1)

- [ ] Verificar cómo carga `esp-sr` NSNet/VADNet (partición `model`) y cómo se flashean los modelos.
- [ ] **WebRTC vs. NSNet/VADNet:** modo 1 `SR_NSN_WEBRTC` + `SR_VADN_WEBRTC`; modo 2 `NSNET2`/`NSNET3` + `VADNET1_MEDIUM` (NSNet3 113 KB, NSNet2 334 KB). Medir CPU por core, RAM, `overruns`/`dma_overflows` en 0 y calidad (SNR; que no se coma r, b). Anotar el modo de cada toma.
- [ ] Que el AFE procese de verdad (ver "Retomar acá").
- [ ] Grabador de test con duración variable (hoy fija en 64 bloques): arrancar/parar por comando.
- [ ] Documentar RAM/CPU en `orbita-obsidian/plan-presupuesto-ram-cpu.md`.

## Fase C — listo para sockets (cierra H2)

- [ ] Mostrar A6 y A7 al compañero y acordar qué pide el transporte (reconexión, qué cierra el estado de envío, wake word a mitad de un envío; contrato = FSM del vault).
- [ ] Revisar con `fsm-check` / `fsm-consistency-reviewer`; actualizar el vault y `arquitectura.md` si cambia algo.
- [ ] Decidir política de captura (continuo vs. tras wake word) y mono vs. estéreo hacia el backend.
- [ ] Checklist de "mergeable" y `embedded-reviewer` sobre todo `orbita_audio`.

## En paralelo: wake word "órbita"

**Pregunta abierta:** ¿sirve entrenar con audio de celu? Se averigua grabando **las mismas tomas a la vez** con tres fuentes: celu, ESP crudo (ch0) y ESP + AFE (ch1, válida solo si el AFE procesa). Primera comparación (una toma, cerca, silencio): celu y ESP se parecen; falta ruido, distancia y más voces.

**Tomas:** "órbita" a 0,5 / 1 / 2 m × con y sin barbijo × ~5, más 1–2 min de ruido de labo; ruido continuo para ver el AFE. Se alinea con `--alinear` (retardo medido), no hace falta aplauso.

| Resultado | Qué hacer |
|---|---|
| Celu y ESP se parecen | Entrenar con celu + ruido de la placa mezclado |
| Difieren de forma pareja | Estimar el filtro celu → ESP y aplicarlo a los audios de celu |
| Difieren en ruido o nivel | Mezclar ruido de la placa y normalizar ganancia |
| El AFE cambia mucho la señal | Entrenar con salida del AFE o con crudo + ruido, y comparar |
| El AFE no aporta o rompe "órbita" | Dejarlo afuera (ahorra PSRAM y CPU) |

**Modelo v0 en Edge Impulse:** test fijo con "órbita" de 3–5 personas en la ESP + `ruido_labo3` para falsas activaciones/hora; variantes (celu limpio / + ruido / + tomas ESP). Métricas: FRR y falsas activaciones por hora. Más personas vale más que más tomas por persona. Lo posterior (Common Voice, WhatsApp, MS-SNSD, inyección de ruido) está en `orbita-dataset/dataset.md`.
