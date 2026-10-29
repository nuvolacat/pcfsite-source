#!/bin/bash
# Firma con Authenticode el .exe y las DLL de la carpeta de Windows que deja app/win/empaquetar.sh y rehace su zip.
# La firma se hace en una máquina Windows con el certificado de firma de código y signtool (por SSH: el certificado
# en la nube no sale de allí). Sin certificado no se firma nada y el zip queda como estaba.
#
#   FIRMA_SSH=<host> FIRMA_HUELLA=<SHA-1 del certificado> app/win/firmar.sh [x86_64] [aarch64]
#
# FIRMA_SELLO: servidor de sello de tiempo RFC 3161 (por defecto el de Certum).
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${FIRMA_SSH:?falta FIRMA_SSH (la máquina Windows que firma)}"
: "${FIRMA_HUELLA:?falta FIRMA_HUELLA (huella SHA-1 del certificado)}"
SELLO="${FIRMA_SELLO:-http://time.certum.pl}"
ARCHS="${*:-x86_64 aarch64}"
VERSION=$(cat app/VERSION)
REMOTO='C:\pcfsite-firma'
for a in $ARCHS; do
  B="core/build/win-$a"; D="$B/PCFSite - Source"
  [ -d "$D" ] || { echo "no está $D: ejecuta antes app/win/empaquetar.sh"; exit 1; }
  # por scp (la consola de OpenSSH en Windows es PowerShell, que no pasa bien binarios por una tubería) y sin espacios
  # en las rutas: el lanzador va como lanzador.exe
  T=$(mktemp -d); mkdir "$T/f"
  cp "$D/PCFSite - Source.exe" "$T/f/lanzador.exe"; cp "$D/SDL3.dll" "$T/f/"; cp -R "$D/programas" "$T/f/"
  echo "== $a: subiendo"
  ssh "$FIRMA_SSH" "powershell -NoProfile -Command \"Remove-Item -Recurse -Force $REMOTO -ErrorAction SilentlyContinue; New-Item -ItemType Directory $REMOTO | Out-Null\""
  scp -q -r "$T/f/lanzador.exe" "$T/f/SDL3.dll" "$T/f/programas" "$FIRMA_SSH:C:/pcfsite-firma/"
  cat > "$B/firmar.ps1" <<EOF
\$ErrorActionPreference = 'Stop'
\$signtool = Get-ChildItem "\${env:ProgramFiles(x86)}\\Windows Kits\\10\\bin\\*\\x64\\signtool.exe" | Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
\$f = @("$REMOTO\\lanzador.exe", "$REMOTO\\SDL3.dll") + (Get-ChildItem "$REMOTO\\programas\\*.dll" | ForEach-Object FullName)
& \$signtool sign /sha1 $FIRMA_HUELLA /tr $SELLO /td sha256 /fd sha256 /d "PCFSite - Source" /du https://source.pcfsite.com \$f
if (\$LASTEXITCODE -ne 0) { exit 1 }
& \$signtool verify /pa /q \$f
exit \$LASTEXITCODE
EOF
  scp -q "$B/firmar.ps1" "$FIRMA_SSH:C:/pcfsite-firma/firmar.ps1"; rm -f "$B/firmar.ps1"
  echo "== $a: firmando"
  ssh "$FIRMA_SSH" "powershell -NoProfile -ExecutionPolicy Bypass -File $REMOTO\\firmar.ps1" | tail -3
  echo "== $a: bajando"
  rm -rf "$T/f"; mkdir "$T/f"
  scp -q -r "$FIRMA_SSH:C:/pcfsite-firma/lanzador.exe" "$FIRMA_SSH:C:/pcfsite-firma/SDL3.dll" "$FIRMA_SSH:C:/pcfsite-firma/programas" "$T/f/"
  n=$(ls "$T/f/programas" | wc -l); m=$(ls "$D/programas" | wc -l)
  [ "$n" -eq "$m" ] || { echo "han vuelto $n DLL de $m"; exit 1; }
  cp "$T/f/lanzador.exe" "$D/PCFSite - Source.exe"; cp "$T/f/SDL3.dll" "$D/"; cp "$T/f/programas/"* "$D/programas/"
  rm -rf "$T"
  case "$a" in x86_64) SUF=x64 ;; aarch64) SUF=arm64 ;; *) SUF=$a ;; esac
  Z="$(pwd)/$B/PCFSite-Source-$VERSION-windows-$SUF.zip"
  cp app/win/LEEME.txt "$D/LEEME.txt"
  rm -f "$Z"
  (cd "$B" && zip -qr "$Z" "PCFSite - Source")
  echo "$(basename "$Z"): firmado ($(du -sh "$Z" | cut -f1))"
done
