// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb.c — Camada de comunicação USB do driver RTL8188FTV
 *
 * Este arquivo implementa toda a comunicação entre o driver e o hardware
 * via protocolo USB. É a "cola" entre o hardware físico e as camadas
 * superiores (HAL e MAC).
 *
 * Conceitos USB fundamentais implementados aqui:
 * ===============================================
 *
 * 1. TRANSFERS USB
 *    USB define 4 tipos de transferência:
 *    - Control:   EP0, bidirecional, para configuração. Usado para ler/
 *                 escrever registradores do chip.
 *    - Bulk:      Alta velocidade, sem garantia de timing. Usado para dados
 *                 (TX/RX de frames WiFi).
 *    - Interrupt: Periódico, baixa latência. Não usado por este chip.
 *    - Isochronous: Para áudio/vídeo em tempo real. Não usado aqui.
 *
 * 2. URB (USB Request Block)
 *    É a estrutura fundamental de I/O USB no Linux. Toda transferência é
 *    feita submetendo um URB ao USB core, que a entrega ao controlador
 *    host (xHCI/EHCI). Quando completa, uma callback é chamada.
 *
 *    Fluxo de RX (recepção):
 *    ┌──────────┐   usb_submit_urb()   ┌──────────┐   hardware   ┌─────────┐
 *    │  driver  │──────────────────────▶ USB core │──────────────▶  chip   │
 *    │  (nós)   │◀──────────────────── │          │◀─────────────│  WiFi   │
 *    └──────────┘  callback (dados)    └──────────┘              └─────────┘
 *
 * 3. CONTROL TRANSFER (para registradores)
 *    O protocolo Realtek usa control transfers para acessar registradores:
 *
 *    LEITURA:
 *      bmRequestType = 0xC0  (Device-to-Host | Vendor | Device)
 *      bRequest      = 0x05  (comando proprietário Realtek)
 *      wValue        = endereço do registrador
 *      wIndex        = 0
 *      wLength       = tamanho em bytes (1, 2 ou 4)
 *
 *    ESCRITA:
 *      bmRequestType = 0x40  (Host-to-Device | Vendor | Device)
 *      bRequest      = 0x05
 *      wValue        = endereço do registrador
 *      wIndex        = 0
 *      data          = valor a escrever
 */

#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/skbuff.h>

#include "rtl8188ftv.h"
#include "rtl8188ftv_reg.h"

/* =========================================================================
 * Constantes do protocolo USB Realtek
 * ========================================================================= */

/*
 * Vendor Request Code para acesso a registradores.
 * Este é um valor proprietário Realtek, descoberto por engenharia reversa
 * e confirmado no código-fonte do driver rtl8xxxu do kernel.
 */
#define RTL_VENDOR_REQUEST_REG_READ     0x05
#define RTL_VENDOR_REQUEST_REG_WRITE    0x05

/* Timeout para control transfers (em milissegundos) */
#define USB_CTRL_TIMEOUT_MS             500

/* Tamanho máximo de um bulk transfer RX */
#define RX_BUF_SIZE                     (MAX_MSDU_SIZE + RTL8188FTV_RX_DESC_SIZE + 256)

/* =========================================================================
 * Acesso a Registradores via Control Transfer
 * ========================================================================= */

/**
 * rtl8188ftv_usb_read_reg - Lê N bytes de um registrador do chip
 *
 * @priv: contexto do driver
 * @addr: endereço do registrador (16 bits)
 * @buf:  buffer de destino
 * @len:  número de bytes a ler (1, 2 ou 4)
 *
 * Envia uma USB Control Transfer do tipo "Vendor IN" ao EP0.
 *
 * O campo wValue do setup packet contém o endereço do registrador.
 * O campo wLength indica quantos bytes queremos ler.
 * O chip responde com os bytes do registrador na fase DATA IN.
 *
 * Retorna 0 em caso de sucesso, código de erro negativo em falha.
 */
