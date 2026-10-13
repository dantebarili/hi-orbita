#include "audio_ring_out.h"

/* RING OUT: (SIN AFE) feed -->ring_out--> sink | (CON AFE) fetch -->ring_out--> sink
 *  Dato:        ring propio. audio en SLOTS de 512 muestras mono, 16 bit c/muestra + header de 16 B
 *  Item:        1 SLOT = 1040 B (header 16 B + 512 muestras = 1024 B = 32 ms)
 *  Capacidad:   313 SLOTS ≈ 325 KB = 10 s (en PSRAM)
 *  Productor:   tarea_feed (sin AFE) o tarea_fetch (con AFE), core 1, prio 5
 *  Consumidor:  sink (A7), core 0 (propuesta)
 *  Si se llena: se descarta el slot mas antiguo y sube descartados_ring_out */
typedef struct{
    // FIXME: un arreglo ACA ADENTRO hace que el struct mida 325 KB y viva donde viva la variable
    // (static = RAM interna, la escasa; no entra o deja sin memoria al resto). Tiene que ser un
    // PUNTERO a orbita_slot_t, que apunte a la memoria reservada en PSRAM por la funcion de init.
    orbita_slot_t slots[ORBITA_RING_SLOTS];
    // FIXME: puntero a int16_t avanza de a 2 bytes, no de a un slot (1040 B), y no se le puede
    // aplicar % N. Usar un INDICE entero (0..ORBITA_RING_SLOTS-1) y acceder con slots[indice].
    int16_t* write_cur;
    int16_t* read_cur;
    // FIXME (menor): una cantidad no es negativa, mejor sin signo (uint16_t alcanza para 313).
    int16_t slots_ocupados;
    // FALTA: proximo seq (uint32_t) y contador descartados_ring_out (uint32_t).
}orbita_ring_out_t;
