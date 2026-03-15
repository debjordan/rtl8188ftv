# 02 — Hardware: Realtek RTL8188FTV

## Identificação

| Campo          | Valor                             |
|----------------|-----------------------------------|
| Chip           | RTL8188F (variante FTV)           |
| USB VID:PID    | `0bda:f179`                       |
| USB Version    | 2.0 High-Speed (480 Mbps)         |
| Padrão WiFi    | IEEE 802.11b/g/n                  |
| Frequência     | 2.4 GHz                           |
| Antenas        | 1T1R (1 TX, 1 RX)                 |
| Taxa máx.      | 150 Mbps (HT20, MCS7, SGI)        |
| Consumo        | 500 mA @ 5V (via USB)             |

## Descoberto com `lsusb -v`

```
Bus 001 Device 007: ID 0bda:f179
  idVendor  = 0x0bda  (Realtek Semiconductor)
  idProduct = 0xf179  (RTL8188FTV)
  iProduct  = "802.11n"

  Interface 0: Class=255/255/255 (Vendor Specific)
    EP 0x81 Bulk IN  — 512 bytes/pkt  (dados RX)
    EP 0x02 Bulk OUT — 512 bytes/pkt  (dados TX normal)
    EP 0x03 Bulk OUT — 512 bytes/pkt  (TX alta prioridade)
```

## Arquitetura interna do chip

```
┌─────────────────────────────────────────────────────────┐
│                      RTL8188FTV                          │
│                                                          │
│  ┌─────────┐   ┌─────────┐   ┌──────────┐   ┌───────┐ │
│  │ USB 2.0 │──▶│  DMA    │──▶│   MAC    │──▶│  BB   │ │
│  │ PHY     │◀──│ Engine  │◀──│ 802.11   │◀──│ OFDM/ │ │
│  └─────────┘   └─────────┘   │ hardware │   │  CCK  │ │
│                               └──────────┘   └───┬───┘ │
│  ┌─────────┐                                     │     │
│  │  EFUSE  │◀─── MAC addr,                  ┌───▼───┐ │
│  │ 512 B   │     calibração                 │  RF   │ │
│  └─────────┘                                │ 2.4G  │ │
│                                              └───┬───┘ │
└──────────────────────────────────────────────────┼──────┘
                                                   │
                                             [Antena]
```

## Espaço de endereços de registradores

```
0x0000–0x00FF  Sistema: power, clock, reset, GPIO
0x0100–0x01FF  MAC: Command Register, buffer sizes
0x0200–0x02FF  MAC TX: FIFO, descriptores
0x0400–0x04FF  TX DMA
0x0500–0x05FF  EDCA (filas de QoS: VO/VI/BE/BK)
0x0600–0x06FF  MAC: RCR, TCR, BSSID, MAC addr
0x0800–0x08FF  Baseband (acesso INDIRETO via HSSI)
0xFE00–0xFEFF  USB-específico: interrupções, agregação
```

## Protocolo de acesso a registradores

O chip usa **USB Vendor Control Transfers** no EP0:

### Leitura (`bmRequestType=0xC0`, `bRequest=0x05`)
```
HOST → CHIP:  [Setup: 0xC0 0x05 ADDR_LO ADDR_HI 00 00 LEN 00]
CHIP → HOST:  [DATA: byte0 byte1 ... byteN]
```

### Escrita (`bmRequestType=0x40`, `bRequest=0x05`)
```
HOST → CHIP:  [Setup: 0x40 0x05 ADDR_LO ADDR_HI 00 00 LEN 00]
              [DATA: byte0 byte1 ... byteN]
```

## Formato do pacote TX

Cada frame transmitido via EP 0x02 ou 0x03 tem este formato:

```
┌──────────────────────────────────────────────────────┐
│           TX Descriptor (32 bytes)                   │
│   DWORD0: pktlen, offset, FS, LS                     │
│   DWORD1: macid, queue, rate bitmap                  │
│   DWORD2–7: parâmetros de rate control               │
├──────────────────────────────────────────────────────┤
│           Frame 802.11 (variável)                    │
│   [MAC Header][LLC][Payload][FCS]                    │
└──────────────────────────────────────────────────────┘
```

