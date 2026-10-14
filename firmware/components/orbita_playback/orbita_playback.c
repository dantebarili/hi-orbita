/* orbita_playback: esqueleto. Se construye por pasos junto con Dante (ver orbita_playback.h). */

#include "orbita_playback.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "playback";

#define PLAY_RATE_HZ          16000   // muestras por segundo (formato del sistema)
#define PLAY_BYTES_POR_MUESTRA    2   // 16 bit mono
#define PLAY_MAX_RESPUESTA_S     15   // respuesta máxima acordada (provisoria)
#define PLAY_RING_BYTES  (PLAY_RATE_HZ * PLAY_BYTES_POR_MUESTRA * PLAY_MAX_RESPUESTA_S)
#define PLAY_PUSH_MAX_BYTES    4096   // tope por llamada a push (frame PLAY del contrato, <= 4 KB)
#define PLAY_TROZO_BYTES       1024   // cuanto saca de a una vez la tarea del ring (32 ms de audio)
#define PLAY_POLL_MS             10   // cada cuanto mira el estado la tarea en reposo
#define PLAY_ESPERA_STOP_MS     100   // cuanto espera begin a que termine un stop en curso
#define PLAY_DMA_DESC_NUM         8   // buffer DMA del I2S de salida: 8 descriptores de 240 frames (igual que la entrada)
#define PLAY_DMA_FRAME_NUM      240
#define PLAY_DMA_MS      (PLAY_DMA_DESC_NUM * PLAY_DMA_FRAME_NUM * 1000 / PLAY_RATE_HZ)   // = 120 ms de colchon
#define PLAY_I2S_WRITE_TIMEOUT_MS 100  // cuanto espera i2s_channel_write a que haya lugar en el DMA (en ms, no en ticks)
#define PLAY_TASK_STACK        4096
#define PLAY_TASK_PRIO            6   // > sink (5): el drenaje de la red no la deja sin datos (arquitectura.md §2)
#define PLAY_TASK_CORE            0   // core 0 = red/control; el core 1 queda para el audio de entrada

typedef enum {
    LIBRE,          // no esta pasando nada
    LLENANDO,       // Aun no se lleno el prebuffer
    SONANDO,        // Sonando pero aun se esta metiendo audio
    VACIANDO,       // Sonando pero ya termino el recibo de audio
    DETENIENDO,     // Se pidio stop: la tarea de reproduccion vacia el ring y pasa a LIBRE
} orbita_state_t;

/* REGLA DE ORO: solo tarea_play lee del ring mientras se reproduce, y SOLO ELLA pasa el estado a LIBRE
 * (fin natural o stop). Un ring de bytes no deja hacer un segundo Receive mientras otro trozo esta
 * "agarrado" (ringbuf.c, prvCheckItemAvail): si stop vaciara el ring desde otra tarea, no descartaria nada.
 * Consecuencia: cuando el estado es LIBRE o LLENANDO, la tarea no tiene ningun trozo en la mano. */

static orbita_state_t estado_play = LIBRE;
static RingbufHandle_t ring_play = NULL;
static orbita_playback_cfg_t cfg_play;
static SemaphoreHandle_t mutex_play = NULL;   // protege estado_play y los contadores: los tocan la tarea de red y la que reproduce
static uint32_t bytes_recibidos_play = 0;     // bytes metidos al ring en la respuesta actual
static uint32_t underruns_play = 0;           // veces que el ring se vacio antes de play_end
static uint32_t prebuffer_bytes_play = 0;     // cuantos bytes hay que juntar antes de empezar a sonar (se calcula en init)
static i2s_chan_handle_t tx_chan = NULL;      // canal I2S de salida (hacia el MAX98357A)
static volatile bool iniciado_play = false;   // true recien cuando init termino TODO (mutex, ring y tarea)

// Avisa un evento a quien registro el callback (puede no haber). Llamar SIEMPRE fuera del mutex.
static void emitir(orbita_play_event_t ev){
    if (cfg_play.cb != NULL) {
        cfg_play.cb(ev, cfg_play.cb_ctx);
    }
}

// Saca y descarta todo lo que haya en el ring. Se lee en un bucle porque un ring de bytes puede
// devolver el contenido en dos trozos cuando "da la vuelta" al final del buffer.
// Solo llamar cuando nadie mas tiene un trozo agarrado: desde tarea_play, o desde begin con estado LLENANDO.
static void vaciar_ring(void){
    size_t n;
    void *trozo;
    while ((trozo = xRingbufferReceiveUpTo(ring_play, &n, 0, PLAY_RING_BYTES)) != NULL) {
        vRingbufferReturnItem(ring_play, trozo);
    }
}

