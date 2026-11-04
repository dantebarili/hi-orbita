# Reel B5 — "Una IA en un chip de 5 dólares"

**Duración:** ~35 s · **Meta:** guardados y autoridad técnica · **Formato:** explicación con animaciones.
Separado del reel B3 (`reel_03_wakeword.md`) para que ninguno pase de 40 s. Fuentes: `orbita-dataset/docs/entrenamiento.md` (§2.8 y §5.3), `entrenamiento.log` (en `orbita-dataset/train_v0/run_orbita.tgz`), `orbita-obsidian/wakeword.md` ("Tiempos máximos").

## Hooks (probar 2-3)
1. "Esta IA corre en un chip de menos de 5 dólares." ← elegido
2. "Achicamos una IA 4 veces para que entre en este chip."

## Guion

| Tiempo | Dicho | Texto en pantalla | Imagen |
|---|---|---|---|
| 0-3 s | "Esta IA corre en un chip de menos de 5 dólares." | CHIP < US$5 | Primer plano del ESP32-S3; el aro LED despierta |
| 3-9 s | "Tiene 26.049 coeficientes: los números que la red ajusta al aprender." | 26.049 COEFICIENTES | Animación de neuronas conectadas; las conexiones se iluminan |
| 9-17 s | "Cada coeficiente era un número de 32 bits. Para que corra rápido en el chip, lo guardamos como un entero de 8 bits por una escala: cuatro veces más chico y con cuentas enteras." | 32 BITS → 8 BITS | Animación: `w = 0,4213` → `q = round(w / 0,005) = 84` → `84 × 0,005 = 0,420` |
| 17-22 s | "Y el S3 tiene instrucciones que multiplican 16 de esos enteros de una vez." | 16 A LA VEZ | Animación: registro de 128 bits partido en 16 casilleros de 8 bits |
| 22-29 s | "Resultado: cada 30 ms de audio, la red decide en 2 ms si dijiste 'Órbita'." | 30 ms → 2 ms | Log de la terminal con los tiempos; la placa escuchando |
| 29-35 s | "En el próximo: cómo la entrenamos sin grabar a nadie." | SIN GRABAR A NADIE | Vos a cámara |

~95 palabras habladas (~35 s).

## Antes de filmar
- [ ] **Red y 8 bits:** 26.049 coeficientes = "Total params" de `entrenamiento.log` (101,75 KB en 32 bits: entraba en la RAM del S3; se achicó por velocidad, no por espacio). La fórmula completa es `real = escala × (entero − cero)` (`entrenamiento.md` §2.8 y §5.3); la animación la simplifica sin el "cero".
- [ ] **16 a la vez:** instrucciones PIE del ESP32-S3 (Technical Reference Manual, cap. 1: registros de 128 bits = 16 enteros de 8 bits). El firmware las usa vía `esp-nn` (`CONFIG_NN_OPTIMIZED=y` en `sdkconfig`), en las convoluciones.
- [ ] **"2 ms":** `Invoke` tarda 2,1 ms como máximo (`wakeword.md`, "Tiempos máximos"); con el cálculo de las features son 5,7 ms por cada 30 ms. Filmar el log que lo muestra.
- [ ] **"Menos de 5 dólares":** el módulo ESP32-S3-WROOM-1-N16R8 sale US$3,44 en LCSC (C2913202) por cantidad y ~US$6 por unidad suelta. Revisar el precio antes de publicar.
- [ ] El CTA apunta al B3: publicar este antes, o cambiar el CTA.

## Caption
> 26.049 coeficientes, de 32 a 8 bits, y un chip que multiplica 16 a la vez: así entra nuestra wake word "Órbita" en un ESP32-S3. #tinyml #esp32 #machinelearning #electronica #buildinpublic

**Portada:** la placa con "CHIP < US$5".
