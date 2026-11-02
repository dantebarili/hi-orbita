# Aro de luz (24 x WS2812) — cómo funciona el firmware

Código: `firmware/components/orbita_led/` (dibuja) y `firmware/components/orbita_audio_ws/audio_ws_led.c` (decide el estado). Pin: GPIO1 (`pines.md`). Tarea `led`: `arquitectura.md`. Colores elegidos con el artifact "Aro LED de Órbita" (31/10). Estado: **sin probar en placa**.

## Capas

```
wake word / WebSocket / parlante / timer de red      ← "pasó algo"
              │  audio_ws_led_conversacion(true) ...
              ▼
   audio_ws_led.c   → ESTADO (reposo, escuchando, hablando, sin red)
              │  orbita_led_set(color, brillo, patrón)
              ▼
   orbita_led.c     → CÓMO SE VE (animación, gamma, corriente)
              │  led_strip_refresh()
              ▼
   RMT + DMA → GPIO1 → 24 x WS2812
```

`orbita_led` no sabe de conversaciones; `audio_ws_led.c` no sabe del RMT. Si cambia el LED (otro modelo, un RGB en la PCB) se toca solo la capa de abajo.

## A. `orbita_led.h` — interfaz

```c
#define ORBITA_LED_GPIO        1
#define ORBITA_LED_CANTIDAD    24
#define ORBITA_LED_MAX_MA      350   // tope de corriente del aro
#define ORBITA_LED_MA_CANAL    20    // por canal a pleno; sin datasheet
#define ORBITA_LED_TOPE        153   // 60 % de brillo máximo
#define ORBITA_LED_PERIODO_MS  1300  // respiración y latido
#define ORBITA_LED_PISO        35    // % mínimo de la respiración
#define ORBITA_LED_FUNDIDO_MS  300   // transición entre estados

typedef struct { uint8_t r, g, b; } orbita_rgb_t;
typedef enum { ORBITA_LED_FIJO, ORBITA_LED_RESPIRA, ORBITA_LED_LATIDO } orbita_led_patron_t;

esp_err_t orbita_led_init(void);
void orbita_led_set(orbita_rgb_t color, uint8_t brillo, orbita_led_patron_t patron);
```

- `#define`: el preprocesador reemplaza texto antes de compilar; no ocupa memoria.
- `struct` agrupa datos; `typedef` le da nombre corto. `uint8_t` = entero sin signo de 8 bit exactos.
- `enum`: enteros con nombre (0, 1, 2), evita números mágicos.
- `orbita_led_set` **no bloquea**: deja un pedido y vuelve en µs (la llaman las tareas de wake word y WebSocket).

## B. `orbita_led.c` — el dibujante

### B1. Datos privados

```c
typedef struct { orbita_rgb_t color; uint8_t brillo; orbita_led_patron_t patron; } pedido_t;
static led_strip_handle_t s_tira;
static QueueHandle_t s_buzon;      // cola de 1 lugar
```

`static` global = visible solo en este `.c`. Un *handle* es un ticket opaco que devuelve la librería al crear algo. `pedido_t` es lo que viaja por el buzón.

### B2. `patron_factor` — forma de la animación (0..1)

```c
const float x = (float)(t_ms % ORBITA_LED_PERIODO_MS) / ORBITA_LED_PERIODO_MS;   // fase 0..1
if (patron == ORBITA_LED_RESPIRA) onda = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * x);
else { /* latido */ lub = expf(-0.5f*a*a); dub = 0.62f*expf(-0.5f*b*b); onda = lub > dub ? lub : dub; }
return piso + (1.0f - piso) * onda;
```

- Fase: resto de dividir por el período, normalizado. El `(float)` (*cast*) evita la división entera, que daría 0.
- Respiración: `0,5 − 0,5·cos(2πx)` va 0 → 1 → 0 y arranca abajo (sin salto).
- Latido: dos campanas de Gauss, "lub" al 10 % del ciclo (σ 0,045) y "dub" al 30 % (σ 0,05, 62 % de alto), después pausa. `? :` es el ternario.
- Piso: comprime la onda a 0,35..1 para que no se apague en cada ciclo.