// Crea y enciende el canal I2S de salida: I2S_NUM_1 (la entrada usa el 0), maestro, 16 kHz, 16 bit, MONO.
// En mono el driver repite la muestra en L y R, asi que el PCM se escribe tal cual llega.
static esp_err_t i2s_salida_iniciar(const orbita_playback_cfg_t *cfg){
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = PLAY_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = PLAY_DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;      // si el DMA se queda sin datos manda CEROS (silencio), no repite el ultimo trozo

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);   // solo TX: el RX va en NULL
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel fallo: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(PLAY_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,          // el MAX98357A no necesita MCLK (sin verificar en su datasheet)
            .bclk = cfg->gpio_bclk,
            .ws   = cfg->gpio_ws,
            .dout = cfg->gpio_dout,
            .din  = I2S_GPIO_UNUSED,          // solo transmitimos
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(tx_chan);    // desde aca sale el reloj; sin datos escritos suena silencio
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo iniciar el I2S de salida: %s", esp_err_to_name(err));
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return err;
    }
    return ESP_OK;
}

// TAREA DE REPRODUCCION: saca audio del ring y lo manda al I2S. Se coordina con el resto SOLO por el estado.
// MEJORABLE (latencia): en reposo mira el estado cada PLAY_POLL_MS (hasta 10 ms de demora al arrancar,
// 100 despertares/s). Alternativa: xTaskNotifyGive desde push/end con ulTaskNotifyTake con timeout
// (arranque inmediato, sin polling). No hace falta mientras haya prebuffer de 400 ms. Ver arquitectura.md §3.7.
static void tarea_play(void *arg){
    bool en_underrun = false;            // para avisar UNDERRUN una vez por episodio, no cada vuelta
    while (1) {
        xSemaphoreTake(mutex_play, portMAX_DELAY);
        orbita_state_t est = estado_play;
        xSemaphoreGive(mutex_play);

        if (est == DETENIENDO) {         // stop pedido: aca no hay ningun trozo en la mano, se puede vaciar
            vaciar_ring();               // lo que ya esta en el DMA (hasta PLAY_DMA_MS) suena igual: disable/enable no lo limpia
            xSemaphoreTake(mutex_play, portMAX_DELAY);
            estado_play = LIBRE;
            xSemaphoreGive(mutex_play);
            en_underrun = false;
            emitir(ORBITA_PLAY_EV_STOPPED);   // fuera del mutex
            continue;
        }

        if (est == LIBRE || est == LLENANDO) {
            en_underrun = false;
            vTaskDelay(pdMS_TO_TICKS(PLAY_POLL_MS));
            continue;
        }

        // SONANDO o VACIANDO: sacar un trozo (espera corta si el ring esta vacio).
        size_t n = 0;
        void *trozo = xRingbufferReceiveUpTo(ring_play, &n, pdMS_TO_TICKS(PLAY_POLL_MS), PLAY_TROZO_BYTES);
        if (trozo != NULL) {
            en_underrun = false;
            // i2s_channel_write se bloquea hasta que el DMA tiene lugar: es la que marca el ritmo real (16 kHz).
            size_t escritos = 0;
            esp_err_t err = i2s_channel_write(tx_chan, trozo, n, &escritos, PLAY_I2S_WRITE_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "i2s_channel_write: %s (%u de %u B)", esp_err_to_name(err), (unsigned)escritos, (unsigned)n);
            }
            vRingbufferReturnItem(ring_play, trozo);
            continue;                    // la vuelta siguiente vuelve a mirar el estado antes de pedir otro trozo
        }

        // Ring vacio. El estado leido arriba pudo cambiar durante la espera: se revisa de nuevo bajo el mutex.
        if (est == VACIANDO) {           // termino la respuesta: se suelta la reproduccion
            // El ring esta vacio pero el DMA todavia tiene hasta PLAY_DMA_MS de audio por sonar: se espera
            // antes de dar la respuesta por terminada (asi is_active() y DONE coinciden con lo que se escucha).
            vTaskDelay(pdMS_TO_TICKS(PLAY_DMA_MS));
            bool fin = false;
            uint32_t underruns = 0;
            xSemaphoreTake(mutex_play, portMAX_DELAY);
            if (estado_play == VACIANDO) {   // si entro un stop (DETENIENDO) no se toca: lo atiende la proxima vuelta
                estado_play = LIBRE;
                fin = true;
                underruns = underruns_play;
            }
            xSemaphoreGive(mutex_play);
            if (fin) {
                ESP_LOGI(TAG, "respuesta terminada: underruns=%u, pila libre=%u B",
                         (unsigned)underruns, (unsigned)uxTaskGetStackHighWaterMark(NULL));
                emitir(ORBITA_PLAY_EV_DONE);  // fuera del mutex
            }
        } else if (!en_underrun) {       // SONANDO con el ring vacio: llego tarde el audio, se oye un corte
            en_underrun = true;
            bool contar = false;
            xSemaphoreTake(mutex_play, portMAX_DELAY);
            if (estado_play == SONANDO) {    // si llego end o stop mientras esperaba, no es un underrun
                underruns_play++;
                contar = true;
            }
            xSemaphoreGive(mutex_play);
            if (contar) {
                emitir(ORBITA_PLAY_EV_UNDERRUN);
            }
        }
    }
}

