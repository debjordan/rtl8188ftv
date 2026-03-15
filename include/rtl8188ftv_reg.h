/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtl8188ftv_reg.h — Mapa de registradores do chip RTL8188FTV
 *
 * Os registradores são acessados via USB Control Transfer no EP0.
 * O RTL8188F compartilha a maior parte dos registradores com o RTL8188EU.
 *
 * Organização do espaço de endereços:
 *
 *   0x0000 – 0x00FF  →  System / Power / Clock
 *   0x0100 – 0x01FF  →  AFE (Analog Front End)
 *   0x0200 – 0x02FF  →  MAC (Media Access Control)
 *   0x0400 – 0x04FF  →  EDCA (Enhanced Distributed Channel Access)
 *   0x0500 – 0x05FF  →  WMAC (Wireless MAC)
 *   0x0600 – 0x06FF  →  MAC Addr / BSS
 *   0x0800 – 0x08FF  →  BB (Baseband) — acesso indireto
 *   0xFE00 – 0xFEFF  →  USB-específico
 *
 * Referência: drivers/net/wireless/realtek/rtl8xxxu/rtl8xxxu_regs.h
 *             drivers/net/wireless/realtek/rtlwifi/rtl8188ee/reg.h
 */

#ifndef _RTL8188FTV_REG_H_
#define _RTL8188FTV_REG_H_

/* =========================================================================
 * Bloco de Sistema (System)
 * ========================================================================= */

/* SYS_ISO_CTRL — controle de isolamento de blocos de potência */
#define REG_SYS_ISO_CTRL            0x0000

/* SYS_FUNC_EN — habilita blocos funcionais do chip */
#define REG_SYS_FUNC_EN             0x0002
#define SYS_FUNC_BBRSTB             BIT(0)   /* Baseband reset_b */
#define SYS_FUNC_BB_GLB_RSTN        BIT(1)   /* BB global reset_n */
#define SYS_FUNC_USBA               BIT(2)   /* USB PHY A */
#define SYS_FUNC_UPLL               BIT(3)   /* USB PLL */
#define SYS_FUNC_USBD               BIT(4)   /* USB device */
#define SYS_FUNC_DIO_PCIE           BIT(5)   /* DIO PCIE */
#define SYS_FUNC_DIO_RF             BIT(6)   /* DIO RF */
#define SYS_FUNC_HWPDN              BIT(7)   /* Hardware power down */
#define SYS_FUNC_MREGEN             BIT(11)  /* Memo regulador habilitado */
#define SYS_FUNC_WLON               BIT(12)  /* WLAN power on */

/* SYS_CLK — controle de clock do sistema */
#define REG_SYS_CLKR                0x0008

/* APS_FSMCO — máquina de estados de gerenciamento de energia */
#define REG_APS_FSMCO               0x0010
#define APS_FSMCO_PFM_ALDN          BIT(6)   /* Power on sequence done */
#define APS_FSMCO_MAC_FUNC_EN       BIT(8)   /* Enable MAC function */
#define APS_FSMCO_APFM_ONMAC        BIT(8)
#define APS_FSMCO_APDM_HPDN         BIT(9)
#define APS_FSMCO_PDN_EN            BIT(4)
#define APS_FSMCO_APFM_OFF          BIT(0)

/* SYS_CFG — configuração geral do sistema */
#define REG_SYS_CFG                 0x00F0

/* =========================================================================
 * Controle de Reset
 * ========================================================================= */

#define REG_RST_CTRL                0x000C
#define RST_CTRL_CPU_INIT_RUN       BIT(2)

/* =========================================================================
 * Gerenciamento de Energia (Power Management)
 * ========================================================================= */

#define REG_LDOV12D_CTRL            0x0014
#define LDOV12D_EN                  BIT(0)

/* =========================================================================
 * EFUSE — memória de configuração "one-time programmable"
 *
 * O EFUSE contém: endereço MAC, calibração de TX, versão do chip, etc.
 * O acesso é feito via registradores de controle EFUSE.
 * ========================================================================= */

