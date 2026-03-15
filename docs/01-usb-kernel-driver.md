# 01 — Como funciona um driver USB no kernel Linux

## O que é um driver de kernel?

Um programa normal (userspace) roda no "Ring 3" — não tem acesso direto ao
hardware. Quando você chama `open("/dev/usb0")`, o kernel intercepta a chamada
e executa o código do driver em seu nome.

Um módulo do kernel, por outro lado, roda no "Ring 0" — tem acesso irrestrito
ao hardware. É isso que nos permite falar diretamente com o chip WiFi.

```
Userspace:   [NetworkManager] [wpa_supplicant] [iw]
                         │
             ────────────│──── barreira syscall ────
                         │
Kernelspace:        [mac80211] → [nosso driver] → [hardware USB]
```

## Estrutura de um driver USB

Todo driver USB no Linux precisa de três coisas:

### 1. Tabela de IDs (`usb_device_id[]`)

```c
static const struct usb_device_id minha_tabela[] = {
    { USB_DEVICE(0x0bda, 0xf179) },  // Realtek RTL8188FTV
    { }  // sentinela — fim da tabela
};
MODULE_DEVICE_TABLE(usb, minha_tabela);
```

O `MODULE_DEVICE_TABLE` exporta a tabela para o arquivo `modules.alias`.
Quando você conecta o USB, o udev lê esse arquivo e chama `modprobe` para
carregar o driver automaticamente.

### 2. `struct usb_driver`

```c
static struct usb_driver meu_driver = {
    .name       = "meu_driver",
    .probe      = meu_probe,       // chamado ao conectar
    .disconnect = meu_disconnect,  // chamado ao desconectar
    .id_table   = minha_tabela,
};
module_usb_driver(meu_driver);
```

### 3. Funções `probe` e `disconnect`

`probe()` é chamado pelo kernel quando o dispositivo aparece. `disconnect()` é
chamado quando o dispositivo é removido ou o módulo é descarregado.

## Tipos de transferência USB

O protocolo USB tem 4 tipos de transferência. Nosso chip usa dois:

| Tipo       | Endpoint | Uso no RTL8188FTV         |
|------------|----------|---------------------------|
| Control    | EP0      | Ler/escrever registradores|
| Bulk IN    | EP 0x81  | Receber frames WiFi (RX)  |
| Bulk OUT   | EP 0x02  | Transmitir frames (TX)    |
| Bulk OUT   | EP 0x03  | TX alta prioridade        |

## O que é um URB?

URB = *USB Request Block*. É a estrutura fundamental de I/O do subsistema USB.

**Fluxo assíncrono:**

```
1. Alocamos o URB:       urb = usb_alloc_urb(0, GFP_KERNEL);
2. Preenchemos:          usb_fill_bulk_urb(urb, dev, pipe, buf, len, cb, ctx);
3. Submetemos:           usb_submit_urb(urb, GFP_ATOMIC);
   ↓ (retorna imediatamente)
4. Hardware executa a transferência...
5. Callback é chamada:   cb(urb)  ← temos os dados!
6. Resubmetemos o URB para continuar recebendo
```

**Por que múltiplos URBs de RX?**

Se temos só 1 URB e o hardware envia dois frames em sequência rápida:
- Frame 1: URB captura, callback processa (demora alguns µs)
- Frame 2: **perdido!** — não tinha URB disponível

Com 8 URBs simultâneos, sempre há um disponível para capturar o próximo frame.

## Contexto de execução — muito importante!

No kernel, existem dois contextos de execução:

| Contexto  | Pode dormir? | Exemplos                        |
|-----------|--------------|---------------------------------|
| Processo  | Sim          | `insmod`, `probe()`, workqueue  |
| Interrupção | Não        | URB callback, softirq, spinlock |

Regra de ouro:
- `probe()`: usa `GFP_KERNEL`, pode chamar `msleep()`
- Callback de URB: usa `GFP_ATOMIC`, NUNCA pode dormir

## Como registradores são acessados

O RTL8188FTV usa USB Control Transfers (EP0) para ler/escrever registradores:

```
LEITURA de 4 bytes do registrador 0x0608 (RCR):

  Setup Packet:
    bmRequestType = 0xC0  (Vendor, Device-to-Host)
    bRequest      = 0x05  (comando Realtek)
    wValue        = 0x0608 (endereço)
    wIndex        = 0x0000
    wLength       = 0x0004 (4 bytes)

  Data Phase (device → host):
    [val_byte0][val_byte1][val_byte2][val_byte3]
```

No código:
```c
usb_control_msg(udev,
    usb_rcvctrlpipe(udev, 0),
    0x05,    // bRequest
    0xC0,    // bmRequestType
    addr,    // wValue = endereço
    0,       // wIndex
    buf,     // buffer destino
    4,       // wLength
    500);    // timeout ms
```

## Ciclo de vida completo do driver

```
insmod rtl8188ftv.ko
    └─→ module_init() → usb_register(&driver)
         └─→ kernel monitora USB hotplug

USB connect (ou já estava conectado)
    └─→ usb_probe()
         ├─→ ieee80211_alloc_hw()   — aloca estruturas mac80211
         ├─→ hal_init()             — power-on do chip
         ├─→ mac_register()         — cria wlan0 no sistema
         └─→ return 0               — sucesso

ip link set wlan0 up
    └─→ mac80211 chama .start()
         └─→ usb_start_rx()         — submete URBs de RX

Frame WiFi chegando
    └─→ rtl8188ftv_rx_complete()   — callback do URB
         ├─→ parse RX descriptor
         ├─→ ieee80211_rx_irqsafe() — entrega ao mac80211
         └─→ resubmete URB

iw scan
    └─→ mac80211 chama .hw_scan()   — não implementado ainda
         └─→ (TODO)

rmmod rtl8188ftv
    └─→ module_exit() → usb_deregister()
         └─→ usb_disconnect()
              ├─→ usb_stop_rx()     — cancela URBs
              ├─→ mac_unregister()  — remove wlan0
              ├─→ hal_deinit()      — desliga chip
              └─→ ieee80211_free_hw()
```
