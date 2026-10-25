"""
Referencia en PC de la wake word del chip (microWakeWord): mismas features que el
entrenamiento (pymicro-features) + el modelo .tflite en streaming + la decision de ESPHome.
Sirve para:
  - ver que espera un modelo (forma, escala y cero de entrada/salida, operaciones)  -> info
  - correr un WAV y ver probabilidades y detecciones, antes de tocar el firmware    -> run
  - volcar las features int8 para compararlas bit a bit con las del chip           -> run --dump

Uso:
  python wakeword_ref.py info okay_nabu.tflite
  python wakeword_ref.py run okay_nabu.tflite toma.wav [--json okay_nabu.json] [--canal 0] [--dump f.txt]
  python wakeword_ref.py comparar log_chip.txt f.txt
Requisitos: pip install numpy scipy pymicro-features==2.0.2 "tensorflow-cpu>=2.16"
"""

import argparse
import json
import sys

import numpy as np
from scipy.io import wavfile

FS = 16000
MUESTRAS_PASO = 160            # 10 ms: lo que consume el frontend por llamada (audio_utils.py)
FLOAT32_SCALE = 0.0390625      # pymicro-features: feature = uint16 * 1/25.6
IGNORAR_INICIAL = 100          # ESPHome MIN_SLICES_BEFORE_DETECTION (en inferencias)


