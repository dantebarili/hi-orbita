"""Datos reales para los interactivos de la presentación (sale assets/datos.js).

Uso: python gen/gen_datos.py   (desde contenido/tp_assd/)
- espectro: un "órbita" de ElevenLabs (muestras int16 + features del microfrontend, el mismo C del chip).
- deteccion: charla en español + palabra parecida + "órbita", pasada por el orbita.tflite del firmware
  en streaming (de a 3 columnas), igual que tools/wakeword_ref.py (rama tests+afe).
- entrenamiento: pérdida por lote (suavizada) y validación cada 500 pasos, del entrenamiento.log del v0.
"""
import base64, json, re, sys, tarfile
from pathlib import Path
import numpy as np
from scipy.io import wavfile

AQUI = Path(__file__).resolve().parent.parent
ORBITA = AQUI.parents[2]                      # .../Orbita
DS = ORBITA / "orbita-dataset" / "dataset_FULL"
MODELO = ORBITA / "hi-orbita" / "firmware" / "components" / "orbita_wakeword" / "modelos" / "orbita.tflite"
RUN = ORBITA / "orbita-dataset" / "train_v0" / "run_orbita.tgz"
FS, PASO, ESCALA_F = 16000, 160, 0.0390625
ONSET = 2.15                                   # s donde empieza la palabra en REAL

POS = DS / "positivos/elevenlabs_positivos_clips/eleven_p1_dante___clear__expressive__natural_DzZyY3_0005.wav"
REAL = DS / "fuentes/activaciones_placa/20261101/act_200626_0a3f1ec67a88f7d7.wav"   # "Órbita" grabado por la placa (mic real)
CONF = sorted((DS / "negativos/elevenlabs_palabras_clips").glob("*.wav"))
CHARLA = sorted((DS / "negativos/spanish_conversation").glob("cordeba_*.wav"))


def wav(p):
    fs, x = wavfile.read(p)
    assert fs == FS and x.dtype == np.int16, p
    return x if x.ndim == 1 else x[:, 0]


def features(audio):
    from pymicro_features import MicroFrontend
    fe, datos, filas, i = MicroFrontend(), audio.tobytes(), [], 0
    while i + PASO * 2 < len(datos):
        r = fe.process_samples(datos[i:i + PASO * 2])
        i += r.samples_read * 2
        if r.features:
            filas.append(np.rint(np.asarray(r.features) / ESCALA_F).astype(np.int32))
    return np.array(filas)                     # (columnas, 40) uint16 como en el chip


def probabilidades(f_u16):
    import tensorflow as tf
    it = tf.lite.Interpreter(model_path=str(MODELO),
                             experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    it.allocate_tensors()
    ent, sal = it.get_input_details()[0], it.get_output_details()[0]
    esc, cero = ent["quantization"]
    q = np.clip(np.rint(f_u16.astype(np.float32) * np.float32(ESCALA_F / esc)).astype(np.int32) + cero, -128, 127)
    out = []
    for k in range(0, len(q) - 2, 3):
        it.set_tensor(ent["index"], q[k:k + 3].astype(np.int8).reshape(ent["shape"]))
        it.invoke()
        out.append(int(it.get_tensor(sal["index"]).reshape(-1)[0]))
    return out, {"escala": float(esc), "cero": int(cero)}


def b64(a):
    return base64.b64encode(np.ascontiguousarray(a).tobytes()).decode()


def a_u8(f):                                    # features a 0-255 para dibujar (0..~1000 en uint16)
    return np.clip(f / 4.0, 0, 255).astype(np.uint8)


def ruido_real(n, x):                           # piso de ruido real de la placa (primeros 2 s del clip), repetido
    base = x[: int(2.0 * FS)]
    return np.tile(base, n // len(base) + 1)[:n]


def recortar(x, umbral=600):
    idx = np.nonzero(np.abs(x.astype(np.int32)) > umbral)[0]
    return x[max(idx[0] - 800, 0): idx[-1] + 800]


def main():
    real = wav(REAL)
    sil = ruido_real(int(0.25 * FS), real)
    pos = real[int(ONSET * FS) - 800: int(ONSET * FS) + int(0.75 * FS)]          # "Órbita" real de la placa
    # --- espectro: 1,5 s, palabra cerca del final (como en el entrenamiento)
    clip = np.concatenate([ruido_real(int(1.5 * FS) - len(pos) - len(sil), real), pos, sil])
    pre = ruido_real(int(1.0 * FS), real)                    # el microfrontend arranca con un transitorio: se lo deja pasar con 1 s de ruido y se descarta
    f_clip = features(np.concatenate([pre, clip]))[100:]

    # --- detección: ~4 s de charla, palabra parecida, charla, "órbita", charla
    rng = np.random.default_rng(3)
    def charla(seg):
        out, n = [], 0
        for p in rng.choice(CHARLA, 40, replace=False):
            x = wav(p); out.append(x); n += len(x)
            if n > seg * FS: break
        return np.concatenate(out)[: int(seg * FS)]
    conf = recortar(wav(CONF[37]))
    gan = lambda x, g: np.clip(x.astype(np.float32) * g, -32768, 32767).astype(np.int16)
    largo = np.concatenate([gan(charla(4.5), 0.8), sil, conf, sil, gan(charla(2.0), 0.8), sil, pos, sil,
                            gan(charla(2.0), 0.8)])
    f_largo = features(largo)
    probs, cuant = probabilidades(f_largo)
    marcas = {"parecida": (4.5 * FS + len(sil)) / FS, "orbita": (len(largo) - 2.0 * FS - len(sil) - len(pos)) / FS}

    # --- entrenamiento
    with tarfile.open(RUN) as t:
        log = t.extractfile("run_orbita_20261027_1501/entrenamiento.log").read().decode("utf-8", "ignore")
    perd = np.array([float(m) for m in re.findall(r"Loss = ([0-9.]+); Mini-Batch", log)])
    suave = np.convolve(perd, np.ones(250) / 250, mode="valid")[::100]
    ev = re.findall(r"Step (\d+) \(nonstreaming\).*?recall = ([0-9.]+)%, precision.*?per hour = ([0-9.]+), "
                    r"loss = ([0-9.]+).*?average viable recall = ([0-9.]+)", log)
    val = [[int(a), float(b), float(c), float(d), float(e)] for a, b, c, d, e in ev]

    datos = {
        "espectro": {"fs": FS, "muestras": b64(clip), "n": len(clip), "feat": b64(a_u8(f_clip)), "cols": len(f_clip),
                     "fuente": REAL.name},
        "deteccion": {"feat": b64(a_u8(f_largo)), "cols": len(f_largo), "probs": probs, "cuant": cuant,
                      "marcas": marcas, "conf": CONF[37].name, "dur": len(largo) / FS},
        "entrenamiento": {"perdida": [round(v, 5) for v in suave], "cada": 100, "desde": 250, "val": val},
    }
    (AQUI / "assets").mkdir(exist_ok=True)
    (AQUI / "assets" / "datos.js").write_text("window.DATOS = " + json.dumps(datos) + ";\n", encoding="utf-8")
    print(f"clip {len(clip)/FS:.2f} s, {len(f_clip)} col | largo {len(largo)/FS:.2f} s, {len(probs)} inf, "
          f"pmax {max(probs)} en {np.argmax(probs)*0.03:.2f} s, marcas {marcas} | val {len(val)} evals")
    p = np.array(probs); print("prob>100:", [round(i * 0.03, 2) for i in np.nonzero(p > 100)[0]])


if __name__ == "__main__":
    sys.exit(main())
