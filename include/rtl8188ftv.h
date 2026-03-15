/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtl8188ftv.h — Estruturas e definições principais do driver RTL8188FTV
 *
 * O RTL8188FTV é um chip WiFi 802.11b/g/n da Realtek com interface USB 2.0.
 * "FTV" indica a variante para o mercado chinês (diferente do FU/EU/SU).
 *
 * Identificação USB:
 *   Vendor ID  : 0x0bda  (Realtek Semiconductor)
 *   Product ID : 0xf179
 *
 * Endpoints USB do dispositivo (descobertos via lsusb -v):
 *   EP 0x00 (EP0) — Control  IN/OUT — configuração e acesso a registradores
 *   EP 0x81 (EP1) — Bulk IN          — recepção de quadros 802.11 (RX)
 *   EP 0x02 (EP2) — Bulk OUT         — transmissão de quadros 802.11 (TX)
 *   EP 0x03 (EP3) — Bulk OUT         — transmissão de alta prioridade / comandos
 *
 * Arquitetura do driver:
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │                    mac80211 (kernel)                    │
 *   │   ieee80211_hw  →  ieee80211_ops  →  nosso driver      │
 *   └────────────────────────┬────────────────────────────────┘
 *                            │ callbacks (tx, start, stop, ...)
 *   ┌────────────────────────▼────────────────────────────────┐
 *   │                  MAC Layer  (mac.c)                     │
 *   │   monta/desmonta quadros 802.11, gerencia BSS, scan     │
 *   └────────────────────────┬────────────────────────────────┘
 *                            │
 *   ┌────────────────────────▼────────────────────────────────┐
 *   │             Hardware Abstraction (hal.c)                │
 *   │   inicializa chip, configura RF/BB, lê/escreve regs     │
 *   └────────────────────────┬────────────────────────────────┘
 *                            │ chamadas de I/O
 *   ┌────────────────────────▼────────────────────────────────┐
 *   │                  USB Layer  (usb.c)                     │
 *   │   URBs bulk/control, probe/disconnect, filas TX/RX      │
 *   └────────────────────────┬────────────────────────────────┘
 *                            │ hardware USB
 *                     [ RTL8188FTV ]
 */

#ifndef _RTL8188FTV_H_
#define _RTL8188FTV_H_

#include <linux/usb.h>
#include <linux/ieee80211.h>
#include <net/mac80211.h>

/* =========================================================================
 * Constantes do dispositivo
 * ========================================================================= */

#define RTL8188FTV_VENDOR_ID    0x0bda
#define RTL8188FTV_PRODUCT_ID   0xf179

/* Números dos endpoints (conforme descriptores USB) */
#define RTL8188FTV_EP_IN        0x81   /* Bulk IN  — dados recebidos (RX) */
#define RTL8188FTV_EP_TX_DATA   0x02   /* Bulk OUT — dados transmitidos (TX normal) */
#define RTL8188FTV_EP_TX_HIGH   0x03   /* Bulk OUT — TX alta prioridade / mgmt */

/* Tamanho máximo de pacote USB 2.0 High-Speed (512 bytes) */
#define USB_MAX_PACKET_SIZE     512

/* Tamanho máximo de um frame 802.11 (MSDU) */
#define MAX_MSDU_SIZE           2346

/* Número de URBs de RX que mantemos "voando" simultaneamente.
 * Mais URBs = menor latência de recepção, mas mais memória consumida. */
#define RX_URB_COUNT            8

/* Número de URBs de TX na fila */
#define TX_URB_COUNT            16

/* =========================================================================
 * Descritor de TX (TX Descriptor — TXD)
 *
 * O chip exige que cada frame transmitido seja precedido por um bloco de
 * 32 bytes descrevendo a transmissão. O hardware lê este bloco via DMA
 * e configura os parâmetros do PHY/MAC automaticamente.
 *
 * Layout baseado no datasheet RTL8188EU (variante mais documentada, FTV
 * é compatível nos campos essenciais):
 *
 *  Byte 0-3:  TXDW0 — tamanho do pacote, offset, tipo
 *  Byte 4-7:  TXDW1 — macid, queue select, rate bitmap
 *  Byte 8-11: TXDW2 — sequência, agregação
 *  ...
 * ========================================================================= */
#define RTL8188FTV_TX_DESC_SIZE  32

struct rtl8188ftv_txdesc {
    /* DWORD 0: pktlen[15:0], offset[23:16], bmc[24], htc[25],
     *          ls[26], fs[27], linip[28], noacm[29], gf[30], own[31] */
    __le32 txdw0;