int rtl8188ftv_usb_read_reg(struct rtl8188ftv_priv *priv,
                            u16 addr, void *buf, u16 len)
{
    int ret;

    /*
     * usb_control_msg() é a API síncrona para control transfers.
     * Ela BLOQUEIA até a transferência completar ou dar timeout.
     * NÃO deve ser chamada em contexto de interrupção (atomic context).
     *
     * Parâmetros do USB Setup Packet:
     *   pipe            = usb_rcvctrlpipe() → EP0, direção IN
     *   bRequestType    = 0xC0 = 1(D-to-H) | 10(Vendor) | 00(Device)
     *   bRequest        = 0x05 (comando Realtek)
     *   wValue          = endereço do registrador
     *   wIndex          = 0
     *   data            = buffer para os dados recebidos
     *   wLength         = número de bytes
     *   timeout         = em jiffies (USB_CTRL_TIMEOUT_MS ms)
     */
    ret = usb_control_msg(priv->udev,
                          usb_rcvctrlpipe(priv->udev, 0),
                          RTL_VENDOR_REQUEST_REG_READ,
                          0xC0,               /* bmRequestType: Vendor IN */
                          addr,               /* wValue = endereço */
                          0,                  /* wIndex */
                          buf,                /* buffer de dados */
                          len,                /* wLength */
                          USB_CTRL_TIMEOUT_MS);

    if (ret < 0) {
        dev_err(&priv->udev->dev,
                "read_reg addr=0x%04x len=%d failed: %d\n",
                addr, len, ret);
    }

    return ret < 0 ? ret : 0;
}

/**
 * rtl8188ftv_usb_write_reg - Escreve N bytes em um registrador do chip
 *
 * @priv: contexto do driver
 * @addr: endereço do registrador
 * @buf:  dados a escrever
 * @len:  número de bytes (1, 2 ou 4)
 *
 * Envia uma USB Control Transfer do tipo "Vendor OUT" ao EP0.
 */
int rtl8188ftv_usb_write_reg(struct rtl8188ftv_priv *priv,
                             u16 addr, void *buf, u16 len)
{
    int ret;

    ret = usb_control_msg(priv->udev,
                          usb_sndctrlpipe(priv->udev, 0),
                          RTL_VENDOR_REQUEST_REG_WRITE,
                          0x40,               /* bmRequestType: Vendor OUT */
                          addr,               /* wValue = endereço */
                          0,                  /* wIndex */
                          buf,                /* dados */
                          len,                /* wLength */
                          USB_CTRL_TIMEOUT_MS);

    if (ret < 0) {
        dev_err(&priv->udev->dev,
                "write_reg addr=0x%04x len=%d failed: %d\n",
                addr, len, ret);
    }

    return ret < 0 ? ret : 0;
}

/* =========================================================================
 * Transmissão de Dados (TX via Bulk OUT)
 * ========================================================================= */

/*
 * Estrutura auxiliar passada como contexto para a callback de TX.
 * Precisamos dela porque a callback é chamada de forma assíncrona
 * (possivelmente em contexto de interrupção), então não podemos
 * inferir o contexto só com o URB.
 */
struct rtl8188ftv_tx_ctx {
    struct rtl8188ftv_priv *priv;
    struct sk_buff          *skb;   /* O frame que foi transmitido */
};

/**
 * rtl8188ftv_tx_complete - Callback chamada quando TX termina (assíncrono)
 *
 * Esta função é chamada pelo USB core quando a transferência bulk OUT
 * completa. Pode ser chamada em contexto de interrupção (softirq).
 *
 * @urb: o URB que completou
 */
static void rtl8188ftv_tx_complete(struct urb *urb)
{
    struct rtl8188ftv_tx_ctx *ctx = urb->context;
    struct rtl8188ftv_priv   *priv = ctx->priv;

    /* Verifica se houve erro na transmissão */
    if (urb->status) {
        if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
            urb->status != -ESHUTDOWN) {
            dev_warn(&priv->udev->dev,
                     "TX URB error: %d\n", urb->status);
        }
    }

    /* Libera o sk_buff — o frame já foi entregue ao hardware */
    dev_kfree_skb_irq(ctx->skb);

    /* Decrementa contador de URBs ativos */
    atomic_dec(&priv->tx_urbs_active);

    /* Libera o contexto e o URB */
    kfree(ctx);
    usb_free_urb(urb);

    /*
     * Notifica o mac80211 que pode voltar a encaminhar frames
     * se havia parado devido a fila cheia (stop_queues).
     * ieee80211_wake_queues() é seguro chamar de contexto de interrupção.
     */
    if (atomic_read(&priv->tx_urbs_active) < TX_URB_COUNT / 2)
        ieee80211_wake_queues(priv->hw);
}

/**
 * rtl8188ftv_usb_tx - Transmite um sk_buff via USB Bulk OUT
 *
 * @priv: contexto do driver
 * @skb:  frame a transmitir (inclui TX descriptor prepended)
 * @ep:   endpoint de destino (RTL8188FTV_EP_TX_DATA ou _TX_HIGH)
 *
 * Fluxo:
 *   1. Aloca um URB
 *   2. Preenche com os dados do skb
 *   3. Submete ao USB core de forma assíncrona
 *   4. Retorna imediatamente; rtl8188ftv_tx_complete() é chamado depois
 */
