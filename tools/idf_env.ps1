# Dot-source to load ESP-IDF into the current PowerShell session.
#   . .\tools\idf_env.ps1
if (-not $env:IDF_PATH -or -not (Test-Path $env:IDF_PATH)) {
    $env:IDF_PATH = Join-Path $env:USERPROFILE 'esp\v5.5.6\esp-idf'
}
if (-not $env:IDF_PYTHON_ENV_PATH -or -not (Test-Path $env:IDF_PYTHON_ENV_PATH)) {
    $env:IDF_PYTHON_ENV_PATH = Join-Path $env:USERPROFILE '.espressif\python_env\idf5.5_py3.12_env'
}
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    . (Join-Path $env:IDF_PATH 'export.ps1') *> $null
    $ErrorActionPreference = $prev
}