def cargar_interprete(ruta_modelo):
    import tensorflow as tf
    # Kernels de referencia y sin XNNPACK: mas parecidos a TFLite Micro y la lista de ops sale limpia.
    it = tf.lite.Interpreter(model_path=ruta_modelo,
                             experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    it.allocate_tensors()
    return it


def info(ruta_modelo):
    it = cargar_interprete(ruta_modelo)
    for nombre, det in (("entrada", it.get_input_details()[0]), ("salida", it.get_output_details()[0])):
        esc, cero = det["quantization"]
        print(f"{nombre}: forma={list(det['shape'])} tipo={det['dtype'].__name__} escala={esc!r} cero={cero}")
    ops = sorted({op["op_name"] for op in it._get_ops_details()})
    print(f"operaciones ({len(ops)}): {', '.join(ops)}")


def leer_wav(ruta, canal):
    fs, x = wavfile.read(ruta)
    if fs != FS:
        sys.exit(f"El WAV es de {fs} Hz; tiene que ser {FS} Hz.")
    if x.dtype != np.int16:
        sys.exit(f"El WAV es {x.dtype}; tiene que ser int16 (como entra al frontend del chip).")
    return x[:, canal] if x.ndim == 2 else x


def features_uint16(audio):
    """Mismo bucle que microwakeword/audio/audio_utils.py (camino C): 160 muestras por llamada."""
    from pymicro_features import MicroFrontend
    fe = MicroFrontend()
    datos = audio.tobytes()
    filas, i = [], 0
    while i + MUESTRAS_PASO * 2 < len(datos):
        r = fe.process_samples(datos[i:i + MUESTRAS_PASO * 2])
        i += r.samples_read * 2
        if r.features:
            # El float es uint16 * 1/25.6, exacto en float32: se recupera el entero que da el C.
            filas.append(np.rint(np.asarray(r.features) / FLOAT32_SCALE).astype(np.int32))
    return np.array(filas)


def a_int8(f_u16, escala, cero):
    """uint16 -> int8 con la cuantizacion del modelo, redondeando (como el op Quantize de TFLite).
    En float32 y con el factor precalculado, igual que cuantizar() de wakeword.c: asi el test bit a bit
    no difiere por redondeos distintos entre PC y chip."""
    factor = np.float32(FLOAT32_SCALE) / np.float32(escala)
    q = np.rint(f_u16.astype(np.float32) * factor).astype(np.int32) + cero
    return np.clip(q, -128, 127).astype(np.int8)


def run(ruta_modelo, ruta_wav, ruta_json, canal, ruta_dump):
    cutoff, ventana = 0.97, 5
    if ruta_json:
        micro = json.load(open(ruta_json))["micro"]
        cutoff, ventana = micro["probability_cutoff"], micro["sliding_window_size"]
    cutoff_u8 = int(cutoff * 255)              # ESPHome guarda el cutoff como uint8 (0-255)

    audio = leer_wav(ruta_wav, canal)
    rms = np.sqrt(np.mean(audio.astype(np.float64) ** 2))
    pico = np.max(np.abs(audio.astype(np.int32)))
    print(f"audio: {len(audio) / FS:.2f} s  RMS={20 * np.log10(max(rms, 1e-9) / 32768):.1f} dBFS  "
          f"pico={20 * np.log10(max(pico, 1) / 32768):.1f} dBFS")

    it = cargar_interprete(ruta_modelo)
    ent, sal = it.get_input_details()[0], it.get_output_details()[0]
    esc_in, cero_in = ent["quantization"]
    _, cero_out = sal["quantization"]
    stride = int(ent["shape"][1])

    f_u16 = features_uint16(audio)
    f_i8 = a_int8(f_u16, esc_in, cero_in)
    # Lo que haria la formula fija de ESPHome: si difiere, el modelo no tiene la escala que asume.
    esphome = np.clip((f_u16 * 256 + 333) // 666 - 128, -128, 127).astype(np.int8)
    print(f"features: {len(f_i8)} columnas x 40, stride={stride}, escala_in={esc_in!r} cero_in={cero_in}; "
          f"distintas de la formula de ESPHome: {np.count_nonzero(f_i8 != esphome)}")

    # Streaming: el estado interno del modelo vive en el interprete entre Invoke.
    probs, ultimas, ignorar, detecciones = [], [], -IGNORAR_INICIAL, []
    for k in range(0, len(f_i8) - stride + 1, stride):
        it.set_tensor(ent["index"], f_i8[k:k + stride].reshape(ent["shape"]))
        it.invoke()
        p_u8 = int(it.get_tensor(sal["index"]).reshape(-1)[0])
        probs.append(p_u8)
        ultimas = (ultimas + [p_u8])[-ventana:]
        if p_u8 < cutoff_u8:
            ignorar = min(ignorar + 1, 0)
        if ignorar == 0 and len(ultimas) == ventana and sum(ultimas) > cutoff_u8 * ventana:
            t = (k + stride) * 0.010
            detecciones.append(t)
            ultimas, ignorar = [], -IGNORAR_INICIAL

    p = (np.array(probs, dtype=np.float32) - cero_out) / 255.0
    print(f"inferencias: {len(p)}  prob max={p.max():.3f} en t={(np.argmax(p) + 1) * stride * 0.010:.2f} s  "
          f"cutoff={cutoff} ventana={ventana}")
    print(f"detecciones: {len(detecciones)} {['%.2f s' % t for t in detecciones]}")
    if detecciones == [] and len(p) <= IGNORAR_INICIAL:
        print(f"ojo: con menos de {IGNORAR_INICIAL} inferencias ({IGNORAR_INICIAL * stride * 10} ms) "
              "nunca se detecta (espera inicial de ESPHome)")

    if ruta_dump:
        with open(ruta_dump, "w") as f:
            for fila in f_i8:
                f.write(" ".join(str(int(v)) for v in fila) + "\n")
            f.write("# probs_u8 " + " ".join(map(str, probs)) + "\n")
        print(f"volcado: {ruta_dump}")


def comparar(ruta_log, ruta_dump):
    """Log del chip (lineas 'F i v0..v39' y 'P k p', con CONFIG_ORBITA_WW_PRUEBA_WAV) vs volcado de `run --dump`."""
    chip_f, chip_p = [], []
    for linea in open(ruta_log, errors="replace"):
        t = linea.split()
        if len(t) == 42 and t[0] == "F":
            chip_f.append([int(v) for v in t[2:]])
        elif len(t) == 3 and t[0] == "P":
            chip_p.append(int(t[2]))
    pc_f, pc_p = [], []
    for linea in open(ruta_dump):
        if linea.startswith("# probs_u8"):
            pc_p = [int(v) for v in linea.split()[2:]]
        elif linea.strip():
            pc_f.append([int(v) for v in linea.split()])
    n = min(len(chip_f), len(pc_f))
    if n == 0:
        sys.exit("No hay features para comparar (¿el log tiene las lineas 'F ...'?)")
    d = np.abs(np.array(chip_f[:n]) - np.array(pc_f[:n]))
    m = min(len(chip_p), len(pc_p))
    dp = np.abs(np.array(chip_p[:m]) - np.array(pc_p[:m])) if m else np.array([0])
    print(f"features: chip={len(chip_f)} pc={len(pc_f)} comparadas={n}  "
          f"identicas={100 * np.mean(d == 0):.2f} %  dif max={d.max()} LSB")
    if d.max() > 0:
        i = int(np.argmax(d.max(axis=1) > 0))
        print(f"  primera diferencia en la columna {i}")
    print(f"probabilidades: chip={len(chip_p)} pc={len(pc_p)}  dif max={dp.max()}/255")
    # El bucle de audio_utils.py deja sin procesar el ultimo bloque de 160 (usa `<` estricto): el chip
    # puede dar una columna mas al final. Se aceptan hasta 1 de diferencia en la cantidad.
    ok = abs(len(chip_f) - len(pc_f)) <= 1 and d.max() <= 1 and dp.max() < 2
    print("PASA" if ok else "NO PASA")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("info")
    a.add_argument("modelo")
    b = sub.add_parser("run")
    b.add_argument("modelo")
    b.add_argument("wav")
    b.add_argument("--json", help="manifiesto del modelo (cutoff y ventana)")
    b.add_argument("--canal", type=int, default=0, help="canal del WAV si es estereo (0 = mic L)")
    b.add_argument("--dump", help="archivo donde volcar las features int8 y las probabilidades")
    c = sub.add_parser("comparar")
    c.add_argument("log", help="log del chip con CONFIG_ORBITA_WW_PRUEBA_WAV (idf.py monitor | tee log.txt)")
    c.add_argument("dump", help="volcado de `run --dump` sobre el mismo WAV")
    args = ap.parse_args()
    if args.cmd == "info":
        info(args.modelo)
    elif args.cmd == "comparar":
        comparar(args.log, args.dump)
    else:
        run(args.modelo, args.wav, args.json, args.canal, args.dump)


if __name__ == "__main__":
    main()