    /* DWORD 1: macid[4:0], pkttype[7:5], multicast[13], bip[14],
     *          morefrag[17], swenc[22], pkttxtime[31:24] */
    __le32 txdw1;

    /* DWORD 2: bw[17:16], lstp[18], data_shortgi[20], ccx_en[22],
     *          ampdu_density[25:23], ampdu_en[31] */
    __le32 txdw2;

    /* DWORD 3: seq[27:16], pkt_offset[28] */
    __le32 txdw3;

    /* DWORD 4: rts_rate[4:0], qos[6], hwseq[7], userrate[8],
     *          dis_rtsfb[10], dis_datafb[11], cts2self[11],
     *          rts_en[12], hwrts[13], data_bw[14], txrate[20:16],
     *          data_stbc[23:22], data_sgi[25], txrate_fb_lmt[29:26] */
    __le32 txdw4;

    /* DWORD 5: data_shortpreamble[4], txpwr_offset[8:6], retry_lmt[16:11] */
    __le32 txdw5;

    /* DWORD 6: timestamp[31:0] */
    __le32 txdw6;

    /* DWORD 7: txbuf_size[15:0], empkt_num[3:0], pack_small_th[7:4] */
    __le32 txdw7;
} __packed;

/* =========================================================================
 * Descritor de RX (RX Descriptor — RXD)
 *
 * Cada frame recebido pelo chip é prefixado com 24 bytes de metadados.
 * O driver lê estes campos para saber: tamanho do payload, sinal RSSI,
 * taxa de recepção, se houve erro de FCS, etc.
 * ========================================================================= */
#define RTL8188FTV_RX_DESC_SIZE  24

struct rtl8188ftv_rxdesc {
    /* DWORD 0: pktlen[13:0], crc32[14], icverr[15], drvinfosize[19:16],
     *          security[22:20], qos[23], shift[25:24], physt[26],
     *          swdec[27], ls[28], fs[29], eor[30], own[31] */
    __le32 rxdw0;

    /* DWORD 1: macid[4:0], tid[8:5], hwrsvd[15:13], paggr[16], faggr[17],
     *          a1fit[21:18], a2fit[25:22], pam[26], pwr[27], moredata[28],
     *          morefrag[29], type[31:30] */
    __le32 rxdw1;

    /* DWORD 2: seq[27:16] */
    __le32 rxdw2;

    /* DWORD 3: rx_mcs[6:0], rx_ht[7], splcp[8], bw[9], htc[10] */
    __le32 rxdw3;

    /* DWORD 4: pattern_match[0], unicast_match[1], magic_match[2] */
    __le32 rxdw4;

    /* DWORD 5: tsfl[31:0] — timestamp em microsegundos */
    __le32 rxdw5;
} __packed;

/* =========================================================================
 * Estrutura de um URB de RX pendente
 *
 * Um URB (USB Request Block) é a unidade fundamental de I/O no subsistema
 * USB do Linux. Para receber dados, submetemos múltiplos URBs ao controlador
 * USB simultaneamente — assim que um completa, sua callback processa os dados
 * e o URB é resubmetido.
 * ========================================================================= */
struct rtl8188ftv_rx_urb {
    struct urb      *urb;       /* O URB em si (alocado via usb_alloc_urb) */
    u8              *buf;       /* Buffer de dados (DMA-safe) */
    dma_addr_t       buf_dma;  /* Endereço DMA do buffer */
    struct rtl8188ftv_priv *priv; /* Ponteiro de volta ao contexto do driver */
};

/* =========================================================================
 * Estrutura principal do driver (priv — private data)
 *
 * O mac80211 aloca esta estrutura via ieee80211_alloc_hw(sizeof(*priv)).
 * É o "objeto" central que carrega todo o estado do driver.
 * ========================================================================= */
struct rtl8188ftv_priv {
    /* --- mac80211 ------------------------------------------------------- */
    struct ieee80211_hw     *hw;        /* Handle do mac80211 */
    struct ieee80211_vif    *vif;       /* Interface virtual ativa */

    /* --- USB ------------------------------------------------------------ */
    struct usb_device       *udev;      /* Dispositivo USB */
    struct usb_interface    *intf;      /* Interface USB ativa */

    /* Endpoints descobertos no probe */
    unsigned int ep_in;
    unsigned int ep_tx_data;
    unsigned int ep_tx_high;

    /* --- RX ------------------------------------------------------------- */
    struct rtl8188ftv_rx_urb rx_urbs[RX_URB_COUNT];
    bool rx_running;

