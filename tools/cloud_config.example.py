"""Plantilla de configuracion del chat en la nube. chat_nube.py la copia a cloud_config.py (ignorado por Git) la
primera vez; editar ESA copia. Nada mas se pasa por linea de comandos."""

# --- Claves (alcanza con las del proveedor que uses; si queda "", se lee la variable de entorno homonima) ---
ELEVENLABS_API_KEY = ""   # elevenlabs.io -> perfil -> API keys (plan gratis)
GEMINI_API_KEY = ""       # aistudio.google.com/apikey (gratis, sin tarjeta; con limites de uso)
ANTHROPIC_API_KEY = ""    # console.anthropic.com (pago)

# --- Que modelo conversa ---
LLM = "gemini"                      # "gemini" (gratis) o "claude" (pago)
GEMINI_MODEL = "gemini-2.5-flash"   # si da 429/404, probar otro de https://ai.google.dev/gemini-api/docs/models
CLAUDE_MODEL = "claude-haiku-5-5"

# --- Voz y STT ---
VOICE_ID = "21m00Tcm4TlvDq8ikWAM"   # ElevenLabs: voice_id (Rachel). Cambiar por la voz que quieras
TTS_MODEL = "eleven_flash_v2_5"
STT = "elevenlabs"                  # "elevenlabs" (Scribe) o "local" (Whisper en la PC; pide faster-whisper)
STT_MODEL = "scribe_v1"             # solo para STT = "elevenlabs"
LOCAL_WHISPER_MODEL = "tiny"        # solo para STT = "local"

# --- Servidor y conversacion ---
HOST = "0.0.0.0"
PORT = 8765
SAVE_AUDIO = False                  # True: guarda en tools/prueba_websocket el WAV de cada pregunta y respuesta
SYSTEM_PROMPT = (
    "Sos un asistente de voz amistoso para charlar. Tu respuesta se lee en voz alta: "
    "espanol rioplatense, maximo dos frases cortas, sin listas, sin markdown, sin emojis."
)
