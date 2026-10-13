# Pruebas del socket (prototipo alfa)

Registro corto de lo probado en la placa. Plan y criterios: `plan_prototipo_alfa.md`. Cable y servidor: `INTEGRACION_WEBSOCKET.md`. La evidencia está en `tools/prueba_websocket/` (fuera de Git; la prueba 1 quedó en `prueba_10s_off_wifi/`); `python tools/analizador_wav_generico.py <carpeta>` la resume.

| # | Prueba | Fecha | Resultado |
|---|---|---|---|
| 1 | Corte corto de Wi-Fi (~10 s) con la sesión en curso | 13/10 | **OK** (detalle abajo) |
| 2 | Toma de 10 min (RAM, pila, `sin_ack`) | — | pendiente |
| 3 | Arranque sin servidor y conexión > 40 s después (descarte del mensaje retenido) | 13/10 | **OK**. Con timeout de 1 s se cayó 4 veces al drenar; con 5 s + ventana de acks, **0 caídas** (detalle abajo) |
| 4 | Corte largo > 40 s (watchdog) | — | pendiente |
| 5 | Prueba larga 1 h con cortes de 5/10/20 s (S5) | — | pendiente |

## Prueba 1 — corte de 10 s (13/10/2026, ~17:05)

**Qué se hizo:** servidor v2 abierto, ESP con el firmware v2 recién flasheado; se cortó la red ~10 s con la sesión en curso y se restableció. Segmentos de 10 s (el servidor corría con el valor anterior a 30 s).

**Qué se ve en el servidor** (3 sesiones, 1 conexión cada una):

| Sesión | Origen | Audio | Huecos | `first_seq` → `next_expected_seq` |
|---|---|---|---|---|
| `…2bcdaa54` | firmware **anterior** (sin descarte de retenido), 31 min sin servidor | 38 s | 1: **30,8 min perdidos** | 2522 → 61458 |
| `…2a95f4d2` | firmware nuevo, arranque limpio (`first_seq` 160 = warm-up de 5 s) | 36,7 s | ninguno | 160 → 1308 |
| `…57876321` | reconexión tras el corte | 34,3 s | ninguno | 1308 → 2380 |

**Conclusiones:**
- **El corte no perdió audio:** la sesión nueva empieza en `seq` 1308, exactamente donde terminó la anterior. En la unión de los WAV el salto entre muestras es 8 (promedio 17,7): la señal es continua.
- **La retención funcionó:** la sesión 3 entregó 34,3 s de audio en 24,1 s de conexión; los ~10 s del corte salieron del atraso acumulado, más rápido que tiempo real.
- **La sesión 1 confirma el defecto que corrigió el descarte del mensaje retenido:** abre con 128 ms viejos (`seq` 2522) y salta 30,8 min. Era firmware anterior a ese cambio; con el firmware nuevo no debería repetirse (prueba 3).

**Bug encontrado en el servidor y corregido:** la sesión 3 salió con `previous_session_restart: true` (falso). La conexión vieja seguía abierta cuando llegó la nueva, y el servidor comparaba contra la sesión anterior a esa. Ahora se guarda la sesión (no un número) y su `seq` se lee en vivo; hay un test (`test_previous_session_still_open_when_new_one_starts`).

**Audio:** sin voz en estas tomas (SNR estimado no representativo). Con el cuarto en silencio el 65–99 % de la energía está bajo 60 Hz: sigue pendiente decidir el pasa-altos.

**Falta para dar la prueba por completa:** las líneas del monitor de la ESP durante el corte (reconexión, backoff, `espera_max`, `sin_ack`, `pila_sink`, `ram_int`) — este registro solo tiene la vista del servidor.

## Prueba 3 — arranque sin servidor (13/10/2026, ~17:12)

**Qué se hizo:** ESP encendida con el servidor cerrado y servidor abierto unos 12 s después (5 s de warm-up + ~7 s de espera).

**Resultado:** sesión `…a810b068`, `first_seq` 160, **sin huecos**, 22,8 s de audio en 15,5 s de conexión. Conectó sola al abrir el servidor y entregó el audio retenido sin pérdida.

**Velocidad de drenado (primera medición real):** el segmento 000 (10,1 s de audio) se cerró ~3 s después de abrir la sesión y el 001 diez segundos después (ritmo real). O sea, los ~7,3 s de atraso salieron en **≤ 3 s** (≥ 3,4 veces tiempo real); extrapolando, 10 s de atraso tardarían ≤ 4 s. Es una cota: falta el log de la ESP para medirlo exacto.

**Qué no cubrió:** el descarte del mensaje retenido necesita que el ring pise ≥ 32 slots, o sea **más de ~33 s** sin servidor; con 12 s el ring no se llenó (`first_seq` 160 = el mensaje retenido se mandó, como corresponde). Para cerrarla: dejar la ESP > 40 s sin servidor, abrirlo, y buscar en el monitor `Mensaje retenido descartado` y un `first_seq` mayor que 160 en el JSON de la sesión.

## Prueba 3, repetida (13/10/2026, ~17:20) — con el log de la ESP

ESP sin servidor 46,7 s (`Red de vuelta tras 46702 ms`), servidor abierto, monitor en UART0. Sin reinicios, `dma=0`, `i2s=0`, `pila_sink` ≥ 3,4 KB libres de 6, `ram_int` ≈ 80–84 KB.

