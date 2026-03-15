// SPDX-License-Identifier: GPL-2.0-only
/*
 * mac.c — Integração com o framework mac80211 do kernel Linux
 *
 * O mac80211 é o framework do kernel que abstrai o gerenciamento do protocolo
 * 802.11 para drivers "softmac" — ou seja, drivers onde a lógica do MAC
 * (controle de acesso ao meio) é implementada em software no kernel.
 *
 * Arquitetura mac80211:
 * ======================
 *
 *   ┌─────────────────────────────────────────────┐
 *   │           Userspace (NetworkManager, wpa_supplicant, iw) │
 *   └──────────────────┬──────────────────────────┘
 *                      │ nl80211 (Netlink)
 *   ┌──────────────────▼──────────────────────────┐
 *   │             cfg80211 (kernel)                │
 *   │  regulatory, scan, connect policies          │
 *   └──────────────────┬──────────────────────────┘
 *                      │
 *   ┌──────────────────▼──────────────────────────┐
 *   │              mac80211 (kernel)               │
 *   │  MLME, power save, QoS, aggregation, crypto │
 *   └──────────────────┬──────────────────────────┘
 *                      │ ieee80211_ops callbacks
 *   ┌──────────────────▼──────────────────────────┐
 *   │         Este driver (mac.c + usb.c + hal.c) │
 *   └──────────────────┬──────────────────────────┘
 *                      │ I/O USB
 *                 [ RTL8188FTV ]
 *
 * Nosso driver implementa ieee80211_ops: um conjunto de callbacks que o
 * mac80211 chama para controlar o hardware. Exemplos:
 *
 *   .tx          — mac80211 quer transmitir um frame
 *   .start       — interface WiFi está sendo ligada (ifconfig up)
 *   .stop        — interface WiFi está sendo desligada
 *   .add_interface  — nova interface virtual (STA/AP/Monitor)
 *   .config      — configuração mudou (canal, potência TX, etc.)
 *   .bss_info_changed — informações do BSS mudaram (associação, etc.)
 *   .hw_scan     — iniciar varredura de redes disponíveis
 *   .set_key     — instalar chave de criptografia no hardware
 */

#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/mac80211.h>

#include "rtl8188ftv.h"
#include "rtl8188ftv_reg.h"

/* =========================================================================
 * Descrição das bandas de frequência suportadas
 *
 * O mac80211 precisa saber exatamente quais frequências e taxas de dados
 * o hardware suporta. Declaramos a banda 2.4GHz com todos os canais e
 * taxas do 802.11b/g/n.
 * ========================================================================= */

/*
 * Taxas de dados suportadas em 802.11b (CCK):
 *   1 Mbps, 2 Mbps, 5.5 Mbps, 11 Mbps
 * E 802.11g (OFDM):
 *   6, 9, 12, 18, 24, 36, 48, 54 Mbps
 *
 * O campo .hw_value é usado internamente para referenciar a taxa.
 * O campo .bitrate está em unidades de 100 Kbps.
 */
static struct ieee80211_rate rtl8188ftv_rates[] = {
    /* 802.11b — CCK */
    { .bitrate = 10,   .hw_value = 0x00, },   /* 1 Mbps */
    { .bitrate = 20,   .hw_value = 0x01, },   /* 2 Mbps */
    { .bitrate = 55,   .hw_value = 0x02, },   /* 5.5 Mbps */
    { .bitrate = 110,  .hw_value = 0x03, },   /* 11 Mbps */
    /* 802.11g — OFDM */
    { .bitrate = 60,   .hw_value = 0x04, },   /* 6 Mbps */
    { .bitrate = 90,   .hw_value = 0x05, },   /* 9 Mbps */
    { .bitrate = 120,  .hw_value = 0x06, },   /* 12 Mbps */
    { .bitrate = 180,  .hw_value = 0x07, },   /* 18 Mbps */
    { .bitrate = 240,  .hw_value = 0x08, },   /* 24 Mbps */
    { .bitrate = 360,  .hw_value = 0x09, },   /* 36 Mbps */
    { .bitrate = 480,  .hw_value = 0x0A, },   /* 48 Mbps */
    { .bitrate = 540,  .hw_value = 0x0B, },   /* 54 Mbps */
};

/*
 * Lista de canais 2.4GHz disponíveis (canais 1–13 para uso global).
 * O campo .hw_value é o número do canal (1–13).
 * O campo .center_freq é a frequência central em MHz.
 */