int rtl8188ftv_usb_tx(struct rtl8188ftv_priv *priv, struct sk_buff *skb,
                      unsigned int ep)
{
    struct urb                *urb;
    struct rtl8188ftv_tx_ctx  *ctx;
    unsigned int               pipe;
    int                        ret;

    /* Limita número de URBs simultâneos para controle de fluxo */
    if (atomic_read(&priv->tx_urbs_active) >= TX_URB_COUNT) {
        ieee80211_stop_queues(priv->hw);
        return -ENOSPC;
    }

    /*
     * Aloca um URB. O parâmetro 0 indica "sem pacotes iso".
     * usb_alloc_urb() usa GFP_ATOMIC se em contexto de interrupção,
     * ou GFP_KERNEL caso contrário (aqui usamos GFP_ATOMIC por segurança).
     */
    urb = usb_alloc_urb(0, GFP_ATOMIC);
    if (!urb)
        return -ENOMEM;

    ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx) {
        usb_free_urb(urb);
        return -ENOMEM;
    }

    ctx->priv = priv;
    ctx->skb  = skb;

    /*
     * usb_sndbulkpipe(udev, ep_num) constrói um "pipe" — um identificador
     * opaco que combina: dispositivo + endpoint + direção.
     * Para TX, usamos "snd" (send = OUT).
     */
    pipe = usb_sndbulkpipe(priv->udev, ep);

    /*
     * usb_fill_bulk_urb() preenche o URB com todos os parâmetros:
     *   urb          = o URB a preencher
     *   dev          = dispositivo USB
     *   pipe         = endpoint de destino
     *   transfer_buf = ponteiro para os dados (cabeçalho do skb)
     *   len          = tamanho total
     *   complete_fn  = callback quando terminar
     *   context      = dado passado para a callback
     */
    usb_fill_bulk_urb(urb, priv->udev, pipe,
                      skb->data, skb->len,
                      rtl8188ftv_tx_complete, ctx);

    /* Marca o URB para liberar automaticamente o transfer_buf? Não aqui —
     * o skb já gerencia seu buffer. */
    urb->transfer_flags |= URB_ZERO_PACKET;

    atomic_inc(&priv->tx_urbs_active);

    /*
     * usb_submit_urb() entrega o URB ao controlador USB.
     * Retorna imediatamente (assíncrono).
     * A callback será chamada quando a transferência completar.
     */
    ret = usb_submit_urb(urb, GFP_ATOMIC);
    if (ret) {
        dev_err(&priv->udev->dev, "TX submit failed: %d\n", ret);
        atomic_dec(&priv->tx_urbs_active);
        kfree(ctx);
        usb_free_urb(urb);
        return ret;
    }

    return 0;
}

/* =========================================================================
 * Recepção de Dados (RX via Bulk IN)
 * ========================================================================= */

/**
 * rtl8188ftv_rx_complete - Callback de RX chamada quando dados chegam
 *
 * Esta é a função mais crítica do driver — é chamada toda vez que o chip
 * envia dados ao host (frame WiFi recebido).
 *
 * Fluxo por frame recebido:
 *   1. Verificar erro do URB
 *   2. Parsear o RX descriptor (primeiros 24 bytes)
 *   3. Extrair o frame 802.11
 *   4. Entregar ao mac80211 via ieee80211_rx_irqsafe()
 *   5. Resubmeter o URB para continuar recebendo
 */
