// SPDX-License-Identifier: GPL-2.0-only
/*
 * hal.c — Hardware Abstraction Layer do RTL8188FTV
 *
 * Esta camada é responsável por interagir diretamente com o hardware:
 * sequência de power-on, leitura do endereço MAC do EFUSE, configuração
 * do canal, calibração do RF e Baseband.
 *
 * Conceitos importantes desta camada:
 * =====================================
 *
 * 1. EFUSE
 *    O chip contém uma memória "one-time-programmable" chamada EFUSE, que
 *    armazena o endereço MAC, parâmetros de calibração, versão do board etc.
 *    Lemos ela durante a inicialização.
 *
 * 2. BASEBAND (BB)
 *    O Baseband processor faz modulação/demodulação (OFDM, CCK).
 *    Seus registradores são acessados INDIRETAMENTE via dois registradores
 *    de controle: HSSI_PARA2 (endereço) e HSSI_PARA1 (dados).
 *
 * 3. RF (Radio Frequency)
 *    O RF frontend (PA, LNA, PLL) é controlado via interface de 3 fios
 *    (RF serial interface). O acesso também é indireto via BB.
 *
 * 4. POWER SEQUENCE
 *    Chips WiFi têm sequências de power-on estritas. Escrever registradores
 *    fora de ordem pode deixar o chip em estado indefinido.
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>

#include "rtl8188ftv.h"
#include "rtl8188ftv_reg.h"

/* =========================================================================
 * Acesso ao EFUSE
 *
 * O EFUSE é uma memória de 512 bytes de leitura única (fuses queimados
 * na fábrica). Acessamos byte a byte via registradores de controle.
 * ========================================================================= */

/**
 * rtl8188ftv_efuse_read_byte - Lê um byte do EFUSE
 *
 * @priv: contexto do driver
 * @addr: endereço no EFUSE (0–511)
 * @val:  ponteiro para receber o valor lido
 */
static int rtl8188ftv_efuse_read_byte(struct rtl8188ftv_priv *priv,
                                      u16 addr, u8 *val)
{
    u32 efuse_ctrl;
    int timeout = 100;

    /*
     * Escreve o endereço e dispara a leitura no REG_EFUSE_CTRL.
     * Bits [15:8] = endereço
     * Bit  [31]   = 0 = operação de leitura
     * Bit  [30]   = 1 = dispara a operação ("access start")
     */
    efuse_ctrl = (addr << 8) | BIT(30);
    rtl_write32(priv, REG_EFUSE_CTRL, efuse_ctrl);

    /* Aguarda o bit 30 ser limpo pelo hardware (indica conclusão) */
    do {
        udelay(50);
        efuse_ctrl = rtl_read32(priv, REG_EFUSE_CTRL);
        if (!(efuse_ctrl & BIT(30))) {
            /* Bit 30 limpo = leitura completa. Dado nos bits [7:0] */
            *val = (u8)(efuse_ctrl & 0xFF);
            return 0;
        }
    } while (--timeout > 0);

    return -ETIMEDOUT;
}

/**
 * rtl8188ftv_hal_read_mac - Lê o endereço MAC do EFUSE
 *
 * O MAC address está em 6 bytes consecutivos no offset EFUSE_MAC_ADDR_OFFSET.
 * Se o EFUSE for inválido (todos 0xFF ou 0x00), gera um endereço aleatório.
 */
void rtl8188ftv_hal_read_mac(struct rtl8188ftv_priv *priv)
{
    int i, ret;
    u8 mac[ETH_ALEN];

    /* Habilita acesso ao EFUSE */
    rtl_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_ON);
    udelay(5);

    for (i = 0; i < ETH_ALEN; i++) {
        ret = rtl8188ftv_efuse_read_byte(priv,
                                         EFUSE_MAC_ADDR_OFFSET + i,
                                         &mac[i]);
        if (ret) {
            dev_warn(&priv->udev->dev,
                     "EFUSE MAC read timeout no byte %d\n", i);
            goto random_mac;
        }
    }

    /* Desabilita acesso ao EFUSE */
    rtl_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);

    /* Valida: endereço não pode ser broadcast, multicast ou zero */
    if (!is_valid_ether_addr(mac)) {
        dev_warn(&priv->udev->dev,
                 "EFUSE MAC inválido: %pM — gerando aleatório\n", mac);
        goto random_mac;
    }

    memcpy(priv->mac_addr, mac, ETH_ALEN);
    dev_info(&priv->udev->dev, "MAC do EFUSE: %pM\n", priv->mac_addr);
    return;

