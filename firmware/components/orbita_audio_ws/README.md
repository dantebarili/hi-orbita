# Puente WebSocket ↔ parlante

`audio_ws_playback` conecta los eventos de `orbita_ws` con el ring/I2S de
`orbita_playback`. Se inicializa una vez antes de arrancar la captura/pipeline y
su callback se registra en `orbita_ws_start()`.

El ID de `play` del servidor es de 64 bits y `orbita_playback_begin()` recibe un
token opaco de 32 bits. El puente mantiene una tabla pequeña que asocia token,
ID del servidor y conexión. El token vuelve en cada evento de playback, así que
un `DONE` atrasado conserva el ID correcto aunque haya empezado otro `play`.

Cada `PLAY_DATA` se copia al ring de playback antes de que el buffer WebSocket
se reutilice. Si `begin`, `push` o `end` falla, el callback devuelve `false` y
`orbita_ws` encola el único `play_reject`. En particular, si `push` devuelve
`ESP_ERR_NO_MEM`, el transporte despacha un `PLAY_STOP` rechazado; el puente
detiene el playback y suprime el `play_done` terminal para no reportar dos
resultados para el mismo audio. Un `play_stop` normal sí termina con
`play_done` de razón `stopped`.

Al desconectarse, `DOWNLINK_RESET` detiene el parlante y suprime reportes de la
conexión anterior. Los eventos de conversación se consumen, pero todavía no
están conectados a un LED. `audio_ws_playback_is_active()` permite consultar si
el parlante está ocupado; el flag de uplink durante reproducción aún requiere
definir y acordar cuándo se marca cada mensaje con el servidor.

El arranque de `main.c` reserva primero el ring de playback en PSRAM, antes de
que el pipeline asigne sus buffers. La prioridad WebSocket sigue en 4,
playback en 6 y sink en 5; flujo simultáneo validado en placa el 15/10; falta medir pilas y RAM.
El formato binario entrante sigue siendo una propuesta hasta que el servidor
confirme el encabezado.

`audio_ws_nfc` (06/11) es la sesión del médico por NFC: tarea propia que lee el
PN532 (`orbita_nfc`), decide login/logout y avisa al servidor; el sink pone el
`uid` en cada `hello` y `wake`. Es opcional: si el lector no contesta, todo
sigue igual sin login. Contrato y pines en el cartel de `audio_ws_nfc.h`.