- **Descarte del mensaje retenido: OK.** `Mensaje retenido descartado: el ring piso 444 slots` (y 248, 278 en los reintentos).
- **Problema: 4 conexiones seguidas se cayeron a 1–2 s de abrirse**, justo al drenar el atraso. Log de la ESP: `transport_poll_write(0)` / `esp_transport_write() returned 0` (el socket no aceptó datos durante el timeout de envío de 1 s), la biblioteca aborta y reconecta. En el servidor cada una recibió 80, 10, 6 y 29 mensajes: el servidor estaba vivo y procesando; el atasco fue del lado de la transmisión.
- **La quinta conexión drenó 33,7 s de audio en ~5 s (≈ 7×) sin fallar** y quedó estable (`ack` 255, `sin_ack` 7).
- **Costo:** unos 17 s de audio perdidos en total, todos marcados: 4 slots por corte (lo ya "enviado" que estaba en el buffer TCP; `gap_from_previous_session_slots` = 4) y 256 y 282 slots (≈ 8 s cada uno) porque el ring seguía lleno y pisó lo más viejo durante el backoff de 8 s.
- **Hipótesis, sin confirmar:** timeout de envío (1 s) más corto que la recuperación normal de TCP (~1–3 s tras una pérdida), ventana TCP de la ESP muy chica (5760 B) y Wi-Fi en modo de ahorro (`pm start, type: 1`, intervalo de escucha ~300 ms). Cada abortar-y-reconectar agrava la situación: el backoff de 8 s deja al ring lleno y se repite.
- **Sin ack en la primera conexión** (`ack=-1` tras 81 mensajes): los acks no se llegaron a procesar antes de la caída.

**Cambio aplicado (pendiente de repetir la prueba):** timeout de envío 1 s → 5 s y ventana de 32 mensajes sin confirmar (`SEND_TIMEOUT_MS`, `ACK_WINDOW`, `ACK_STALL_MS` en `audio_ws_sink.c`). Hash del firmware (`ELF file SHA256` del banner): `25086f88a`. La línea de resumen suma `vent=` (veces que se esperó por la ventana). Si siguen las caídas: WiFi sin ahorro de energía (`esp_wifi_set_ps(WIFI_PS_NONE)`), después ventana TCP más grande.

## Prueba 3, tercera corrida (13/10/2026, ~17:31) — con timeout de 5 s y ventana de acks (`25086f88a`)

ESP sin servidor 46,3 s (`Red de vuelta tras 46302 ms`), servidor con `--segment-seconds 20`.

- **Una sola sesión, 0 caídas** (antes: 4 seguidas). `610 mensajes, 78,1 s`; `first_seq` 588 → `next_expected_seq` 3028 = 2440 slots = exactamente 610 mensajes: **sin ningún hueco dentro de la sesión**.
- **Único faltante: 13,6 s al principio** (`first_lost` 217088 muestras = 424 slots), que es el límite físico del ring: 46,3 s de corte − 32,8 s de ring. Se descartó el mensaje retenido y quedó marcado. Cubrir cortes más largos exige un ring más grande (plan: 35–60 s).
- **Drenado:** 32,7 s de atraso entregados en ~8,5 s (≈ 4× tiempo real, más lento que los ~7× sin ventana). En el log del servidor se ve **un atasco de ~2 s** (mensajes 96→128) que **sobrevivió**: con el timeout anterior de 1 s habría abortado la conexión. Es la evidencia más directa de la causa. A partir del mensaje ~330 el ritmo es el real (32 mensajes cada 4 s).
- **ESP:** `vent=1` (la ventana actuó una vez), `ack` 311 / `sin_ack` 6 a los 8 s; `dma=0`, `i2s=0`, `pila_sink` ≥ 3,7 KB libres.
- **A vigilar:** `ram_int` bajó de 83,8 a 77,5 KB entre los 61 s y los 91 s (en la corrida anterior también varió, 80–84 KB). Puede ser el buffer de lwip/Wi-Fi bajo tráfico; solo la prueba de 10 min dice si es una tendencia.
- **Audio:** el segmento 002 tiene un pico de −1,1 dBFS (voz fuerte, cerca del tope): sin saturar, pero sin margen.

Pendiente: repetir 2–3 veces (un solo atasco sobrevivido no es una estadística) y la prueba de 10 min.

## Prueba 3, cuarta corrida — automática (13/10/2026, ~17:40), firmware `25086f88a`

Hecha desde el asistente: flasheo por COM4, log de la ESP leído por COM4 (UART0), servidor abierto cuando la ESP llevaba 44,8 s de uptime, cierre limpio del servidor tras dos resúmenes. Evidencia en `tools/prueba_websocket/prueba_ventana_auto1/`.

- **Idéntico a la corrida anterior: 0 caídas.** `Red de vuelta tras 46302 ms`, descarte del retenido (426 slots), sesión única de 557 mensajes (71,3 s), `first_seq` 590 → 2818 = 2228 slots = 557 × 4: **sin huecos internos**. El único faltante son 13,6 s al inicio (límite del ring).
- **Drenado más rápido que antes:** el servidor registró 237 KB/s (≈ 7,4× tiempo real); los 288 primeros mensajes llegaron en ~5 s, sin atasco. `vent=1`.
- **ESP estable:** `dma=0`, `i2s=0`, `pila_sink` 3,7–4,0 KB libres de 6; `ram_int` 82,2 → 83,8 KB (la bajada de 6 KB de la corrida anterior era una fluctuación, no una pérdida). Sin reinicios en 200 s de espera sin servidor (comprobado en una corrida previa donde el servidor no arrancó: `pila_sink` y `ram_int` planos).
- **Al cerrar el servidor** la ESP imprimió 7 `Envio fallido; se reintenta el mismo mensaje` (cada 100 ms) y `Cambio de conexion ... tras 557 mensajes`: es lo esperado, una conexión que se cierra de golpe. El servidor recibió los mismos 557 que la ESP contó como enviados.

Con esta corrida son **dos seguidas sin caídas** con el timeout de 5 s + ventana. Falta la prueba de 10 min.