random_mac:
    rtl_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);
    /* eth_random_addr() gera um MAC unicast aleatório com bit U/L setado */
    eth_random_addr(priv->mac_addr);
    dev_warn(&priv->udev->dev,
             "Usando MAC aleatório: %pM\n", priv->mac_addr);
}

/* =========================================================================
 * Sequência de Power-On
 * ========================================================================= */

/**
 * rtl8188ftv_hal_power_on - Liga o chip e habilita os blocos necessários
 *
 * Segue a sequência especificada no Reference Design da Realtek.
 * A ordem é crítica — o chip pode travar se os registradores forem
 * escritos na ordem errada.
 */
static int rtl8188ftv_hal_power_on(struct rtl8188ftv_priv *priv)
{
    u16 val16;

    /*
     * Passo 1: Habilitar PMC (Power Management Controller)
     * O APS_FSMCO controla a máquina de estados de energia do chip.
     * Limpamos o bit PDN_EN para sair do estado de power-down.
     */
    val16 = rtl_read16(priv, REG_APS_FSMCO);
    val16 &= ~(u16)APS_FSMCO_APFM_OFF;
    rtl_write16(priv, REG_APS_FSMCO, val16);

    /*
     * Passo 2: Habilitar MAC e BB
     * REG_SYS_FUNC_EN controla quais blocos funcionais estão ligados.
     */
    rtl_write16(priv, REG_SYS_FUNC_EN,
                SYS_FUNC_BBRSTB     |
                SYS_FUNC_BB_GLB_RSTN |
                SYS_FUNC_USBA       |
                SYS_FUNC_UPLL       |
                SYS_FUNC_USBD       |
                SYS_FUNC_MREGEN     |
                SYS_FUNC_WLON);

    /* Aguarda estabilização dos PLLs */
    mdelay(1);

    /*
     * Passo 3: Habilitar MAC function via APS_FSMCO
     * Bit APFM_ONMAC dispara a sequência de power-on do MAC.
     * O hardware processa a sequência automaticamente e limpa o bit
     * quando termina.
     */
    val16 = rtl_read16(priv, REG_APS_FSMCO);
    val16 |= APS_FSMCO_MAC_FUNC_EN;
    rtl_write16(priv, REG_APS_FSMCO, val16);

    /* Aguarda o hardware completar a sequência (até 10ms) */
    {
        int timeout = 100;
        do {
            udelay(100);
            if (!(rtl_read16(priv, REG_APS_FSMCO) & APS_FSMCO_MAC_FUNC_EN))
                break;
        } while (--timeout > 0);

        if (timeout == 0) {
            dev_err(&priv->udev->dev,
                    "Power-on timeout: APS_FSMCO não zerou\n");
            return -ETIMEDOUT;
        }
    }

    dev_dbg(&priv->udev->dev, "Power-on completo\n");
    return 0;
}

/* =========================================================================
 * Inicialização do MAC
 * ========================================================================= */

