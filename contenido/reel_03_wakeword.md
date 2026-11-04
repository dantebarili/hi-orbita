# Reel B3 — "Sin grabar a nadie"

**Duración:** ~40 s · **Meta:** compartidos y comentarios · **Formato:** fail → arreglo, con demo en vivo.
Fuentes: `orbita-obsidian/wakeword.md`, commits `e17692c` y `61be89e`, `orbita-training/docs/dataset.md`.

## Hooks (probar 2-3)
1. "Entrenamos una IA para reconocer 'Órbita' sin grabar a nadie diciéndola." ← elegido
2. "El modelo que más entrenamos fue el que peor anduvo."
3. "Entrenamos una IA 4 veces más. Dejó de entendernos."
4. "En la computadora andaba perfecto. En la placa, sorda."

## Guion

| Tiempo | Dicho | Texto en pantalla | Imagen |
|---|---|---|---|
| 0-3 s | "Entrenamos una IA para reconocer 'Órbita' sin grabar a nadie diciéndola." | 0 VOCES REALES | Vos diciendo "Órbita" a la placa; el aro LED despierta |
| 3-10 s | "Queremos que solo escuche cuando la llamás. Por eso la detección corre adentro del chip, sin internet." | CORRE EN EL CHIP | Primer plano del ESP32-S3; el log de la terminal corriendo |
| 10-17 s | "Para enseñarle, generamos más de 11.000 'Órbita' con voces argentinas hechas con IA." | 11.727 VOCES IA | Montaje rápido de clips de ElevenLabs sonando uno tras otro |
| 17-24 s | "La entrenamos 45.000 pasos. En la compu, impecable. En la placa, con gente de verdad, sorda." | 45.000 PASOS → SORDA | Gráfico de entrenamiento; después vos repitiendo "Órbita" sin respuesta |
| 24-31 s | "Volvimos a una versión de mucho antes, la del paso 11.000. Esa sí." | PASO 11.000 ✓ | Decís "Órbita" y el aro LED cambia de color |
| 31-36 s | "90 % de aciertos y cero disparos falsos en más de 5 horas de charla en español." | 90 % · 0 FALSOS EN 5 H | Números grandes sobre la placa encendida |
| 36-40 s | "Ahora la probamos a tres metros, con la tele prendida. En el próximo te mostramos." | ¿A 3 METROS? | Vos alejándote de la placa |

~95 palabras habladas: leer en voz alta y ajustar.

## Antes de filmar
- [ ] **Confirmar en `dataset.md` que ningún positivo del entrenamiento es una grabación real.** Si hay alguno, el hook pasa a "casi sin grabar a nadie" o se usa otro.
- [ ] **Confirmar en `resultados_v0.md` (no está en ninguno de los dos repos locales) que el 90,3 % y los 0 disparos en 5,3 h son del modelo de 11.000 pasos, y sobre qué audio se midió el 90,3 %.** Si no se confirma, sacar el tramo 31-36 s.
- [ ] **No decir por qué falló el de 45.000** salvo que esté medido. La sospecha (memorizó las voces de IA) va como pregunta en la caption, no como afirmación.
- [ ] Filmar el "antes" de verdad: flashear el `orbita_45k_int8.tflite` (cutoff 0,8) y grabar que no detecta. Después flashear el de 11.000.
- [ ] El CTA de los 3 m va solo si esa prueba se va a hacer (está pendiente en `wakeword.md`, "Verificación" 3).
- [ ] No mostrar claves de ElevenLabs ni `cloud_config.py`.

## Caption
> Entrenamos nuestra propia wake word, "Órbita", para un ESP32-S3 con más de 11.000 voces generadas con IA. El modelo más entrenado no reconocía voces reales; volvimos a uno de mucho antes y anduvo. ¿Por qué creés que pasó? #tinyml #esp32 #machinelearning #electronica #buildinpublic

**Portada:** la placa con "0 VOCES REALES".
