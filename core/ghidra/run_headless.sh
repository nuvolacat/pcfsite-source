#!/bin/zsh
# Analiza binarios de PCF7 con Ghidra headless (FixGaps + exportación completa a $W/decomp).
# Uso: ghidra/run_headless.sh <proyecto> <binario> [<binario>...]
set -e
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
P=${0:A:h}; W=${PCF7_WORK:-$HOME/pcf7-work}
PROJ=$1; shift
mkdir -p $W/ghidra/proj $W/decomp
/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless $W/ghidra/proj $PROJ \
  -import "$@" -overwrite -max-cpu 6 \
  -scriptPath $P/scripts -postScript FixGaps.java -postScript ExportAll.java $W/decomp \
  2>&1 | tee -a $W/ghidra/$PROJ.log
