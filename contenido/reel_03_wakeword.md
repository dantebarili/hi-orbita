# Reel B3 — "Más ≠ mejor"

**Duración:** ~40 s · **Meta:** compartidos y comentarios · **Formato:** fail → arreglo, con demo en vivo.
Fuentes: `orbita-obsidian/wakeword.md`, commits `e17692c` y `61be89e`, `orbita-training/docs/dataset.md`.

## Hooks (probar 2-3)
1. "El modelo que más entrené fue el que peor anduvo." ← elegido
2. "Entrené una IA 4 veces más. Dejó de entenderme."
3. "En la computadora andaba perfecto. En la placa, sordo."
4. "Le enseñé a una placa la palabra 'Órbita' con 11.000 voces falsas."

## Guion

| Tiempo | Dicho | Texto en pantalla | Imagen |
|---|---|---|---|
| 0-3 s | "El modelo que más entrené fue el que peor anduvo." | MÁS ≠ MEJOR | Vos diciendo "Órbita" a la placa; no pasa nada, el aro LED sigue en reposo |
| 3-10 s | "Quiero que esta placa se despierte cuando decís 'Órbita'. Sin internet: la red neuronal corre adentro del chip." | CORRE EN EL CHIP | Primer plano del ESP32-S3; el log de la terminal corriendo |
| 10-17 s | "Para enseñarle, generé más de 11.000 'Órbita' con voces argentinas hechas con IA. Ninguna persona real." | 11.727 VOCES IA | Montaje rápido de clips de ElevenLabs sonando uno tras otro |
| 17-24 s | "La entrené 45.000 pasos. En la compu, impecable. En la placa, con gente de verdad, no me detectaba." | 45.000 PASOS → SORDO | Gráfico de entrenamiento; después vos repitiendo "Órbita" sin respuesta |
| 24-31 s | "Volví a una versión de mucho antes, la del paso 11.000. Esa sí." | PASO 11.000 ✓ | Decís "Órbita" y el aro LED cambia de color |
| 31-36 s | "90 % de aciertos y cero disparos falsos en más de 5 horas de charla en español." | 90 % · 0 FALSOS EN 5 H | Números grandes sobre la placa encendida |
| 36-40 s | "Ahora la pruebo a tres metros, con la tele prendida. En el próximo te muestro." | ¿A 3 METROS? | Vos alejándote de la placa |

~115 palabras habladas: leer en voz alta y recortar a ~95.

## Antes de filmar
- [ ] **Confirmar en `resultados_v0.md` (no está en ninguno de los dos repos locales) que el 90,3 % y los 0 disparos en 5,3 h son del modelo de 11.000 pasos, y sobre qué audio se midió el 90,3 %.** Si no se confirma, sacar el tramo 31-36 s.
- [ ] **No decir por qué falló el de 45.000** salvo que esté medido. La sospecha (memorizó las voces de IA) va como pregunta en la caption, no como afirmación.
- [ ] Filmar el "antes" de verdad: flashear el `orbita_45k_int8.tflite` (cutoff 0,8) y grabar que no detecta. Después flashear el de 11.000.
- [ ] El CTA de los 3 m va solo si esa prueba se va a hacer (está pendiente en `wakeword.md`, "Verificación" 3).
- [ ] No mostrar claves de ElevenLabs ni `cloud_config.py`.

## Caption
> Entrenamos nuestra propia wake word, "Órbita", para un ESP32-S3 con más de 11.000 voces generadas con IA. El modelo más entrenado no reconocía voces reales; volvimos a uno de mucho antes y anduvo. ¿Por qué creés que pasó? #tinyml #esp32 #machinelearning #electronica #buildinpublic

**Portada:** la placa con "MÁS ≠ MEJOR".