#define REG_EFUSE_CTRL              0x0030  /* Controle de acesso ao EFUSE */
#define REG_EFUSE_TEST              0x0034  /* Registro de teste */
#define REG_EFUSE_ACCESS            0x00CF  /* Habilita acesso ao EFUSE */
#define EFUSE_ACCESS_ON             0x69
#define EFUSE_ACCESS_OFF            0x00

/* Endereço do MAC dentro do mapa lógico do EFUSE */
#define EFUSE_MAC_ADDR_OFFSET       0x107

/* =========================================================================
 * GPIO e LED
 * ========================================================================= */

#define REG_GPIO_MUXCFG             0x0040
#define REG_GPIO_IO_SEL             0x0042
#define REG_MAC_PINMUX_CFG          0x0043
#define REG_GPIO_PIN_CTRL           0x0044
#define REG_GPIO_INTM               0x0048
#define REG_LEDCFG0                 0x004C
#define REG_LEDCFG1                 0x004D
#define REG_LEDCFG2                 0x004E

/* =========================================================================
 * MAC — Configuração geral
 * ========================================================================= */

/* CR — Command Register: habilita TX/RX do MAC */
#define REG_CR                      0x0100
#define CR_HCI_TXDMA_EN             BIT(0)
#define CR_HCI_RXDMA_EN             BIT(1)
#define CR_TXDMA_EN                 BIT(2)
#define CR_RXDMA_EN                 BIT(3)
#define CR_PROTOCOL_EN              BIT(4)
#define CR_SCHEDULE_EN              BIT(5)
#define CR_MAC_TX_EN                BIT(6)
#define CR_MAC_RX_EN                BIT(7)
#define CR_SECURITY_EN              BIT(9)
#define CR_CALTMR_EN                BIT(10)

/* PBP — Page size para filas de TX/RX */
#define REG_PBP                     0x0104
#define PBP_PAGE_SIZE_128           0
#define PBP_PAGE_SIZE_256           1
#define PBP_PAGE_SIZE_512           2
#define PBP_PAGE_SIZE_1024          3
#define PBP_RXPBP_MASK              0x0F
#define PBP_TXPBP_MASK              0xF0

/* RCR — Receive Configuration Register */
#define REG_RCR                     0x0608
#define RCR_AAP                     BIT(0)   /* Accept All Packets */
#define RCR_APM                     BIT(1)   /* Accept Physical Match */
#define RCR_AM                      BIT(2)   /* Accept Multicast */
#define RCR_AB                      BIT(3)   /* Accept Broadcast */
#define RCR_APWRMGT                 BIT(4)   /* Accept power management */
#define RCR_ADD3                    BIT(5)   /* Accept ADDR3 match */
#define RCR_ADF                     BIT(6)   /* Accept Data Frame */
#define RCR_AICV                    BIT(7)   /* Accept ICV error */
#define RCR_ACRC32                  BIT(8)   /* Accept CRC32 error */
#define RCR_CBSSID_DATA             BIT(9)   /* Check BSSID in data */
#define RCR_CBSSID_BCN              BIT(10)  /* Check BSSID em beacon */
#define RCR_ENMBID                  BIT(11)
#define RCR_VHT_ACK                 BIT(12)
#define RCR_AMF                     BIT(13)  /* Accept Management Frame */
#define RCR_HTC_LOC_CTRL            BIT(14)
#define RCR_UC2ME                   BIT(16)
#define RCR_BW_40                   BIT(17)
#define RCR_STOP_RX_DMA             BIT(19)
#define RCR_APP_ICV                 BIT(20)  /* Adiciona ICV ao buffer RX */
#define RCR_VHT_DACK                BIT(22)
#define RCR_APP_MIC                 BIT(23)  /* Adiciona MIC ao buffer RX */
#define RCR_APP_PHYSTS              BIT(28)  /* Adiciona estatísticas PHY */
#define RCR_APP_PHYSTS_RXDESC       BIT(30)
#define RCR_APPFCS                  BIT(31)  /* Adiciona FCS ao buffer RX */

