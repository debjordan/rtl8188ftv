# Driver USB RTL8188FTV — Projeto Educacional

Driver de kernel Linux para o adaptador WiFi USB **Realtek RTL8188FTV**
(`0bda:f179`), construído do zero em C com foco em aprendizado e documentação.

## Objetivo

Este projeto existe para entender **como drivers USB WiFi funcionam** por
dentro — desde o protocolo USB até a integração com o framework mac80211 do
kernel Linux. Cada arquivo tem comentários extensos explicando os conceitos.

## Hardware alvo

| Propriedade | Valor |
|-------------|-------|
| Chip        | Realtek RTL8188FTV |
| USB VID:PID | `0bda:f179` |
| Padrão WiFi | 802.11b/g/n (2.4 GHz) |
| Antenas     | 1T1R — 1 TX, 1 RX |
| Taxa máx.   | 150 Mbps (HT20 + SGI) |

## Estrutura do repositório

```
rtl8188ftv/
├── Makefile                   # Sistema de build Kbuild
├── include/
│   ├── rtl8188ftv.h           # Estruturas principais e protótipos
│   └── rtl8188ftv_reg.h       # Mapa completo de registradores
├── src/
│   ├── main.c                 # Entry point: tabela USB, module_init/exit
│   ├── usb.c                  # Camada USB: URBs, probe, control transfers
│   ├── hal.c                  # Hardware: power-on, EFUSE, canal, BB
│   └── mac.c                  # mac80211: ieee80211_ops, RX/TX de frames
├── docs/
│   ├── 01-usb-kernel-driver.md   # Conceitos: URBs, contextos, ciclo de vida
│   └── 02-hardware-rtl8188ftv.md # Hardware: registradores, EFUSE, protocolos
├── firmware/                  # Firmware binário do chip (não incluído)
└── scripts/
    └── load.sh                # Compila, carrega e monitora o módulo
```

## Dependências

```bash
# Headers do kernel (Debian/Ubuntu)
sudo apt install linux-headers-$(uname -r)

# Ferramentas de desenvolvimento
sudo apt install build-essential

# Ferramentas WiFi para teste
sudo apt install iw wireless-tools
```

## Compilar e carregar

```bash
# Compilar
make

# Carregar (requer root)
sudo make install
# ou
sudo ./scripts/load.sh load

# Verificar status
./scripts/load.sh status

# Ver logs do kernel
dmesg | grep rtl8188ftv | tail -20

# Descarregar
sudo ./scripts/load.sh unload
```

## Fluxo de aprendizado sugerido

1. **[docs/01-usb-kernel-driver.md](docs/01-usb-kernel-driver.md)**
   — Conceitos base: módulos, URBs, contextos de execução

2. **[src/main.c](src/main.c)**
   — Ponto de entrada: `module_init`, tabela USB

3. **[src/usb.c](src/usb.c)**
   — Probe/disconnect, control transfers, URBs de RX/TX

4. **[docs/02-hardware-rtl8188ftv.md](docs/02-hardware-rtl8188ftv.md)**
   — Hardware: como o chip funciona por dentro

5. **[src/hal.c](src/hal.c)**
   — Power-on, EFUSE, Baseband, canal

6. **[src/mac.c](src/mac.c)**
   — mac80211: ieee80211_ops, entrega de frames

## Arquitetura em camadas

```
┌──────────────── mac80211 (kernel) ───────────────────┐
│  ieee80211_hw → ieee80211_ops → nosso driver          │
└──────────────────────┬───────────────────────────────┘
                       │ callbacks (tx, start, config...)
┌──────────────────────▼───────────────────────────────┐
│                mac.c — MAC Layer                      │
│  monta/desmonta frames 802.11, scan, associação       │
└──────────────────────┬───────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────┐
│                hal.c — Hardware Abstraction           │
│  power-on, EFUSE, registradores MAC, BB, RF           │
└──────────────────────┬───────────────────────────────┘
                       │ control/bulk transfers
┌──────────────────────▼───────────────────────────────┐
│                usb.c — USB Layer                      │
│  URBs, probe/disconnect, I/O assíncrono               │
└──────────────────────┬───────────────────────────────┘
                       │
                 [ RTL8188FTV ]
```

## Registradores-chave implementados

| Registrador  | Endereço | Função                    |
|--------------|----------|---------------------------|
| `REG_CR`     | `0x0100` | Habilita TX/RX do MAC     |
| `REG_RCR`    | `0x0608` | Filtro de recepção        |
| `REG_MACID`  | `0x0610` | Endereço MAC do hardware  |
| `REG_BSSID`  | `0x0618` | BSSID do AP associado     |
| `REG_EFUSE_CTRL` | `0x0030` | Acesso ao EFUSE       |

## Estado atual (WIP)

- [x] Estrutura do projeto e Makefile
- [x] Tabela USB e module_init/exit
- [x] Probe/disconnect USB
- [x] Control transfers (leitura/escrita de registradores)
- [x] URBs de RX (loop assíncrono com múltiplos URBs)
- [x] URBs de TX (bulk OUT assíncrono)
- [x] Registro no mac80211 (ieee80211_register_hw)
- [x] Power-on sequence (APS_FSMCO, SYS_FUNC_EN)
- [x] Leitura do MAC via EFUSE
- [x] Configuração de canal via BB/RF
- [x] RX: entrega de frames ao mac80211
- [x] TX: montagem do TX descriptor
- [ ] Scan de redes (`hw_scan`)
- [ ] Rate control (escolha automática de MCS/taxa)
- [ ] Aggregation A-MPDU
- [ ] Power save (PS-Poll)
- [ ] Criptografia hardware (WPA2/AES)

## Referências

- [linux/drivers/net/wireless/realtek/rtl8xxxu/](https://elixir.bootlin.com/linux/latest/source/drivers/net/wireless/realtek/rtl8xxxu)
  — Driver oficial do kernel, boa referência para protocolo
- [mac80211 subsystem docs](https://www.kernel.org/doc/html/latest/driver-api/80211/mac80211.html)
- [USB urb documentation](https://www.kernel.org/doc/html/latest/driver-api/usb/URB.html)
- `lsusb -v -d 0bda:f179` — descriptores USB do dispositivo
- `dmesg | grep rtl` — log do kernel durante probe/disconnect