esp_err_t orbita_playback_init(const orbita_playback_cfg_t *cfg){
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;      // sin config no hay nada que copiar: leer *cfg con NULL rompe el programa
    }
    if (iniciado_play || mutex_play != NULL) {
        return ESP_ERR_INVALID_STATE;    // el .h dice "llamar UNA vez": una segunda reservaria otro ring
    }

    // El prebuffer tiene que caber en el ring con margen (como maximo la mitad), si no un push
    // se rechazaria por lleno antes de que la reproduccion arranque.
    uint64_t prebuffer_bytes = (uint64_t)cfg->prebuffer_ms * PLAY_RATE_HZ * PLAY_BYTES_POR_MUESTRA / 1000;
    if (prebuffer_bytes > PLAY_RING_BYTES / 2) {
        ESP_LOGE(TAG, "prebuffer de %u ms demasiado grande para el ring", (unsigned)cfg->prebuffer_ms);
        return ESP_ERR_INVALID_ARG;
    }

    // Todo se arma en variables locales y recien al final se publica: asi ninguna otra tarea ve el
    // modulo a medio inicializar (por eso las funciones publicas miran `iniciado_play`).
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Creo el ring, reservo memoria
    RingbufHandle_t ring = xRingbufferCreateWithCaps(PLAY_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (ring == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar el ring_play en PSRAM");
        vSemaphoreDelete(mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2s_salida_iniciar(cfg);
    if (err != ESP_OK) {
        vRingbufferDeleteWithCaps(ring);
        vSemaphoreDelete(mutex);
        return err;
    }

    cfg_play = *cfg;
    prebuffer_bytes_play = (uint32_t)prebuffer_bytes;
    mutex_play = mutex;
    ring_play = ring;                    // la tarea los usa apenas nace, por eso se publican antes de crearla

    if (xTaskCreatePinnedToCore(tarea_play, "playback", PLAY_TASK_STACK, NULL, PLAY_TASK_PRIO, NULL, PLAY_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de reproduccion");
        ring_play = NULL;
        mutex_play = NULL;
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        vRingbufferDeleteWithCaps(ring);
        vSemaphoreDelete(mutex);
        return ESP_ERR_NO_MEM;
    }

    iniciado_play = true;                // desde aca begin/push/end/stop/is_active funcionan
    return ESP_OK;
}

esp_err_t orbita_playback_begin(void){
    if (!iniciado_play) {
        return ESP_ERR_INVALID_STATE;    // No inicializado
    }

    // Si hay un stop en curso (la tarea todavia esta vaciando el ring), se espera a que termine:
    // un play nuevo justo despues de un stop es normal (por ejemplo, el usuario interrumpe y pide otra cosa).
    bool libre = false;
    for (int i = 0; i < PLAY_ESPERA_STOP_MS; i++) {   // 1 ms por vuelta: el tick de FreeRTOS es de 1 ms
        xSemaphoreTake(mutex_play, portMAX_DELAY);
        orbita_state_t est = estado_play;
        if (est == LIBRE) {
            estado_play = LLENANDO;      // STARTED sale despues, cuando push llene el prebuffer
            bytes_recibidos_play = 0;
            underruns_play = 0;
            libre = true;
        }
        xSemaphoreGive(mutex_play);
        if (libre || est != DETENIENDO) {
            break;                       // ya se abrio, o hay una respuesta en curso de verdad
        }
        vTaskDelay(1);
    }

    if (!libre) {
        emitir(ORBITA_PLAY_EV_REJECTED); // ya hay una respuesta en curso (fuera del mutex)
        return ESP_ERR_INVALID_STATE;
    }
    // Restos de una respuesta anterior (un push que llego tarde, tras un stop). Es seguro vaciar aca: en
    // LLENANDO la tarea de reproduccion no lee, y push no puede correr (lo llama esta misma tarea de red).
    vaciar_ring();
    return ESP_OK;
}

esp_err_t orbita_playback_push(const uint8_t *data, size_t len){
    if (!iniciado_play) {
        return ESP_ERR_INVALID_STATE;    // No inicializado
    }
    // Argumentos: PCM de 16 bit => largo par, y no mas que un frame PLAY.
    if (data == NULL || len == 0 || len > PLAY_PUSH_MAX_BYTES || (len % PLAY_BYTES_POR_MUESTRA) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 1) Solo se acepta audio mientras se llena el prebuffer o mientras suena.
    xSemaphoreTake(mutex_play, portMAX_DELAY);
    bool acepta = (estado_play == LLENANDO || estado_play == SONANDO);
    xSemaphoreGive(mutex_play);
    if (!acepta) {
        emitir(ORBITA_PLAY_EV_REJECTED);
        return ESP_ERR_INVALID_STATE;
    }

    // 2) Al ring SIN esperar (ticks = 0) y fuera del mutex: si no entra, se rechaza el frame nuevo.
    //    Nunca se pisa lo anterior: una respuesta con un hueco en el medio no sirve.
    if (xRingbufferSend(ring_play, data, len, 0) != pdTRUE) {
        emitir(ORBITA_PLAY_EV_REJECTED);
        return ESP_ERR_NO_MEM;
    }

    // 3) Contar el audio y, si ya se junto el prebuffer, pasar a SONANDO.
    bool arranca = false;
    bool cortado = false;
    xSemaphoreTake(mutex_play, portMAX_DELAY);
    if (estado_play == LLENANDO || estado_play == SONANDO) {
        bytes_recibidos_play += len;
        if (estado_play == LLENANDO && bytes_recibidos_play >= prebuffer_bytes_play) {
            estado_play = SONANDO;
            arranca = true;
        }
    } else {
        cortado = true;                  // entro un stop entre el paso 1 y el 2: no se cuenta
    }
    xSemaphoreGive(mutex_play);

    if (cortado) {
        // El audio que acaba de entrar al ring queda como resto: lo limpia el proximo begin.
        emitir(ORBITA_PLAY_EV_REJECTED);
        return ESP_ERR_INVALID_STATE;
    }
    if (arranca) {
        emitir(ORBITA_PLAY_EV_STARTED);  // fuera del mutex
    }
    return ESP_OK;
}

esp_err_t orbita_playback_end(void){
    if (!iniciado_play) {
        return ESP_ERR_INVALID_STATE;    // No inicializado
    }

    bool valido = false;
    bool arranca = false;
    xSemaphoreTake(mutex_play, portMAX_DELAY);
    if (estado_play == LLENANDO) {
        // Respuesta mas corta que el prebuffer: nunca se alcanzo el umbral en push, se arranca igual.
        estado_play = VACIANDO;
        valido = true;
        arranca = true;
    } else if (estado_play == SONANDO) {
        estado_play = VACIANDO;          // ya sonaba; solo falta que se vacie el ring
        valido = true;
    }
    xSemaphoreGive(mutex_play);

    if (!valido) {
        return ESP_ERR_INVALID_STATE;    // no hay respuesta abierta (LIBRE), ya se cerro (VACIANDO) o se esta cortando
    }
    if (arranca) {
        emitir(ORBITA_PLAY_EV_STARTED);  // fuera del mutex
    }
    // DONE no sale aca: lo emite la tarea de reproduccion cuando el ring llegue a vacio.
    return ESP_OK;
}

esp_err_t orbita_playback_stop(void){
    if (!iniciado_play) {
        return ESP_ERR_INVALID_STATE;    // No inicializado
    }

    // Solo se PIDE el corte. Vaciar el ring y pasar a LIBRE lo hace tarea_play (regla de oro, arriba);
    // el evento STOPPED sale de ahi cuando termino.
    xSemaphoreTake(mutex_play, portMAX_DELAY);
    bool habia = (estado_play == LLENANDO || estado_play == SONANDO || estado_play == VACIANDO);
    if (habia) {
        estado_play = DETENIENDO;        // desde aca push rechaza
    }
    xSemaphoreGive(mutex_play);

    return habia ? ESP_OK : ESP_ERR_INVALID_STATE;   // LIBRE o ya DETENIENDO: nada que cortar
}

bool orbita_playback_is_active(void){
    if (!iniciado_play) {
        return false;                    // no inicializado
    }
    xSemaphoreTake(mutex_play, portMAX_DELAY);
    bool activo = (estado_play != LIBRE);   // DETENIENDO cuenta: el parlante todavia puede sonar
    xSemaphoreGive(mutex_play);
    return activo;
}