### B3. `objetivo` — color que debería verse ahora

```c
static void objetivo(const pedido_t *p, int64_t t_ms, float out[3])
{
    const float k = (p->brillo / 255.0f) * (ORBITA_LED_TOPE / 255.0f) * patron_factor(p->patron, t_ms);
    out[0] = p->color.r / 255.0f * k;  /* ... g, b */
}
```

- `const pedido_t *p`: puntero (se pasa la dirección, no una copia); `const` = no lo modifica; `p->campo` = `(*p).campo`.
- `float out[3]`: los arrays no se copian al pasarlos; la función escribe en el array de quien llama (forma de devolver 3 valores).
- `k` = brillo del estado × tope global × animación, en **brillo percibido**.

### B4. `mostrar` — percibido → corriente → hardware

```c
duty[c] = powf(v, LED_GAMMA);                                        // gamma 2,2
const float ma_canales = ORBITA_LED_CANTIDAD * suma * ORBITA_LED_MA_CANAL;
const float ma_libre = ORBITA_LED_MAX_MA - ORBITA_LED_CANTIDAD * LED_MA_CHIP;
const float escala = ma_canales > ma_libre ? ma_libre / ma_canales : 1.0f;
rgb[c] = (uint8_t)lrintf(duty[c] * escala * 255.0f);
for (int i = 0; i < ORBITA_LED_CANTIDAD; i++) led_strip_set_pixel(s_tira, i, rgb[0], rgb[1], rgb[2]);
led_strip_refresh(s_tira);
```

1. Recorte a 0..1.
2. **Gamma:** el ojo no es lineal; `duty = percibido^2,2` (50 % percibido ≈ 22 % de PWM). Sin esto la respiración se ve pegada arriba.
3. **Tope de corriente:** estima 24 × suma × 20 mA; a 350 mA le resta 24 mA (consumo propio de los chips, no se baja). Si se pasa, escala todo igual: mantiene el color, baja la intensidad. Ej. "hablando": duty ≈ (0,12; 0,13; 0,33) → 273 + 24 ≈ **300 mA** < 326 libres, no recorta.
4. `set_pixel` solo carga un buffer en RAM; `refresh` manda la trama. Los 24 LEDs llevan el mismo color.

### B5. `tarea_led` — el bucle

```c
TickType_t espera = animando ? pdMS_TO_TICKS(LED_CUADRO_MS) : portMAX_DELAY;
if (xQueueReceive(s_buzon, &nuevo, espera) == pdTRUE) {
    for (int c = 0; c < 3; c++) desde[c] = ahora[c];   // el fundido parte de lo que se ve
    actual = nuevo;  inicio_us = esp_timer_get_time();
}
f = t_ms >= ORBITA_LED_FUNDIDO_MS ? 1.0f : (float)t_ms / ORBITA_LED_FUNDIDO_MS;
ahora[c] = desde[c] + (meta[c] - desde[c]) * f;
mostrar(ahora);
animando = actual.patron != ORBITA_LED_FIJO || f < 1.0f;
```

- Tarea FreeRTOS: `for(;;)` infinito que el sistema reparte con las demás; nunca hace `return`.
- `xQueueReceive` bloquea (sin gastar CPU) hasta `espera`: animando → 20 ms (50 cuadros/s); quieta → para siempre hasta el próximo pedido.
- `&nuevo`: dirección donde la cola copia el pedido.
- Fundido lineal de 300 ms desde lo que **se ve ahora** (sin saltos si cambia a mitad de una respiración); en percibido, así se ve parejo. `t_ms` cuenta desde el inicio del estado.

### B6. `orbita_led_init` — hardware

```c
led_strip_config_t tira_cfg = { .strip_gpio_num = ORBITA_LED_GPIO, .max_leds = ORBITA_LED_CANTIDAD,
    .led_model = LED_MODEL_WS2812, .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB };
led_strip_rmt_config_t rmt_cfg = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000,
    .mem_block_symbols = 1024, .flags.with_dma = true };
s_buzon = xQueueCreate(1, sizeof(pedido_t));
xTaskCreatePinnedToCore(tarea_led, "led", LED_TAREA_STACK, NULL, LED_TAREA_PRIO, NULL, LED_TAREA_CORE);
```