static void rtl8188ftv_rx_complete(struct urb *urb)
{
    struct rtl8188ftv_rx_urb  *rx_urb = urb->context;
    struct rtl8188ftv_priv    *priv   = rx_urb->priv;
    struct rtl8188ftv_rxdesc  *rxdesc;
    u32 pktlen, drvinfo_size;
    u8 *payload;
    int ret;

    /* URB cancelado (rmmod, unplug) — não resubmeter */
    if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
        urb->status == -ESHUTDOWN) {
        return;
    }

    /* Erro de I/O — resubmeter mesmo assim para não parar RX */
    if (urb->status) {
        dev_warn(&priv->udev->dev,
                 "RX URB error: %d, resubmitting\n", urb->status);
        goto resubmit;
    }

    /* Verifica tamanho mínimo: pelo menos um RX descriptor */
    if (urb->actual_length < RTL8188FTV_RX_DESC_SIZE) {
        dev_warn(&priv->udev->dev, "RX: short packet (%d bytes)\n",
                 urb->actual_length);
        goto resubmit;
    }

    /*
     * O buffer recebido tem o formato:
     *
     *   [ RX Descriptor (24 bytes) ][ PHY status (variável) ][ 802.11 frame ]
     *
     * RX Descriptor está no início do buffer.
     */
    rxdesc = (struct rtl8188ftv_rxdesc *)rx_urb->buf;

    /*
     * Extrai o tamanho do payload do DWORD 0 do descritor.
     * Bits [13:0] = pktlen (tamanho do frame 802.11)
     * Bits [19:16] = drvinfosize (tamanho extra em unidades de 8 bytes)
     */
    pktlen       = le32_to_cpu(rxdesc->rxdw0) & 0x3FFF;
    drvinfo_size = ((le32_to_cpu(rxdesc->rxdw0) >> 16) & 0x0F) * 8;

    /* Ponteiro para o início do frame 802.11 */
    payload = rx_urb->buf + RTL8188FTV_RX_DESC_SIZE + drvinfo_size;

    /* Valida tamanhos */
    if (pktlen == 0 ||
        RTL8188FTV_RX_DESC_SIZE + drvinfo_size + pktlen > urb->actual_length) {
        dev_warn(&priv->udev->dev,
                 "RX: invalid pktlen=%u, actual=%u\n",
                 pktlen, urb->actual_length);
        goto resubmit;
    }

    /*
     * Entrega o frame ao mac80211.
     * rtl8188ftv_mac_rx_frame() é implementado em mac.c e faz o trabalho
     * de criar um sk_buff e chamar ieee80211_rx_irqsafe().
     */
    rtl8188ftv_mac_rx_frame(priv, payload, pktlen);

resubmit:
    /* Resubmete o URB para continuar recebendo.
     * Sem isso, paramos de receber dados! */
    if (priv->rx_running) {
        ret = usb_submit_urb(urb, GFP_ATOMIC);
        if (ret && ret != -ENODEV)
            dev_err(&priv->udev->dev,
                    "RX resubmit failed: %d\n", ret);
    }
}

/**
 * rtl8188ftv_usb_start_rx - Inicia o loop de recepção USB
 *
 * Aloca e submete RX_URB_COUNT URBs simultâneos ao controlador USB.
 * Ter múltiplos URBs "voando" ao mesmo tempo minimiza latência:
 * enquanto um está sendo processado na callback, os outros já estão
 * aguardando dados do hardware.
 */
int rtl8188ftv_usb_start_rx(struct rtl8188ftv_priv *priv)
{
    int i, ret;

    priv->rx_running = true;

    for (i = 0; i < RX_URB_COUNT; i++) {
        struct rtl8188ftv_rx_urb *rx_urb = &priv->rx_urbs[i];

        rx_urb->priv = priv;

        /* Aloca URB */
        rx_urb->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!rx_urb->urb) {
            ret = -ENOMEM;
            goto err_stop;
        }

        /*
         * Aloca buffer DMA-coerente para recepção.
         * usb_alloc_coherent() garante que o buffer é acessível tanto
         * pelo CPU quanto pelo controlador DMA do USB sem cache issues.
         * É mais caro que kmalloc, mas necessário para DMA.
         */
        rx_urb->buf = usb_alloc_coherent(priv->udev,
                                         RX_BUF_SIZE,
                                         GFP_KERNEL,
                                         &rx_urb->buf_dma);
        if (!rx_urb->buf) {
            usb_free_urb(rx_urb->urb);
            rx_urb->urb = NULL;
            ret = -ENOMEM;
            goto err_stop;
        }

        /*
         * Preenche o URB para Bulk IN (recepção).
         * usb_rcvbulkpipe = endpoint de recepção bulk.
         */
        usb_fill_bulk_urb(rx_urb->urb, priv->udev,
                          usb_rcvbulkpipe(priv->udev, priv->ep_in),
                          rx_urb->buf, RX_BUF_SIZE,
                          rtl8188ftv_rx_complete, rx_urb);

        /* Marca o buffer como DMA (melhor performance) */
        rx_urb->urb->transfer_dma  = rx_urb->buf_dma;
        rx_urb->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

        /* Submete o URB — começa a "escutar" o hardware */
        ret = usb_submit_urb(rx_urb->urb, GFP_KERNEL);
        if (ret) {
            dev_err(&priv->udev->dev,
                    "RX URB[%d] submit failed: %d\n", i, ret);
            goto err_stop;
        }
    }

    dev_info(&priv->udev->dev,
             "RX iniciado: %d URBs submetidos\n", RX_URB_COUNT);
    return 0;

