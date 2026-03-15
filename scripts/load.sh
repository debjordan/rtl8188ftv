#!/usr/bin/env bash
# load.sh — Carrega/descarrega o driver RTL8188FTV
#
# Uso:
#   ./scripts/load.sh          → compila e carrega
#   ./scripts/load.sh unload   → descarrega
#   ./scripts/load.sh reload   → descarrega, recompila e carrega
#   ./scripts/load.sh status   → mostra estado atual

set -euo pipefail

MODULE="rtl8188ftv"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# ── Verifica root ────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "[ERRO] Este script requer permissão root."
    echo "       Execute: sudo $0 $*"
    exit 1
fi

# ── Funções ──────────────────────────────────────────────────────────────────

build() {
    echo "[BUILD] Compilando módulo..."
    cd "$ROOT_DIR"
    make clean
    make
    echo "[BUILD] OK — ${MODULE}.ko gerado."
}

do_unload() {
    if lsmod | grep -q "^${MODULE}"; then
        echo "[UNLOAD] Removendo módulo ${MODULE}..."
        rmmod "$MODULE"
        echo "[UNLOAD] OK."
    else
        echo "[UNLOAD] Módulo não está carregado."
    fi
}

do_load() {
    if lsmod | grep -q "^${MODULE}"; then
        echo "[LOAD] Módulo já está carregado."
        return
    fi

    if [[ ! -f "$ROOT_DIR/${MODULE}.ko" ]]; then
        echo "[LOAD] .ko não encontrado, compilando..."
        build
    fi

    echo "[LOAD] Carregando módulo ${MODULE}..."
    insmod "$ROOT_DIR/${MODULE}.ko"

    # Aguarda udev processar o hotplug
    sleep 1

    # Verifica se a interface apareceu
    IFACE=$(iw dev 2>/dev/null | awk '/Interface/{print $2}' | head -1)
    if [[ -n "$IFACE" ]]; then
        echo "[LOAD] OK — Interface detectada: $IFACE"
    else
        echo "[LOAD] Módulo carregado, mas interface ainda não apareceu."
        echo "       Verifique: dmesg | tail -30"
    fi
}

do_status() {
    echo "=== Status do Driver ${MODULE} ==="
    echo ""
    echo "── Módulo ──────────────────────────────"
    if lsmod | grep -q "^${MODULE}"; then
        lsmod | grep "^${MODULE}"
    else
        echo "Não carregado."
    fi

    echo ""
    echo "── Dispositivo USB ─────────────────────"
    lsusb | grep -i "0bda:f179" || echo "RTL8188FTV não detectado via USB."

    echo ""
    echo "── Interface WiFi ──────────────────────"
    iw dev 2>/dev/null || echo "Nenhuma interface wifi disponível."

    echo ""
    echo "── Kernel log recente ──────────────────"
    dmesg | grep -i "${MODULE}\|rtl8188\|f179" | tail -10 || true
}

# ── Dispatch ─────────────────────────────────────────────────────────────────

case "${1:-load}" in
    load)    build; do_load ;;
    unload)  do_unload ;;
    reload)  do_unload; build; do_load ;;
    status)  do_status ;;
    build)   build ;;
    *)
        echo "Uso: $0 [load|unload|reload|status|build]"
        exit 1
        ;;
esac