## Formato do pacote RX

Cada frame recebido via EP 0x81 tem este formato:

```
┌──────────────────────────────────────────────────────┐
│           RX Descriptor (24 bytes)                   │
│   DWORD0: pktlen, FCS error, drvinfosize             │
│   DWORD1: macid, tid, agregação                      │
│   DWORD2: número de sequência                        │
│   DWORD3: MCS rate recebido, largura de banda        │
│   DWORD4: match flags                                │
│   DWORD5: timestamp (µs)                             │
├──────────────────────────────────────────────────────┤
│           PHY Status (drvinfosize × 8 bytes)         │
│   RSSI, noise floor, AGC, EVM                        │
├──────────────────────────────────────────────────────┤
│           Frame 802.11 (pktlen bytes)                │
└──────────────────────────────────────────────────────┘
```

## EFUSE

O EFUSE é uma memória de 512 bytes gravada na fábrica com:

| Offset | Conteúdo                           |
|--------|------------------------------------|
| 0x000  | Versão do board                    |
| 0x001  | Configuração de antena             |
| 0x107  | Endereço MAC (6 bytes)             |
| 0x10D  | Calibração de potência TX por canal|
| 0x1A0  | Parâmetros de ganho de RX          |

**Leitura do EFUSE:**
1. Habilitar: `REG_EFUSE_ACCESS = 0x69`
2. Para cada byte: escrever `(addr << 8) | BIT(30)` em `REG_EFUSE_CTRL`
3. Aguardar bit 30 zerar (hardware conclui leitura)
4. Ler byte dos bits [7:0] de `REG_EFUSE_CTRL`
5. Desabilitar: `REG_EFUSE_ACCESS = 0x00`

## Sequência de Power-On

```
1. APS_FSMCO: limpar bit APFM_OFF (sair de power-down)
2. SYS_FUNC_EN: habilitar BB, USB, PLL, WLON
3. Aguardar 1ms (PLLs estabilizam)
4. APS_FSMCO: setar MAC_FUNC_EN (dispara sequência do MAC)
5. Aguardar MAC_FUNC_EN zerar (hardware confirma conclusão)
6. Inicializar registradores do MAC (CR, PBP, RCR, EDCA)
7. Inicializar Baseband (tabela de registradores BB)
8. Configurar canal inicial
```

## Diferença FTV vs FU vs EU

| Variante | PID    | Mercado | Status drivers Linux |
|----------|--------|---------|----------------------|
| EU       | 0x8179 | Europa  | `rtl8xxxu` (bem suportado) |
| FU       | 0xf179 | China   | `rtl8188fu` (limitado) |
| FTV      | 0xf179 | China   | idem FU (mesmo PID!) |
| SU       | 0x818b | —       | `rtl8xxxu` |

O FTV e FU compartilham o mesmo PID `0xf179`. A diferença está na versão
do firmware e em pequenas variações de hardware interno. O driver `rtl8188fu`
do kernel cobre parcialmente este chip, mas com problemas de throughput
e desconexões esporádicas — motivação para este driver.

## Por que o driver existente é ineficiente?

Problema observado com `rtl8188fu` do kernel 6.1:

1. **Sem agregação A-MSDU/A-MPDU**: cada frame é transmitido individualmente,
   gerando overhead USB enorme (32 bytes de descriptor + overhead USB ≈ 50%
   do payload em frames pequenos).

2. **URBs de RX em série**: o driver oficial usa poucos URBs simultâneos,
   causando perda de frames em ambientes de alto tráfego.

3. **Rate control fraco**: sem feedback adequado de sinal, o algoritmo de
   rate control tende a ficar em taxas baixas mesmo com sinal bom.

4. **Sem SGI em HT20**: Short Guard Interval (aumentaria 10% da throughput)
   nunca ativado por bug na inicialização.