err_stop:
    rtl8188ftv_usb_stop_rx(priv);
    return ret;
}

/**
 * rtl8188ftv_usb_stop_rx - Para o loop de recepção e libera URBs
 */
void rtl8188ftv_usb_stop_rx(struct rtl8188ftv_priv *priv)
{
    int i;

    priv->rx_running = false;

    for (i = 0; i < RX_URB_COUNT; i++) {
        struct rtl8188ftv_rx_urb *rx_urb = &priv->rx_urbs[i];

        if (!rx_urb->urb)
            continue;

        /* usb_kill_urb() cancela o URB e AGUARDA a callback completar.
         * Diferente de usb_unlink_urb() que é assíncrono. */
        usb_kill_urb(rx_urb->urb);

        if (rx_urb->buf) {
            usb_free_coherent(priv->udev, RX_BUF_SIZE,
                              rx_urb->buf, rx_urb->buf_dma);
            rx_urb->buf = NULL;
        }

        usb_free_urb(rx_urb->urb);
        rx_urb->urb = NULL;
    }
}

/* =========================================================================
 * Probe e Disconnect
 * ========================================================================= */

/**
 * rtl8188ftv_usb_probe - Inicializa o driver quando o dispositivo é detectado
 *
 * Esta função é chamada pelo USB core quando um dispositivo com VID/PID
 * compatível é conectado (ou quando o módulo é carregado com o dispositivo
 * já conectado).
 *
 * Responsabilidades:
 *   1. Alocar a estrutura ieee80211_hw (com nosso priv embutido)
 *   2. Configurar os campos do hw para o mac80211
 *   3. Descobrir os endpoints USB
 *   4. Salvar referência ao dispositivo USB
 *   5. Inicializar o hardware (hal_init)
 *   6. Registrar com o mac80211 (mac_register)
 */