- `.campo = valor`: inicializadores designados; lo no nombrado queda en 0.
- **RMT:** periférico que genera pulsos con tiempos precisos. Cada bit del WS2812 es un pulso alto corto (0) o largo (1), ~0,4/0,8 µs; a 10 MHz hay 0,1 µs de resolución. GRB: el WS2812 espera verde, rojo, azul.
- **DMA con 1024 símbolos:** la trama (24 × 24 = 576 símbolos) sale de una vez. Sin DMA el RMT recarga por interrupción a mitad de trama; si el Wi-Fi la demora, colores basura. Cuesta ~4 KB de RAM interna.
- `tarea_led` sin paréntesis = puntero a función (qué correr, no llamarla). Core 0, prioridad 2, pila 3 KB.

### B7. `orbita_led_set` — dejar el pedido

```c
if (s_buzon == NULL) return;
const pedido_t p = { .color = color, .brillo = brillo, .patron = patron };
(void)xQueueOverwrite(s_buzon, &p);
```

- `xQueueOverwrite` (solo colas de 1 lugar) pisa el pedido sin leer: buzón, gana el último; nunca se llena ni bloquea.
- `(void)` = descarto el retorno a propósito. Guarda `NULL`: si `init` falló, el equipo sigue sin indicador.

## C. `audio_ws_led.c` — decide el estado

### C1. Siempre compilado

Antes iba entre `#if CONFIG_ORBITA_LAB_WEBSOCKET ... #endif`; desde el 01/11 el firmware tiene un solo modo y el `#if` se sacó.

### C2. Tabla de estilos (toda la estética acá)

```c
typedef enum { REPOSO, ESCUCHANDO, HABLANDO, SIN_RED, N_ESTADOS } estado_t;
static const estilo_t ESTILOS[N_ESTADOS] = {
    [REPOSO]     = { {0x64, 0xB5, 0xF6},  31, ORBITA_LED_FIJO    },   // celeste, 12 %
    [ESCUCHANDO] = { {0x64, 0xB5, 0xF6}, 255, ORBITA_LED_RESPIRA },   // mismo celeste, pleno
    [HABLANDO]   = { {0x9F, 0xA8, 0xFF}, 255, ORBITA_LED_FIJO    },   // lavanda
    [SIN_RED]    = { {0xFF, 0xA7, 0x26}, 115, ORBITA_LED_LATIDO  },   // ámbar, 45 %
};
```

- `N_ESTADOS` (último del enum) = cantidad de estados: el tamaño de la tabla se ajusta solo.
- `[REPOSO] = ...`: fila asociada por nombre, no por posición. `static const` → queda en flash.

### C3. Tres entradas, un estado calculado

```c
static SemaphoreHandle_t s_mutex;
static bool s_conversacion, s_hablando, s_red_ok;
static estado_t s_mostrado = N_ESTADOS;     // centinela: "todavía no se mostró nada"
```

Cada parte del firmware informa solo su hecho; el estado sale de combinarlos.

### C4. `actualizar` — el corazón

```c
static void actualizar(bool *dato, bool valor)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *dato = valor;
    estado_t e = s_hablando ? HABLANDO : !s_red_ok ? SIN_RED : s_conversacion ? ESCUCHANDO : REPOSO;
    if (e != s_mostrado) { s_mostrado = e; orbita_led_set(ESTILOS[e].color, ESTILOS[e].brillo, ESTILOS[e].patron); }
    xSemaphoreGive(s_mutex);
}
```

- `bool *dato`: dirección de una de las tres variables; `*dato = valor` escribe ahí. Una función para las tres entradas.
- Ternario encadenado = **prioridad**: hablando > sin red > escuchando > reposo.
- Solo pide al aro si el estado cambió (si no, el timer de red reiniciaría la respiración cada 500 ms).
- **Mutex:** la llaman 4 tareas; sin él, el último pedido al aro podría ser de un estado viejo. Spinlock no sirve: desactiva interrupciones y adentro no se puede usar la cola.