static struct ieee80211_channel rtl8188ftv_channels[] = {
    { .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9  },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
    { .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
};

/*
 * Capacidades MCS (Modulation and Coding Scheme) para 802.11n.
 * O RTL8188F é 1T1R (1 antena TX, 1 antena RX), portanto suporta
 * apenas MCS0–MCS7 (single spatial stream).
 *
 * MCS0  = BPSK   1/2  → 6.5 Mbps  (HT20) / 7.2 Mbps  (HT20 SGI)
 * MCS7  = 64-QAM 5/6  → 65 Mbps   (HT20) / 72.2 Mbps (HT20 SGI)
 */
static struct ieee80211_sta_ht_cap rtl8188ftv_ht_cap = {
    .ht_supported = true,
    .cap = IEEE80211_HT_CAP_SGI_20,     /* Short Guard Interval em 20MHz */
    .ampdu_factor   = IEEE80211_HT_MAX_AMPDU_64K,
    .ampdu_density  = IEEE80211_HT_MPDU_DENSITY_16,
    .mcs = {
        /* rx_mask: bit N=1 significa que MCS N está suportado em RX */
        .rx_mask    = { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* MCS0–7 */
        .rx_highest = cpu_to_le16(72), /* 72.2 Mbps com SGI */
        .tx_params  = IEEE80211_HT_MCS_TX_DEFINED,
    },
};

/* Descrição completa da banda 2.4GHz */
static struct ieee80211_supported_band rtl8188ftv_band_2g = {
    .band       = NL80211_BAND_2GHZ,
    .channels   = rtl8188ftv_channels,
    .n_channels = ARRAY_SIZE(rtl8188ftv_channels),
    .bitrates   = rtl8188ftv_rates,
    .n_bitrates = ARRAY_SIZE(rtl8188ftv_rates),
    .ht_cap     = /* será copiado do rtl8188ftv_ht_cap acima */
        { .ht_supported = false }, /* inicializado em mac_register() */
};

/* =========================================================================
 * Callbacks ieee80211_ops
 *
 * Estas funções são chamadas pelo mac80211 para controlar o hardware.
 * ========================================================================= */

/**
 * rtl8188ftv_op_tx - Transmite um frame (chamado pelo mac80211)
 *
 * O mac80211 chama esta função quando tem um frame pronto para TX.
 * O skb já contém o frame 802.11 completo (header + payload).
 *
 * Nossa responsabilidade:
 *   1. Preparar o TX descriptor (32 bytes de metadados)
 *   2. Preencher campos: tamanho do pacote, tipo de frame, taxa de TX
 *   3. Preappend o descriptor no skb
 *   4. Submeter ao USB via rtl8188ftv_usb_tx()
 */
static void rtl8188ftv_op_tx(struct ieee80211_hw *hw,
                             struct ieee80211_tx_control *control,
                             struct sk_buff *skb)
{
    struct rtl8188ftv_priv   *priv = hw->priv;
    struct rtl8188ftv_txdesc *txdesc;
    struct ieee80211_hdr     *hdr  = (void *)skb->data;
    u32 pktlen = skb->len;
    unsigned int ep;
    int ret;

    /*
     * Adiciona espaço para o TX descriptor no início do skb.
     * skb_push() move o ponteiro skb->data para trás, abrindo espaço.
     * Não copia dados — apenas expande o "headroom".
     */
    txdesc = (struct rtl8188ftv_txdesc *)skb_push(skb,
                                                   RTL8188FTV_TX_DESC_SIZE);
    memset(txdesc, 0, RTL8188FTV_TX_DESC_SIZE);

    /*
     * TXDW0: tamanho total do pacote (sem o descriptor),
     * offset = tamanho do descriptor, bit FS=first segment, LS=last segment.
     * Para frames não-agregados, FS=LS=1 (frame único).
     */
    txdesc->txdw0 = cpu_to_le32(
        pktlen |                            /* bits [15:0] = pktlen */
        (RTL8188FTV_TX_DESC_SIZE << 16) |  /* bits [23:16] = offset */
        BIT(26) |                           /* LS = last segment */
        BIT(27)                             /* FS = first segment */
    );