int rtl8188ftv_usb_probe(struct usb_interface *intf,
                         const struct usb_device_id *id)
{
    struct usb_device              *udev = interface_to_usbdev(intf);
    struct usb_host_interface      *iface_desc;
    struct usb_endpoint_descriptor *ep_desc;
    struct ieee80211_hw            *hw;
    struct rtl8188ftv_priv         *priv;
    int i, ret;

    dev_info(&udev->dev,
             "RTL8188FTV detectado: %s %s (serial: %s)\n",
             udev->manufacturer ?: "?",
             udev->product ?: "?",
             udev->serial ?: "?");

    /*
     * ieee80211_alloc_hw() aloca a estrutura ieee80211_hw mais nossa
     * estrutura privada (priv) em um bloco de memória contíguo.
     *
     * Parâmetros:
     *   sizeof(struct rtl8188ftv_priv) = tamanho do nosso priv
     *   &rtl8188ftv_mac_ops           = nossa implementação das ops mac80211
     *                                   (definida em mac.c)
     *
     * Retorna um hw com hw->priv apontando para nossa estrutura privada.
     */
    hw = ieee80211_alloc_hw(sizeof(struct rtl8188ftv_priv),
                            rtl8188ftv_get_mac_ops());
    if (!hw) {
        dev_err(&udev->dev, "Falha ao alocar ieee80211_hw\n");
        return -ENOMEM;
    }

    priv = hw->priv;
    priv->hw   = hw;
    priv->udev = udev;
    priv->intf = intf;

    mutex_init(&priv->mutex);
    spin_lock_init(&priv->tx_lock);
    skb_queue_head_init(&priv->tx_queue);
    atomic_set(&priv->tx_urbs_active, 0);

    /*
     * Cria uma workqueue dedicada para este driver.
     * Workqueues permitem executar código em contexto de processo
     * (pode dormir) agendado a partir de contextos de interrupção.
     */
    priv->wq = create_singlethread_workqueue("rtl8188ftv");
    if (!priv->wq) {
        ret = -ENOMEM;
        goto err_free_hw;
    }

    /*
     * Descobre os endpoints percorrendo os descriptores da interface.
     * O driver valida que os endpoints esperados (EP1 IN, EP2 OUT, EP3 OUT)
     * estão presentes antes de continuar.
     */
    iface_desc = intf->cur_altsetting;
    priv->ep_in      = 0;
    priv->ep_tx_data = 0;
    priv->ep_tx_high = 0;

    for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
        ep_desc = &iface_desc->endpoint[i].desc;

        /* usb_endpoint_is_bulk_in() verifica bmAttributes e bEndpointAddress */
        if (usb_endpoint_is_bulk_in(ep_desc) && !priv->ep_in) {
            priv->ep_in = usb_endpoint_num(ep_desc);
            dev_dbg(&udev->dev, "EP IN encontrado: 0x%02x\n",
                    ep_desc->bEndpointAddress);
        }
        else if (usb_endpoint_is_bulk_out(ep_desc) && !priv->ep_tx_data) {
            priv->ep_tx_data = usb_endpoint_num(ep_desc);
            dev_dbg(&udev->dev, "EP TX DATA encontrado: 0x%02x\n",
                    ep_desc->bEndpointAddress);
        }
        else if (usb_endpoint_is_bulk_out(ep_desc) && !priv->ep_tx_high) {
            priv->ep_tx_high = usb_endpoint_num(ep_desc);
            dev_dbg(&udev->dev, "EP TX HIGH encontrado: 0x%02x\n",
                    ep_desc->bEndpointAddress);
        }
    }

    if (!priv->ep_in || !priv->ep_tx_data) {
        dev_err(&udev->dev, "Endpoints obrigatórios não encontrados\n");
        ret = -ENODEV;
        goto err_destroy_wq;
    }

    /*
     * Configura os campos do ieee80211_hw para informar ao mac80211
     * as capacidades do nosso hardware.
     */

    /* Informa ao mac80211 em quais bandas de frequência operamos */
    ieee80211_hw_set(hw, SIGNAL_DBM);           /* RSSI em dBm */
    ieee80211_hw_set(hw, RX_INCLUDES_FCS);      /* Frame inclui FCS */
    ieee80211_hw_set(hw, SUPPORTS_PS);          /* Power save */

    /*
     * SET_IEEE80211_DEV vincula o ieee80211_hw ao dispositivo USB.
     * Isso cria o link no sysfs e permite que ferramentas como
     * iw e NetworkManager encontrem o dispositivo.
     */
    SET_IEEE80211_DEV(hw, &intf->dev);

    /*
     * usb_set_intfdata() salva nosso ponteiro priv na interface USB.
     * Permite recuperá-lo em disconnect() via usb_get_intfdata().
     */
    usb_set_intfdata(intf, priv);

    /* Inicializa o hardware (sequência de power-on do chip) */
    ret = rtl8188ftv_hal_init(priv);
    if (ret) {
        dev_err(&udev->dev, "HAL init falhou: %d\n", ret);
        goto err_destroy_wq;
    }

    /* Registra com o mac80211 — a partir daqui o dispositivo aparece no sistema */
    ret = rtl8188ftv_mac_register(priv);
    if (ret) {
        dev_err(&udev->dev, "MAC register falhou: %d\n", ret);
        goto err_hal_deinit;
    }

    dev_info(&udev->dev,
             "RTL8188FTV pronto. MAC: %pM\n", priv->mac_addr);
    return 0;

err_hal_deinit:
    rtl8188ftv_hal_deinit(priv);
err_destroy_wq:
    destroy_workqueue(priv->wq);
err_free_hw:
    ieee80211_free_hw(hw);
    return ret;
}

/**
 * rtl8188ftv_usb_disconnect - Limpa tudo quando o dispositivo é removido
 *
 * Ordem inversa ao probe: desregistra, para hardware, libera recursos.
 */
void rtl8188ftv_usb_disconnect(struct usb_interface *intf)
{
    struct rtl8188ftv_priv *priv = usb_get_intfdata(intf);

    if (!priv)
        return;

    dev_info(&priv->udev->dev, "RTL8188FTV desconectando...\n");

    /* Para recepção */
    rtl8188ftv_usb_stop_rx(priv);

    /* Desregistra do mac80211 */
    rtl8188ftv_mac_unregister(priv);

    /* Desinicializa hardware */
    rtl8188ftv_hal_deinit(priv);

    /* Para e destrói workqueue (espera trabalhos pendentes terminarem) */
    flush_workqueue(priv->wq);
    destroy_workqueue(priv->wq);

    /* Limpa a fila TX */
    skb_queue_purge(&priv->tx_queue);

    usb_set_intfdata(intf, NULL);

    /* Libera o ieee80211_hw (e nosso priv embutido) */
    ieee80211_free_hw(priv->hw);

    dev_info(&priv->udev->dev, "RTL8188FTV desconectado.\n");
}
