# Reel B3 — "Sin ningún clip real"

**Duración:** ~48 s · **Meta:** compartidos y comentarios · **Formato:** fail → explicación con gráficos → CTA.
Fuentes: `orbita-dataset/docs/resultados_v0.md` (validación, "Robustez", "Con grabaciones reales"), `orbita-dataset/docs/dataset.md` (positivos 100 % sintéticos), `orbita-obsidian/wakeword.md`.

## Hooks (probar 2-3)
1. "Entrenamos una IA para que se despierte cuando escuche la palabra 'Órbita' y sin ningún clip real." ← elegido
2. "El modelo que más entrenamos fue el que peor anduvo."
3. "Entrenamos una IA 4 veces más. Se volvió demasiado segura."

## Guion

| Tiempo | Dicho | Texto en pantalla | Imagen |
|---|---|---|---|
| 0-4 s | "Entrenamos una IA para que se despierte cuando escuche la palabra 'Órbita' y sin ningún clip real." | 0 VOCES REALES | Vos diciendo "Órbita" a la placa; el aro LED despierta |
| 4-11 s | "No queríamos mandar a internet todo lo que se habla en un consultorio: sale caro y es privado. Por eso la escucha corre en un chip de menos de 5 dólares." | SIN INTERNET · CHIP < US$5 | Primer plano del ESP32-S3 |
| 11-15 s | "Para enseñarle usamos solo 11.727 voces argentinas hechas con IA." | 11.727 VOCES IA | Montaje rápido de clips de ElevenLabs |
| 15-21 s | "La entrenamos 45.000 pasos. Con sus ejemplos, el error no paró de bajar." | ERROR ÷ 13 | **Gráfico 1:** `1_error_entrenamiento.png` |
| 21-29 s | "Pero con audio que nunca había escuchado, entrenarla más la empeoró. Eso es overfitting: memorizó en vez de aprender." | OVERFITTING · NOTA 0,86 → 0,70 | **Gráfico 2:** `2_nota_validacion.png` |
| 29-36 s | "Y se volvió demasiado segura: con puro ruido, la de 45.000 llegó a estar 99,5 % segura de escuchar 'Órbita'. La de 11.000, 41 %." | 99,5 % vs 41 % | **Gráfico 3:** `3_certeza_ruido.png`; o la demo del clip de ruido disparando el LED |
| 36-41 s | "Hoy en la placa está la del paso 11.000: con ruido casi no se dispara." | PASO 11.000 ✓ | La llamás y el aro LED cambia de color |
| 41-48 s | "¿El problema de fondo? Con voces reales detecta 1 de cada 3. Para la próxima versión, ¿nos mandás un audio diciendo 'Órbita'?" | 11 DE 30 · ¿NOS MANDÁS TU "ÓRBITA"? | Vos frente a la placa, a cámara |

~130 palabras habladas (~48 s): si queda largo, acortar el hook o el tramo 29-36 s.

## Antes de filmar
- [x] **Gráficos** en `graficos_reel03/` (1080×1350): `python graficos.py <entrenamiento.log>` (el log está en `orbita-dataset/train_v0/run_orbita.tgz`). El error es la media móvil de 250 pasos (0,032 en el paso 1.000 → 0,0024); la "Tendencia" del gráfico 2 es la media de 9 evaluaciones. El 3 sale de "Robustez", punto 4 (máximo de cada modelo en MUSAN `noise`), no del log.
- [ ] **Demo del gráfico 3:** ubicar el clip que dio 0,995 y reproducírselo a la placa con el 45.000 flasheado.
- [ ] **No decir que el 45.000 "no oye voces reales":** medido afuera de la placa detecta más que el 11.000 (17 contra 11 de 30). El overfitting que está medido son las falsas activaciones de alta confianza y la caída del avr.
- [ ] **Privacidad:** el equipo escucha siempre, pero solo en el chip; a internet manda recién con la wake word, más los 3 s previos (`protocolo.md`, "Política de captura"). No decir "no escucha" ni "no guarda nada".
- [ ] **"Menos de 5 dólares":** el módulo ESP32-S3-WROOM-1-N16R8 sale US$3,44 en LCSC (C2913202) por cantidad y ~US$6 por unidad suelta. Revisar el precio antes de publicar.
- [ ] **CTA de audios:** decir para qué se usan (entrenar y probar la v1), pedir permiso explícito y definir por dónde se mandan antes de publicar.
- [ ] No mostrar claves de ElevenLabs ni `cloud_config.py`.

## Caption
> Entrenamos nuestra wake word "Órbita" para un ESP32-S3 solo con voces generadas con IA. Entrenarla más la empeoró: overfitting. Para la v1 necesitamos voces de verdad: ¿nos mandás la tuya? #tinyml #esp32 #machinelearning #electronica #buildinpublic

**Portada:** la placa con "0 VOCES REALES".