static int rtl8188ftv_hal_init_mac(struct rtl8188ftv_priv *priv)
{
    /*
     * Habilita RX e TX no Command Register.
     * A ordem importa: primeiro habilitar DMA, depois MAC.
     */
    rtl_write8(priv, REG_CR,
               CR_HCI_TXDMA_EN | CR_HCI_RXDMA_EN |
               CR_TXDMA_EN     | CR_RXDMA_EN      |
               CR_PROTOCOL_EN  | CR_SCHEDULE_EN   |
               CR_MAC_TX_EN    | CR_MAC_RX_EN);

    /* Configura tamanho de página do DMA para 512 bytes */
    rtl_write8(priv, REG_PBP,
               (PBP_PAGE_SIZE_512 << 4) | PBP_PAGE_SIZE_512);

    /*
     * Configura o Receive Configuration Register.
     * Aqui definimos quais tipos de frames o hardware deve entregar:
     * - APM: frames destinados ao nosso MAC
     * - AM:  multicast
     * - AB:  broadcast
     * - APP_PHYSTS: inclui estatísticas do PHY no buffer RX (RSSI, etc)
     */
    rtl_write32(priv, REG_RCR,
                RCR_APM        |
                RCR_AM         |
                RCR_AB         |
                RCR_AMF        |
                RCR_APP_PHYSTS |
                RCR_APP_ICV    |
                RCR_APP_MIC);

    /* Escreve o endereço MAC no hardware */
    rtl_write32(priv, REG_MACID,
                priv->mac_addr[0] | (priv->mac_addr[1] << 8) |
                (priv->mac_addr[2] << 16) | (priv->mac_addr[3] << 24));
    rtl_write16(priv, REG_MACID + 4,
                priv->mac_addr[4] | (priv->mac_addr[5] << 8));

    /* Configura parâmetros EDCA padrão para as 4 filas de QoS:
     * Formato: [31:16]=TXOP, [15:12]=AIFS, [11:8]=ECWmax, [7:4]=ECWmin, [3:0]=AIFSN */
    rtl_write32(priv, REG_EDCA_BE_PARAM, 0x005EA42B); /* Best Effort */
    rtl_write32(priv, REG_EDCA_BK_PARAM, 0x0000A44F); /* Background */
    rtl_write32(priv, REG_EDCA_VI_PARAM, 0x005EA324); /* Video */
    rtl_write32(priv, REG_EDCA_VO_PARAM, 0x002FA226); /* Voice */

    return 0;
}

/* =========================================================================
 * Inicialização do Baseband (BB)
 *
 * O Baseband é configurado escrevendo tabelas de pares (endereço, valor)
 * via interface indireta. Os valores são derivados do Reference Design
 * da Realtek e representam calibração de RF, filtros digitais, etc.
 * ========================================================================= */

/* Registradores de acesso indireto ao BB */
#define REG_FPGA0_XA_HSSIPAR2      0x0824  /* BB register address */
#define REG_FPGA0_XA_HSSIPAR1      0x0820  /* BB register data */
#define BB_ACCESS_FLAG             BIT(16) /* "write" flag */

static void rtl8188ftv_bb_write(struct rtl8188ftv_priv *priv,
                                u32 reg_addr, u32 val)
{
    /*
     * Acesso indireto ao Baseband:
     * 1. Escreve o valor no registro de dados
     * 2. Escreve o endereço + flag no registro de controle
     * O hardware então executa a escrita no BB internamente.
     */
    rtl_write32(priv, REG_FPGA0_XA_HSSIPAR1, val);
    rtl_write32(priv, REG_FPGA0_XA_HSSIPAR2,
                reg_addr | BB_ACCESS_FLAG);
    udelay(1);
}

static u32 rtl8188ftv_bb_read(struct rtl8188ftv_priv *priv, u32 reg_addr)
{
    rtl_write32(priv, REG_FPGA0_XA_HSSIPAR2, reg_addr & ~BB_ACCESS_FLAG);
    udelay(1);
    return rtl_read32(priv, REG_FPGA0_XA_HSSIPAR1);
}

/* Tabela mínima de inicialização do BB para 802.11b/g/n */
static const struct { u32 addr; u32 val; } bb_init_table[] = {
    /* Habilita os caminhos de TX e RX */
    { 0x0800, 0x80040000 },
    { 0x0804, 0x00000003 },
    /* Configura ganho AGC (Automatic Gain Control) */
    { 0x0808, 0x0000FC00 },
    { 0x080C, 0x0000000A },
    /* Parâmetros OFDM */
    { 0x0810, 0x10001331 },
    { 0x0814, 0x020C3D10 },
    /* Parâmetros CCK (802.11b) */
    { 0x0A00, 0x00D047C8 },
    { 0x0A04, 0x01FF800C },
    { 0x0A08, 0x8C838300 },
    /* Sentinel */
    { 0, 0 }
};

static int rtl8188ftv_hal_init_bb(struct rtl8188ftv_priv *priv)
{
    int i;

    for (i = 0; bb_init_table[i].addr != 0; i++) {
        rtl8188ftv_bb_write(priv,
                            bb_init_table[i].addr,
                            bb_init_table[i].val);
    }

    dev_dbg(&priv->udev->dev,
            "BB inicializado (%d registradores)\n", i);
    return 0;
}

