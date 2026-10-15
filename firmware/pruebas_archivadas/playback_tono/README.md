# Prueba local del parlante (archivada)

Tono de 1 kHz por `orbita_playback` (begin/push/end/stop), sin mics ni red. Fuera del build; probada en el banco el 15/10.

Reactivar (desde la raíz del repo):
1. `git apply firmware/pruebas_archivadas/playback_tono/enganche_main.patch` (main.c, CMakeLists, Kconfig, .gitignore).
2. Copiar `test_playback.c` y `test_playback.h` a `firmware/main/` y `sdkconfig.playback` a `firmware/`.
3. Desde `firmware/`: `idf.py -B build_play -D SDKCONFIG=build_play/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.playback" -p COMx flash monitor`

Volumen real = `ORBITA_PLAY_GAIN_Q15` en `orbita_playback.h` (el tono sale a 4000 con 8192).