    /*
     * Determina o endpoint de TX com base no tipo de frame:
     * - Management/Control: usa EP_TX_HIGH (alta prioridade)
     * - Data: usa EP_TX_DATA (normal)
     *
     * ieee80211_is_mgmt() e ieee80211_is_ctl() são helpers do kernel
     * que verificam o campo fc (frame control) do header 802.11.
     */
    if (ieee80211_is_mgmt(hdr->frame_control) ||
        ieee80211_is_ctl(hdr->frame_control)) {
        ep = priv->ep_tx_high ? priv->ep_tx_high : priv->ep_tx_data;
    } else {
        ep = priv->ep_tx_data;
    }

    ret = rtl8188ftv_usb_tx(priv, skb, ep);
    if (ret) {
        /* TX falhou — libera o skb para não vazar memória */
        dev_kfree_skb_any(skb);
        dev_warn(&priv->udev->dev, "TX falhou: %d\n", ret);
    }
}

/**
 * rtl8188ftv_op_start - Liga a interface WiFi
 *
 * Chamado quando o usuário faz `ip link set wlan0 up` ou equivalente.
 * Neste ponto o hardware já foi inicializado no probe, então só
 * precisamos iniciar o loop de RX.
 */
static int rtl8188ftv_op_start(struct ieee80211_hw *hw)
{
    struct rtl8188ftv_priv *priv = hw->priv;
    int ret;

    dev_info(&priv->udev->dev, "Interface iniciando...\n");

    ret = rtl8188ftv_usb_start_rx(priv);
    if (ret) {
        dev_err(&priv->udev->dev, "Falha ao iniciar RX: %d\n", ret);
        return ret;
    }

    return 0;
}

/**
 * rtl8188ftv_op_stop - Desliga a interface WiFi
 *
 * Chamado em `ip link set wlan0 down`. Para os URBs de RX.
 */
static void rtl8188ftv_op_stop(struct ieee80211_hw *hw)
{
    struct rtl8188ftv_priv *priv = hw->priv;

    dev_info(&priv->udev->dev, "Interface parando...\n");
    rtl8188ftv_usb_stop_rx(priv);
}

/**
 * rtl8188ftv_op_add_interface - Adiciona uma interface virtual 802.11
 *
 * O mac80211 suporta múltiplas interfaces virtuais (STA, AP, Monitor).
 * No RTL8188F, suportamos apenas uma interface por limitação do hardware
 * (apenas 1 instância de MAC hardware).
 *
 * @hw:  ieee80211_hw do driver
 * @vif: a nova interface virtual (contém type, addr, etc.)
 */
static int rtl8188ftv_op_add_interface(struct ieee80211_hw *hw,
                                       struct ieee80211_vif *vif)
{
    struct rtl8188ftv_priv *priv = hw->priv;

    if (priv->vif) {
        dev_warn(&priv->udev->dev,
                 "add_interface: já temos uma vif ativa\n");
        return -EBUSY;
    }

    priv->vif = vif;

    dev_info(&priv->udev->dev,
             "Interface adicionada: tipo=%d addr=%pM\n",
             vif->type, vif->addr);

    return 0;
}

/**
 * rtl8188ftv_op_remove_interface - Remove a interface virtual
 */
static void rtl8188ftv_op_remove_interface(struct ieee80211_hw *hw,
                                           struct ieee80211_vif *vif)
{
    struct rtl8188ftv_priv *priv = hw->priv;

    priv->vif = NULL;
    dev_info(&priv->udev->dev, "Interface removida\n");
}

/**
 * rtl8188ftv_op_config - Aplica nova configuração (canal, potência TX, etc.)
 *
 * O mac80211 chama esta função quando qualquer parâmetro de configuração
 * muda. O campo `changed` indica quais parâmetros foram alterados
 * (máscara de bits IEEE80211_CONF_CHANGE_*).
 */
static int rtl8188ftv_op_config(struct ieee80211_hw *hw, u32 changed)
{
    struct rtl8188ftv_priv *priv = hw->priv;
    struct ieee80211_conf  *conf = &hw->conf;

    /* Mudança de canal */
    if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
        int channel = conf->chandef.chan->hw_value;

        dev_dbg(&priv->udev->dev,
                "config: mudando para canal %d\n", channel);

        return rtl8188ftv_hal_set_channel(priv, channel);
    }