/* =========================================================================
 * Configuração de Canal
 * ========================================================================= */

/*
 * Tabela de frequências dos canais WiFi 2.4GHz (canais 1–14).
 * A frequência central de cada canal em MHz:
 *   Canal 1 = 2412 MHz
 *   Canal N = 2407 + N*5 MHz  (para N=1..13)
 *   Canal 14 = 2484 MHz (apenas Japão)
 *
 * O chip precisa deste valor para configurar o PLL do RF.
 */
static const u32 channel_freq_mhz[14] = {
    2412, 2417, 2422, 2427, 2432, 2437, 2442,
    2447, 2452, 2457, 2462, 2467, 2472, 2484
};

/**
 * rtl8188ftv_hal_set_channel - Sintoniza o RF no canal especificado
 *
 * @priv:    contexto do driver
 * @channel: número do canal (1–14)
 */
int rtl8188ftv_hal_set_channel(struct rtl8188ftv_priv *priv, int channel)
{
    u32 bb_val;

    if (channel < 1 || channel > 14) {
        dev_err(&priv->udev->dev,
                "Canal inválido: %d\n", channel);
        return -EINVAL;
    }

    dev_dbg(&priv->udev->dev,
            "Configurando canal %d (%u MHz)\n",
            channel, channel_freq_mhz[channel - 1]);

    /*
     * A configuração de canal no RTL8188F é feita via RF serial interface.
     * O registrador RF channel é escrito via BB (acesso indireto duplo):
     *
     *   BB[0x018] = canal  → BB repassa ao RF via SPI de 3 fios
     *
     * O valor do registrador inclui o número do canal nos bits [9:4]
     * e flags de configuração nos bits restantes.
     */
    bb_val = rtl8188ftv_bb_read(priv, 0x818);
    bb_val = (bb_val & ~0x1F) | (channel & 0x1F);
    rtl8188ftv_bb_write(priv, 0x818, bb_val);

    /* Aguarda PLL travar na nova frequência */
    mdelay(1);

    return 0;
}

/* =========================================================================
 * Inicialização e Desinicialização principal
 * ========================================================================= */

/**
 * rtl8188ftv_hal_init - Sequência completa de inicialização do hardware
 *
 * Esta função deve ser chamada uma única vez após o probe USB.
 * Segue a ordem:
 *   1. Power on
 *   2. Leitura do MAC
 *   3. Init MAC
 *   4. Init Baseband
 *   5. Inicia no canal 6 (padrão)
 */
int rtl8188ftv_hal_init(struct rtl8188ftv_priv *priv)
{
    int ret;

    dev_info(&priv->udev->dev, "HAL: iniciando sequência de power-on\n");

    ret = rtl8188ftv_hal_power_on(priv);
    if (ret) {
        dev_err(&priv->udev->dev, "HAL: power-on falhou\n");
        return ret;
    }

    rtl8188ftv_hal_read_mac(priv);

    ret = rtl8188ftv_hal_init_mac(priv);
    if (ret) {
        dev_err(&priv->udev->dev, "HAL: init MAC falhou\n");
        return ret;
    }

    ret = rtl8188ftv_hal_init_bb(priv);
    if (ret) {
        dev_err(&priv->udev->dev, "HAL: init BB falhou\n");
        return ret;
    }

    /* Canal padrão: 6 (2437 MHz — centro da banda 2.4GHz) */
    ret = rtl8188ftv_hal_set_channel(priv, 6);
    if (ret)
        return ret;

    priv->chip_up = true;
    dev_info(&priv->udev->dev, "HAL: hardware inicializado\n");
    return 0;
}

/**
 * rtl8188ftv_hal_deinit - Desliga o chip de forma segura
 */
void rtl8188ftv_hal_deinit(struct rtl8188ftv_priv *priv)
{
    if (!priv->chip_up)
        return;

    priv->chip_up = false;

    /* Desabilita TX e RX no MAC */
    rtl_write8(priv, REG_CR, 0);

    /* Coloca o chip em power-down */
    rtl_write16(priv, REG_APS_FSMCO, APS_FSMCO_APFM_OFF);

    dev_info(&priv->udev->dev, "HAL: hardware desligado\n");
}