    /* --- TX ------------------------------------------------------------- */
    /* Fila de SKBs aguardando transmissão */
    struct sk_buff_head tx_queue;
    /* Spinlock protege a fila TX (usada em contexto de interrupção) */
    spinlock_t tx_lock;
    /* Contador de URBs TX ativos — usado para controle de fluxo */
    atomic_t tx_urbs_active;

    /* --- Hardware ------------------------------------------------------- */
    bool chip_up;                       /* Chip foi inicializado? */
    u8   mac_addr[ETH_ALEN];           /* Endereço MAC lido do EFUSE */

    /* Canal atual (ponteiro para struct do mac80211) */
    struct cfg80211_chan_def chandef;

    /* Mutex protege operações de inicialização/desinicialização */
    struct mutex mutex;

    /* Workqueue para operações que não podem rodar em contexto de interrupção */
    struct workqueue_struct *wq;
    struct work_struct      tx_work;    /* Trabalho de transmissão */
    struct work_struct      rx_work;    /* Trabalho de processamento RX */
};

/* =========================================================================
 * Protótipos exportados entre os módulos de compilação
 * ========================================================================= */

/* usb.c */
int  rtl8188ftv_usb_probe(struct usb_interface *intf,
                          const struct usb_device_id *id);
void rtl8188ftv_usb_disconnect(struct usb_interface *intf);
int  rtl8188ftv_usb_read_reg(struct rtl8188ftv_priv *priv,
                             u16 addr, void *buf, u16 len);
int  rtl8188ftv_usb_write_reg(struct rtl8188ftv_priv *priv,
                              u16 addr, void *buf, u16 len);
int  rtl8188ftv_usb_tx(struct rtl8188ftv_priv *priv, struct sk_buff *skb,
                       unsigned int ep);
int  rtl8188ftv_usb_start_rx(struct rtl8188ftv_priv *priv);
void rtl8188ftv_usb_stop_rx(struct rtl8188ftv_priv *priv);

/* hal.c */
int  rtl8188ftv_hal_init(struct rtl8188ftv_priv *priv);
void rtl8188ftv_hal_deinit(struct rtl8188ftv_priv *priv);
int  rtl8188ftv_hal_set_channel(struct rtl8188ftv_priv *priv, int channel);
void rtl8188ftv_hal_read_mac(struct rtl8188ftv_priv *priv);

/* mac.c */
int  rtl8188ftv_mac_register(struct rtl8188ftv_priv *priv);
void rtl8188ftv_mac_unregister(struct rtl8188ftv_priv *priv);
void rtl8188ftv_mac_rx_frame(struct rtl8188ftv_priv *priv,
                             u8 *data, u32 len);
const struct ieee80211_ops *rtl8188ftv_get_mac_ops(void);

/* =========================================================================
 * Helpers de acesso a registradores (inline wrappers)
 *
 * Wrappers tipados evitam erros de tamanho ao ler/escrever registradores.
 * O hardware RTL8188FTV usa registradores de 8, 16 e 32 bits.
 * ========================================================================= */

static inline u8 rtl_read8(struct rtl8188ftv_priv *priv, u16 addr)
{
    u8 val = 0;
    rtl8188ftv_usb_read_reg(priv, addr, &val, 1);
    return val;
}

static inline u16 rtl_read16(struct rtl8188ftv_priv *priv, u16 addr)
{
    __le16 val = 0;
    rtl8188ftv_usb_read_reg(priv, addr, &val, 2);
    return le16_to_cpu(val);
}

static inline u32 rtl_read32(struct rtl8188ftv_priv *priv, u16 addr)
{
    __le32 val = 0;
    rtl8188ftv_usb_read_reg(priv, addr, &val, 4);
    return le32_to_cpu(val);
}

static inline void rtl_write8(struct rtl8188ftv_priv *priv, u16 addr, u8 val)
{
    rtl8188ftv_usb_write_reg(priv, addr, &val, 1);
}

static inline void rtl_write16(struct rtl8188ftv_priv *priv, u16 addr, u16 val)
{
    __le16 v = cpu_to_le16(val);
    rtl8188ftv_usb_write_reg(priv, addr, &v, 2);
}

static inline void rtl_write32(struct rtl8188ftv_priv *priv, u16 addr, u32 val)
{
    __le32 v = cpu_to_le32(val);
    rtl8188ftv_usb_write_reg(priv, addr, &v, 4);
}

#endif /* _RTL8188FTV_H_ */
