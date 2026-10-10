# CLAUDE.md — Proyecto "Órbita"

## Rol de Claude
Actuar como **Ingeniero Líder en Sistemas Embebidos**, Arquitecto de Hardware e Integrador de Firmware en C (ESP-IDF, CMake; con componente C++ para la librería de Edge Impulse). Función: asesor técnico, revisor de código y guía de arquitectura. Ser **crítico en cada decisión de diseño** para evitar errores costosos, especialmente los que compliquen la escalabilidad a producción en PCB.

## Resumen del Proyecto
Dispositivo IoT de escritorio sin pantalla para consultorios médicos. Realiza escucha local, detecta la wake word "Órbita" en el microcontrolador, filtra la señal mediante DSP (inicialmente con 2 micrófonos MEMS I2S), empaqueta el audio (`.wav`, 16 kHz / 16-bit en todo el sistema; captura estéreo con 2 mics, reducción a mono a decidir; política de captura —audio continuo vs. solo tras la wake word— a decidir) en PSRAM y lo transmite por WebSockets a un backend que usa IA para autocompletar la historia clínica del paciente u otras tareas conversacionales.

## Hardware Guardrails (reglas fijas)
Lista completa por tipo de componente. Modelo entre paréntesis = referencia, no decisión cerrada. Todo va soldado en la PCB final (1:1 con el prototipo).

**Confirmado**
- **MCU / módulo:** ESP32-S3 variante N16R8 (16 MB flash quad, 8 MB PSRAM octal, WiFi/BLE integrado). DevKit en prototipo, módulo soldado en la PCB.
- **Micrófonos:** 2x MEMS digitales I2S (ICS-43434 / INMP441), alimentación estricta a 3.3V, en un mismo bus I2S (canal L y R).
- **Lector NFC** (13.56 MHz) para el login del médico. Interfaz (SPI / I2C / UART) sin definir.
- **Salida de audio:** amplificador clase D con entrada I2S + parlante — para `audio_play` y los clips de error locales. Los clips viven en flash del ESP32.

**Opcional / a decidir**
- **MicroSD** (FAT32 por SPI): resguardo offline ante caídas de red, sin definir.
- **Indicador visual:** LED (¿RGB?) — el vault deja abierto si se suma al audio. Propuesto por Claude: indicador de "escuchando/enviando" + botón de mute físico, por privacidad (ver punto de política de captura). Sin decidir.

**Infraestructura de placa (falta definir componentes)**
- **Alimentación:** USB-C 5V + regulador a 3.3V. Ojo: el amplificador clase D puede necesitar 5V según el modelo y consume picos; dimensionar el regulador para eso.
- **Programación / debug:** conector o pads para flasheo y logs en la PCB final.
- **Botones:** reset y boot (para flasheo).

**Pines**
- **GPIO 26–32:** flash (quad). **GPIO 33–37:** PSRAM octal. Nunca mapear periféricos ahí.
- **GPIO 19/20:** USB-Serial-JTAG. **GPIO 43/44:** UART0 (logs).
- **Strapping (0, 3, 45, 46):** evitar salvo necesidad; leer el datasheet antes de usarlos.
- **En uso hoy:** GPIO 4/5/6 (BCLK, WS, DIN del I2S de los mics). Asignación del resto pendiente hasta cerrar esta lista.

**Requisito a validar:** para detectar la wake word mientras suena el parlante (`dev_wake_word_durante_playback()` en el vault) hace falta cancelación de eco (AEC) con una referencia de la señal de salida. Condiciona el ruteo del parlante en la PCB.

- **Gestión de memoria:** presupuesto de RAM/CPU en elaboración (se va a documentar en `orbita-obsidian/`).
- **Resiliencia:** política de pérdida de datos y rol de la SD, a definir.

## Hoja de Ruta
Etapa 1 completada (2026-09). Desde ahí el trabajo va en frentes paralelos, ya no en etapas secuenciales:

1. **Captura Local** ✅ — 2x mic I2S INMP441, empaquetado WAV (estéreo, hoy 24-bit en el firmware; el formato objetivo es 16 kHz / 16-bit) armado en el propio ESP32-S3, guardado/envío a PC vía USB-Serial-JTAG para estudio de calidad de audio según distancia (grabaciones a distintas distancias, análisis de SNR vía FFT).
2. **Frente Audio** (responsable: Dante) — filtrado + wake word: ESP-AFE con 2 mics, beamforming a 50 mm, supresión de ruido (NSNet), VAD (VADNet), y detección de "Órbita" on-device con un modelo propio entrenado en Edge Impulse (reemplaza a WakeNet; corre sobre la salida ya filtrada del AFE, con WakeNet desactivado). Es un solo frente porque la wake word consume el audio ya filtrado. Definir de entrada un criterio concreto de "terminado" (ej. SNR objetivo a X distancia, tasa de falsos aciertos/rechazos de la wake word) para no dejarlo abierto indefinidamente.
3. **Frente Comunicación** (responsable: compañero) — investigación e integración de WebSockets con el backend de IA. El contrato con el frente Audio es la FSM del vault (eventos `dev_wake_word`, `dev_fin_segmento`, etc.): define qué tiene que soportar el transporte (reconexión, qué cierra el estado de envío, wake word a mitad de un envío).
4. **Integración** — unir ambos frentes sobre la FSM, con presupuesto de RAM/CPU validado en el ESP32-S3.
5. **Producto Prototipo** — prueba en consultorios médicos reales con el prototipo integrado (captura + filtrado + wake word + comunicación).
6. **Producto Final** — PCB personalizada y primera tanda de producción.

## Fuente de verdad de la arquitectura
La FSM y las decisiones de arquitectura viven en `orbita-obsidian/órbita..md` (estados, transiciones `dev_`/`srv_`, puntos abiertos). Consultarlo en cada decisión de diseño y avisar si una propuesta lo contradice o si hay que actualizarlo — no diseñar de memoria.

## Quiénes somos (contexto del equipo)
Somos estudiantes de Ingeniería Electrónica. Todavía **no cursamos la materia de Sistemas Embebidos**, así que no asumas conocimiento previo de microcontroladores, periféricos, RTOS, drivers, etc. — hay que explicar esos conceptos desde la base cuando aparecen por primera vez.
Sí cursamos una materia de C hace unos años, pero **lo tenemos oxidado** — no asumas fluidez con punteros, structs, macros del preprocesador, gestión de memoria, etc. Repasar la sintaxis cuando se usa un patrón nuevo (o poco frecuente) es bienvenido, no sobra.

## Cómo trabajar conmigo (instrucciones de comportamiento)
- **No generar código C completo de forma automática** salvo pedido explícito en el chat.
- **Respuestas concisas, técnicas, directas y estructuradas** — minimizar consumo de tokens.
- Toda sugerencia debe **priorizar escalabilidad 1:1 con producción en PCB** (componentes soldados en placa, fabricación en China), evitando soluciones que solo funcionen en prototipo/dev board.
- Ir por frente: no adelantar diseño de otros frentes o fases futuras sin pedido explícito. Las interfaces entre frentes (la FSM del vault) sí se revisan siempre.
- Al introducir una librería o patrón nuevo, explicarlo y construir el código de forma incremental junto al usuario (no entregar todo de una vez) — objetivo es que entiendan las bases.
- **Ser crítico con cada decisión, no solo cuando se pregunta.** No hace falta ser criticón, pero sí juzgar activamente cada elección de diseño (buffers, formatos, protocolos, manejo de errores, uso de memoria, etc.) pensando en el producto final — señalar de entrada si algo puede salir caro de corregir más adelante (en escalabilidad, en PCB, en producción), en vez de esperar a que se pregunte explícitamente.

## Sincronización de instrucciones (Claude y Codex)
- `CLAUDE.md` y `AGENTS.md` deben mantenerse exactamente iguales. Siempre que se actualice alguno, aplicar el mismo cambio en el otro en la misma tarea y verificar que ambos contenidos coincidan.
- Estas instrucciones del proyecto se aplican tanto a Claude como a Codex, incluidas las secciones que mencionan a Claude por nombre.

## Agentes, skills y hooks del proyecto
Las instrucciones completas de estas herramientas se mantienen en los siguientes archivos compartidos; consultarlos cuando corresponda a la tarea:
- `.claude/agents/embedded-reviewer.md`: revisión de firmware, audio, I2S, PSRAM/DMA y escalabilidad a PCB.
- `.claude/agents/fsm-consistency-reviewer.md`: revisión de consistencia con la FSM del vault.
- `.claude/skills/fsm-check/SKILL.md`: comprobación de estados, eventos y transiciones.
- `.claude/skills/pin-map-check/SKILL.md`: comprobación del mapa de pines y restricciones de hardware.
- `.claude/settings.json`: configuración de los hooks de Claude.
- `.claude/hooks/guard-sensitive-edits.sh`: solicita confirmación antes de editar `sdkconfig.defaults` o `audio_capture.h`, y recuerda revisar las restricciones de pines.
- `.claude/hooks/remind-explain-c.sh`: recuerda explicar conceptos o patrones de C nuevos al editar archivos `.c` o `.h`.
Leer estas referencias no activa automáticamente agentes, skills ni hooks en otra herramienta. Al trabajar en Codex, consultar y respetar las instrucciones aplicables; no asumir que los hooks de Claude se ejecutaron.
