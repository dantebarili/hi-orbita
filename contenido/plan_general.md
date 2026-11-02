# Plan de contenido — "Construyendo Órbita"

Building in public **por feature**: cada serie cuenta cómo se construyó una pieza difícil (beamforming, wake word, conversación, aro LED), con la prueba real y el número medido. Nada de internals sin imagen (ring buffer, protocolo): si no se ve ni se oye, no es reel.

Promesa: dos estudiantes de electrónica que no cursaron Embebidos hacen un aparato que escucha al médico. **Objetivo:** seguidores y autoridad técnica. **Cadencia:** 2 reels/semana (IG, TikTok, Shorts), 1 posteo de LinkedIn por serie. Filmar por tandas.

## Fórmula de cada reel (40 s máx.)
1. **0-2 s, el número o el fallo** en pantalla y en movimiento (gráfico, placa, LED). Hook < 12 palabras + texto de 3-5.
2. **La idea** en una frase, con imagen.
3. **El fallo**: lo que no anduvo, medido.
4. **El arreglo** y el antes/después (mejor si se **oye**).
5. **Lo que sigue abierto** + un solo CTA ("en el próximo…").

## Series

| Serie | Reel → hook · texto en pantalla | Material que ya existe |
|---|---|---|
| **A. Beamforming** | A1. *"Dos micrófonos desfasados 54 millonésimas de segundo me arruinaron todo."* · 54 µs → guion en `reel_02_beamforming.md` | Tomas [L, R, BF] por posición, tabla −3,8 → −15 dB (`resumen_beamforming.html`) |
| | A2. *"Restá dos micrófonos y escuchás para un solo lado."* · RESTAR = ENFOCAR | Gráfico interactivo del β (`teoria_hipercardioide.html`) |
| | A3. *"Funciona en una placa. ¿Y en la próxima?"* · ¿CADA PLACA? | Pendiente: medir 2-3 placas |
| **B. Wake word** | B1. *"Le enseñé 'Órbita' a una placa sin grabar a nadie."* · 100 % VOCES IA | Positivos de ElevenLabs con ~85-100 voces argentinas (`orbita-training/docs/positivos.md`) |
| | B2. *"El chip y la PC tienen que dar el mismo número. Exacto."* · BIT A BIT | 697 × 40 features y 232 probabilidades idénticas (`wakeword.md`) |
| | B3. *"El modelo que más entrené fue el que peor anduvo."* · MÁS ≠ MEJOR | 45.000 pasos no detectaba voces reales; volvió el de 11.000 |
| | B4. *"Dije 'Órbita' y se prendió."* · FUNCIONÓ | Demo en vivo + aro LED. Falta medir aciertos a 0,5-3 m |
| **C. Conversación** | C1. *"Le hablé y me contestó."* · HABLÓ. C2. *"¿Cuánto tarda en contestarte?"* · LATENCIA | Servidor STT en vivo. **Es frente de Juani: hablarlo con él antes** |
| **D. Lo que falta** | D1. *"El parlante se escucha a sí mismo."* · EL ECO (AEC). D2. *"Del protoboard a una placa de verdad."* · PCB | Mostrar como en curso |

Orden sugerido: A1, B1, A2, B3, B2, B4 (A1 y B3 son las historias de fallo, las más fuertes). El aro LED es B-roll en todos.

## Reglas
- "Como si fuera ahora" = re-ejecutar la prueba real frente a cámara. No inventar fechas ni resultados.
- Lo en curso (AEC, wake word en distancia, beamforming en varias placas) se muestra como en curso.
- Nunca mostrar `cloud_config.py`, claves de API, audios de terceros ni nada que insinúe pacientes.
- Nombrar a Juani (avisarle antes). Privacidad como argumento solo con la política de captura decidida.
- No afirmar números sin verificar. Si el CTA promete un video, ese video sale.

## Herramientas
Método original: skills de Ootto-AI/claude-content-skills (MIT), hoy **no instaladas**. Para edición: plugin *IT Reelsmaker* (cortes, subtítulos, render 1080×1920).
