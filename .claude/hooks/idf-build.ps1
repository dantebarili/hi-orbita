# Compila firmware/ activando ESP-IDF por su cuenta (no depende del PATH).
# Sale con 3 si no encuentra una instalacion de ESP-IDF (el hook lo trata como "saltar").
$env:MSYSTEM = $null
# Con la salida redirigida Python usa cp1252 y el movemodel.py de esp-sr (imprime '─') revienta.
$env:PYTHONUTF8 = '1'

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $tools = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { 'C:\Espressif' }
    if (-not (Test-Path "$tools\frameworks")) { exit 3 }
    $env:IDF_TOOLS_PATH = $tools
    if (-not $env:IDF_PATH) {
        $fw = Get-ChildItem "$tools\frameworks" -Directory -Filter 'esp-idf-*' | Sort-Object Name | Select-Object -Last 1
        if (-not $fw) { exit 3 }
        $env:IDF_PATH = $fw.FullName
    }
    # export.ps1 busca el venv segun el python del PATH: hay que anteponer el Python propio del IDF
    # (si el del sistema es otro, ej. 3.14, no encuentra el venv).
    $py = Get-ChildItem "$tools\tools\idf-python" -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if ($py) { $env:PATH = "$($py.FullName);$env:PATH" }
    . "$env:IDF_PATH\export.ps1" *>$null
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { exit 3 }
}

idf.py -C firmware build
exit $LASTEXITCODE