/* TCR — Transmit Configuration Register */
#define REG_TCR                     0x0604
#define TCR_ICMP_CH_EN              BIT(9)

/* =========================================================================
 * Endereços MAC e BSS
 * ========================================================================= */

#define REG_MACID                   0x0610   /* MAC address (6 bytes) */
#define REG_BSSID                   0x0618   /* BSSID (6 bytes) */
#define REG_MAR                     0x0620   /* Multicast address */

/* =========================================================================
 * Configuração do Canal / Frequência
 * ========================================================================= */

/* O canal é configurado via registradores do Baseband (acesso indireto)
 * e do RF. Os valores abaixo são para o clock do RF. */
#define REG_FPGA0_XA_RFINTERFACEOE  0x0860
#define REG_OFDM0_TRXPATHENA        0x0ED0
#define REG_OFDM0_TRSWISOLATION     0x0ED4

/* =========================================================================
 * Configuração EDCA (QoS)
 * ========================================================================= */

/* Filas de EDCA: BE (Best Effort), BK (Background), VI (Video), VO (Voice) */
#define REG_EDCA_VO_PARAM           0x0500
#define REG_EDCA_VI_PARAM           0x0504
#define REG_EDCA_BE_PARAM           0x0508
#define REG_EDCA_BK_PARAM           0x050C

/* =========================================================================
 * USB-específico
 * ========================================================================= */

/* USB_HIMR — Host Interrupt Mask Register */
#define REG_USB_HIMR                0xFE04
#define USB_HIMR_TIMEOUT2           BIT(31)
#define USB_HIMR_TIMEOUT1           BIT(30)
#define USB_HIMR_PSTDOK             BIT(29)
#define USB_HIMR_GTINT4             BIT(28)
#define USB_HIMR_GTINT3             BIT(27)
#define USB_HIMR_TXERR              BIT(26)
#define USB_HIMR_RXERR              BIT(25)
#define USB_HIMR_RDU                BIT(24)   /* RX Descriptor Unavailable */
#define USB_HIMR_PDNINT             BIT(23)
#define USB_HIMR_ROK                BIT(4)    /* RX OK */
#define USB_HIMR_VODOK              BIT(3)    /* VO TX OK */
#define USB_HIMR_VIDOK              BIT(2)    /* VI TX OK */
#define USB_HIMR_BEDOK              BIT(1)    /* BE TX OK */
#define USB_HIMR_BKDOK              BIT(0)    /* BK TX OK */

/* USB_SPECIAL_OPTION */
#define REG_USB_SPECIAL_OPTION      0xFE55

/* USB_AGG_TO — timeout de agregação USB */
#define REG_USB_AGG_TO              0xFE5B

/* USB_DMA_AGG_TO */
#define REG_USB_DMA_AGG_TO          0xFE5C

/* Controle de velocidade USB */
#define REG_USB_HRPWM               0xFE58

/* =========================================================================
 * Sequência de Power-on (ordem de escrita nos registradores)
 *
 * Resumo da sequência de inicialização (detalhada em hal.c):
 *
 *   1. Habilitar EFUSE (REG_EFUSE_ACCESS = EFUSE_ACCESS_ON)
 *   2. Ler MAC do EFUSE
 *   3. Desabilitar EFUSE (REG_EFUSE_ACCESS = EFUSE_ACCESS_OFF)
 *   4. Power on sequence: APS_FSMCO
 *   5. Habilitar funções: REG_SYS_FUNC_EN
 *   6. Inicializar MAC: REG_CR
 *   7. Configurar RX: REG_RCR
 *   8. Escrever MAC addr: REG_MACID
 *   9. Configurar EDCA
 *  10. Inicializar Baseband (RF)
 *  11. Inicializar RF (PLL, ganho)
 *  12. Habilitar TX/RX
 * ========================================================================= */

#endif /* _RTL8188FTV_REG_H_ */
