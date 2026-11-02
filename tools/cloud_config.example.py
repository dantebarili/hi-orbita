"""Plantilla de configuracion del chat en la nube. servidor_wakeword.py la copia a cloud_config.py (ignorado por Git) la
primera vez; editar ESA copia. Nada mas se pasa por linea de comandos."""

# --- Claves (solo las del proveedor que uses; si queda "", se lee la variable de entorno homonima) ---
GEMINI_API_KEY = ""       # aistudio.google.com/apikey (gratis, sin tarjeta; con limites de uso)
ANTHROPIC_API_KEY = ""    # console.anthropic.com (pago; solo si LLM = "claude")
ELEVENLABS_API_KEY = ""   # elevenlabs.io -> API Keys (STT y voz)

# --- STT en vivo (ElevenLabs) ---
STT_MODEL = "scribe_v2_realtime"
STT_VAD_SILENCE_S = 0.8             # silencio que cierra la frase (ElevenLabs acepta 0.192 a 3.0). Si corta a mitad, subir
STT_CONTEXT = "Charla con el asistente Orbita"   # previous_text de la 1.a sesion (< 50 caracteres); despues, la ultima respuesta

# --- Voz (ElevenLabs) ---
ELEVENLABS_VOICE_ID = "21m00Tcm4TlvDq8ikWAM"
ELEVENLABS_TTS_MODEL = "eleven_flash_v2_5"

# --- Que modelo conversa ---
LLM = "gemini"                      # "gemini" (gratis) o "claude" (pago)
GEMINI_MODEL = "gemini-3.5-flash-lite"   # 01/11: 0,8 s de mediana (el 3.1-flash-lite se saturaba: 4-13 s)
GEMINI_RESPALDO = ["gemini-flash-lite-latest", "gemini-3.5-flash"]   # si el principal tarda > 6 s o da 429/503
CLAUDE_MODEL = "claude-haiku-5-5"

# --- Servidor y conversacion ---
HOST = "0.0.0.0"
PORT = 8765
SAVE_AUDIO = False                  # True: guarda en tools/prueba_websocket el WAV de cada pregunta y respuesta
SYSTEM_PROMPT = (
    "Sos un asistente de voz amistoso para charlar. Tu respuesta se lee en voz alta: "
    "espanol rioplatense, maximo dos frases cortas, sin listas, sin markdown, sin emojis."
)