### C5. Timer de red

```c
static void on_timer_red(void *arg) { (void)arg; actualizar(&s_red_ok, orbita_ws_is_connected()); }
```

Callback de `esp_timer` cada 500 ms (corre en la tarea de `esp_timer`, no en ISR: el mutex vale). Polling en vez de eventos: simple y no toca el código de Juani; hasta 500 ms de retraso en mostrar SIN RED.

### C6. Init y API pública

Init: crea el mutex primero, después `orbita_led_init`, arranca en SIN RED y lanza el timer (`500 * 1000` µs, con cast `(uint64_t)` por hábito contra desbordes). Las funciones públicas son de una línea: `audio_ws_led_conversacion(b)` → `actualizar(&s_conversacion, b)`, `audio_ws_led_hablando(b)` → `actualizar(&s_hablando, b)`.

## D. Enganches y secuencia

| Llamada | Archivo | FSM (`fsm.md`) |
|---|---|---|
| `audio_ws_led_init()` | `main.c`, después de la wake word | arranque |
| `audio_ws_led_conversacion(true)` | `audio_ws_conversacion.c` (`on_wake`) | `dev_wake_word` → `server_send` |
| `audio_ws_led_conversacion(false)` | `audio_ws_conversacion.c` (`cerrar`) | fin / timeout → `audio_wake` |
| `audio_ws_led_hablando(true)` | `audio_ws_playback.c` (`STARTED`) | `audio_play` |
| `audio_ws_led_hablando(false)` | `audio_ws_playback.c` (`DONE`/`STOPPED`) | vuelve a `server_back` |
| timer 500 ms | `audio_ws_led.c` | `error_reintento` |

```
arranque          → SIN RED (ámbar, latido)
conecta (≤500 ms) → REPOSO (celeste 12 %, fijo)
"Órbita"          → ESCUCHANDO (celeste, respira)
llega el audio    → HABLANDO (lavanda, fijo)
termina de sonar  → ESCUCHANDO
timeout           → REPOSO
se cae el Wi-Fi   → SIN RED (salvo hablando, que gana)
```

## E. Costo de CPU (estimado, sin medir)

- **Fijo (reposo, hablando):** 0 % tras el fundido; la tarea queda bloqueada.
- **Animando (escuchando, sin red):** por cuadro (20 ms), 1-2 `cosf`/`expf` + 3 `powf` + 24 `set_pixel` ≈ 10-30 µs → **~0,1-0,3 % de un core**. El S3 tiene FPU de precisión simple; las funciones `static` chicas el compilador las mete en línea.
- La trama (576 bits × 1,25 µs ≈ 0,75 ms) la saca el **DMA**; la tarea espera bloqueada.
- Prioridad 2 en core 0: el Wi-Fi (~23) la interrumpe cuando quiere; AFE y wake word van en el otro core. Peor caso: un cuadro unos ms tarde, invisible.
- Una tarea con `float` debe quedar fija en un core (registros de FPU por core): por eso `xTaskCreatePinnedToCore`. Nunca `float` en una ISR.
- Si hiciera falta: `LED_CUADRO_MS` = 33 (30 cuadros/s) o tabla de 256 valores para la gamma.
- **Medir:** `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` + `vTaskGetRunTimeStats()`.

## F. Sin verificar

- 20 mA/canal y 1 mA/chip: de la publicación del aro y estimado, no de datasheet → ajustar `ORBITA_LED_MA_CANAL` tras medir.
- Nivel lógico: a 5 V el "1" pide ~3,5 V y el S3 da 3,3 V → buffer 74AHCT1G125 en la PCB (sin verificar contra el datasheet del LED).
- Gamma 2,2: valor típico; ajustar `LED_GAMMA` si se ve raro.
- RAM interna (~7,5 KB: 4 KB DMA + 3 KB pila) y corriente real: medir en placa.