    return 0;
}

/**
 * rtl8188ftv_op_bss_info_changed - Informa mudanças no BSS atual
 *
 * Chamado quando o estado de associação muda, ou quando parâmetros como
 * BSSID, beacon interval, etc. são atualizados.
 *
 * @changed: máscara de bits BSS_CHANGED_* indicando o que mudou
 */
static void rtl8188ftv_op_bss_info_changed(struct ieee80211_hw *hw,
                                           struct ieee80211_vif *vif,
                                           struct ieee80211_bss_conf *info,
                                           u64 changed)
{
    struct rtl8188ftv_priv *priv = hw->priv;

    /* Quando nos associamos a um AP, atualizamos o BSSID no hardware */
    if (changed & BSS_CHANGED_BSSID) {
        dev_dbg(&priv->udev->dev,
                "BSS: novo BSSID %pM\n", info->bssid);

        /* Escreve BSSID no registrador do chip */
        rtl_write32(priv, REG_BSSID,
                    info->bssid[0] | (info->bssid[1] << 8) |
                    (info->bssid[2] << 16) | (info->bssid[3] << 24));
        rtl_write16(priv, REG_BSSID + 4,
                    info->bssid[4] | (info->bssid[5] << 8));
    }

    /* Quando associado, ativa filtragem BSSID no hardware */
    if (changed & BSS_CHANGED_ASSOC) {
        u32 rcr = rtl_read32(priv, REG_RCR);

        if (vif->cfg.assoc) {
            dev_info(&priv->udev->dev,
                     "Associado ao AP, ativando filtro BSSID\n");
            rcr |= RCR_CBSSID_DATA | RCR_CBSSID_BCN;
        } else {
            dev_info(&priv->udev->dev,
                     "Desassociado, desativando filtro BSSID\n");
            rcr &= ~(RCR_CBSSID_DATA | RCR_CBSSID_BCN);
        }

        rtl_write32(priv, REG_RCR, rcr);
    }
}

/**
 * rtl8188ftv_op_configure_filter - Configura filtragem de quadros no RX
 *
 * O mac80211 usa isso para ativar modo promíscuo, filtrar broadcasts, etc.
 * Mapeamos as flags do mac80211 para os bits do RCR do hardware.
 *
 * @changed_flags: quais flags mudaram
 * @total_flags:   estado final desejado (entrada e saída)
 */
static void rtl8188ftv_op_configure_filter(struct ieee80211_hw *hw,
                                           unsigned int changed_flags,
                                           unsigned int *total_flags,
                                           u64 multicast)
{
    struct rtl8188ftv_priv *priv = hw->priv;
    u32 rcr;

    rcr = rtl_read32(priv, REG_RCR);

    /*
     * FIF_PROMISC_IN_BSS foi removido no kernel 5.x. Modo promíscuo agora
     * é tratado pelo mac80211 internamente. Mantemos RCR_AAP desabilitado
     * por padrão para não saturar o driver com frames de outros dispositivos.
     */
    rcr &= ~RCR_AAP;

    /* Aceitar pacotes com FCS errado — útil para debug/monitor mode */
    if (*total_flags & FIF_FCSFAIL)
        rcr |= RCR_ACRC32;
    else
        rcr &= ~RCR_ACRC32;

    /* Aceitar multicast */
    if (*total_flags & FIF_ALLMULTI)
        rcr |= RCR_AM;
    else
        rcr &= ~RCR_AM;

    rtl_write32(priv, REG_RCR, rcr);

    /* Informa ao mac80211 o que realmente conseguimos implementar */
    *total_flags &= FIF_ALLMULTI | FIF_FCSFAIL |
                    FIF_BCN_PRBRESP_PROMISC | FIF_CONTROL;
}

/*
 * Tabela de operações: ponteiros para todas as nossas callbacks.
 * Esta struct é passada para ieee80211_alloc_hw() em usb.c.
 */
static const struct ieee80211_ops rtl8188ftv_mac_ops = {
    .tx                 = rtl8188ftv_op_tx,
    .start              = rtl8188ftv_op_start,
    .stop               = rtl8188ftv_op_stop,
    .add_interface      = rtl8188ftv_op_add_interface,
    .remove_interface   = rtl8188ftv_op_remove_interface,
    .config             = rtl8188ftv_op_config,
    .bss_info_changed   = rtl8188ftv_op_bss_info_changed,
    .configure_filter   = rtl8188ftv_op_configure_filter,
};

/* Getter para as ops (usado em usb.c no ieee80211_alloc_hw) */
const struct ieee80211_ops *rtl8188ftv_get_mac_ops(void)
{
    return &rtl8188ftv_mac_ops;
}

/* =========================================================================
 * Registro e desregistro no mac80211
 * ========================================================================= */

/**
 * rtl8188ftv_mac_register - Registra o driver no mac80211
 *
 * Após esta chamada, o sistema reconhece o adaptador como wlan0 (ou similar).
 * O NetworkManager e outras ferramentas podem então detectá-lo.
 */
int rtl8188ftv_mac_register(struct rtl8188ftv_priv *priv)
{
    struct ieee80211_hw *hw = priv->hw;
    int ret;

    /* Copia as capacidades HT para a band struct */
    memcpy(&rtl8188ftv_band_2g.ht_cap, &rtl8188ftv_ht_cap,
           sizeof(rtl8188ftv_ht_cap));

    /* Registra a banda 2.4GHz no ieee80211_hw */
    hw->wiphy->bands[NL80211_BAND_2GHZ] = &rtl8188ftv_band_2g;

    /*
     * Informa ao mac80211 o tamanho extra necessário no início de
     * cada TX skb (para nosso TX descriptor de 32 bytes).
     */
    hw->extra_tx_headroom = RTL8188FTV_TX_DESC_SIZE;

    /* Número máximo de interfaces virtuais simultâneas */
    hw->wiphy->iface_combinations = NULL;
    hw->wiphy->max_scan_ssids = 1;

    /* Endereço MAC do hardware */
    SET_IEEE80211_PERM_ADDR(hw, priv->mac_addr);

    /*
     * ieee80211_register_hw() finaliza o registro:
     * - cria a interface wlanX no sistema
     * - registra no wiphy (cfg80211)
     * - torna o device visível para ferramentas userspace
     */
    ret = ieee80211_register_hw(hw);
    if (ret) {
        dev_err(&priv->udev->dev,
                "ieee80211_register_hw falhou: %d\n", ret);
        return ret;
    }

    dev_info(&priv->udev->dev,
             "Registrado no mac80211: %s\n",
             wiphy_name(hw->wiphy));
    return 0;
}

/**
 * rtl8188ftv_mac_unregister - Remove o driver do mac80211
 */
void rtl8188ftv_mac_unregister(struct rtl8188ftv_priv *priv)
{
    ieee80211_unregister_hw(priv->hw);
    dev_info(&priv->udev->dev, "Desregistrado do mac80211\n");
}

/* =========================================================================
 * Processamento de frames recebidos (RX)
 * ========================================================================= */

/**
 * rtl8188ftv_mac_rx_frame - Entrega um frame 802.11 recebido ao mac80211
 *
 * @priv: contexto do driver
 * @data: ponteiro para o início do frame 802.11 (sem o RX descriptor)
 * @len:  tamanho do frame em bytes
 *
 * O mac80211 espera receber um sk_buff com o frame 802.11 completo.
 * Preenchemos o struct ieee80211_rx_status com metadados do frame
 * (sinal RSSI, taxa de recepção, canal) antes de entregar.
 */
void rtl8188ftv_mac_rx_frame(struct rtl8188ftv_priv *priv,
                             u8 *data, u32 len)
{
    struct sk_buff           *skb;
    struct ieee80211_rx_status *rx_status;

    /* Aloca um novo sk_buff para o frame */
    skb = dev_alloc_skb(len);
    if (!skb) {
        dev_warn(&priv->udev->dev,
                 "RX: falha ao alocar skb (%u bytes)\n", len);
        return;
    }

    /* Copia os dados do frame para o skb */
    skb_put_data(skb, data, len);

    /*
     * ieee80211_rx_status é armazenado no skb usando o campo cb
     * (control buffer — 48 bytes reservados para uso do driver).
     * O mac80211 lê estes metadados ao processar o frame.
     */
    rx_status = IEEE80211_SKB_RXCB(skb);
    memset(rx_status, 0, sizeof(*rx_status));

    /* Banda e canal atuais */
    rx_status->band    = NL80211_BAND_2GHZ;
    rx_status->freq    = priv->hw->conf.chandef.chan ?
                         priv->hw->conf.chandef.chan->center_freq : 2437;

    /* Sinal recebido em dBm (placeholder — RSSI real vem do PHY status) */
    rx_status->signal  = -70;

    /* Taxa de dados: por simplicidade, reportamos 54 Mbps */
    rx_status->rate_idx = 11; /* índice em rtl8188ftv_rates */

    /*
     * ieee80211_rx_irqsafe() é seguro chamar de qualquer contexto,
     * inclusive interrupção/softirq. Ela enfileira o skb para
     * processamento no contexto de softirq do mac80211.
     *
     * Diferente de ieee80211_rx() que requer contexto de processo.
     */
    ieee80211_rx_irqsafe(priv->hw, skb);
}
