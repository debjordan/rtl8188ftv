// SPDX-License-Identifier: GPL-2.0-only
/*
 * main.c — Ponto de entrada do módulo do kernel RTL8188FTV
 *
 * Este arquivo é o ponto de entrada do módulo. Ele registra o driver USB
 * no kernel e define os metadados do módulo (autor, licença, descrição).
 *
 * Conceitos fundamentais de módulos do kernel Linux:
 * ===================================================
 *
 * 1. MÓDULO DO KERNEL vs. PROGRAMA USERSPACE
 *    Um módulo roda no "kernel space" (Ring 0), com acesso direto ao hardware.
 *    Não há libc, não há sistema de arquivos de forma direta — só APIs do kernel.
 *    Um bug aqui pode causar kernel panic (tela preta / reboot).
 *
 * 2. module_init / module_exit
 *    Ao rodar `insmod rtl8188ftv.ko`, o kernel chama a função registrada com
 *    module_init(). Ao rodar `rmmod rtl8188ftv`, chama module_exit().
 *    Diferente de programas normais, não há main() nem argv.
 *
 * 3. USB DRIVER REGISTRATION
 *    Registrar um driver USB = informar ao kernel: "se aparecer um dispositivo
 *    com este VID/PID, chame minha função probe()". O kernel monitora hotplug.
 *
 * 4. LICENÇA GPL
 *    Módulos GPL têm acesso a símbolos "EXPORT_SYMBOL_GPL" do kernel.
 *    Módulos proprietários só podem usar EXPORT_SYMBOL. Como drivers WiFi
 *    dependem de mac80211 (GPL), o módulo DEVE ser GPL.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>

#include "rtl8188ftv.h"

/* =========================================================================
 * Tabela de IDs USB
 *
 * Esta tabela é usada pelo kernel para fazer "matching" automático.
 * Quando um dispositivo USB é conectado, o kernel percorre as tabelas
 * de todos os drivers registrados buscando por VID/PID compatível.
 *
 * MODULE_DEVICE_TABLE exporta esta tabela para o arquivo /lib/modules/...
 * /modules.alias, que é usado pelo udev para carregar o módulo
 * automaticamente no hotplug.
 * ========================================================================= */
static const struct usb_device_id rtl8188ftv_id_table[] = {
    /*
     * RTL8188FTV — Realtek 0bda:f179
     * Encontrado em adaptadores USB WiFi de baixo custo (dongles pequenos).
     * Chip: RTL8188F, interface: USB 2.0 High-Speed
     */
    { USB_DEVICE(RTL8188FTV_VENDOR_ID, RTL8188FTV_PRODUCT_ID) },

    /*
     * Entrada sentinela (obrigatória): marca o fim da tabela.
     * O kernel itera até encontrar esta entrada zerada.
     */
    { }
};

/*
 * Exporta a tabela para o sistema de módulos (gera entrada em modules.alias).
 * Formato: "usb:vXXXXpXXXX" — o udev lê e chama `modprobe rtl8188ftv`
 * automaticamente quando o dispositivo é inserido.
 */
MODULE_DEVICE_TABLE(usb, rtl8188ftv_id_table);

/* =========================================================================
 * Estrutura usb_driver
 *
 * Esta é a estrutura principal que o subsistema USB do kernel espera.
 * Ela registra as funções de callback que serão chamadas nos eventos
 * de lifecycle do dispositivo.
 *
 * Callbacks principais:
 *   .probe      — chamado quando o dispositivo é conectado (USB "insert")
 *   .disconnect — chamado quando o dispositivo é desconectado
 *   .suspend    — chamado antes de suspender o sistema (power management)
 *   .resume     — chamado ao retornar da suspensão
 * ========================================================================= */
static struct usb_driver rtl8188ftv_usb_driver = {
    /*
     * .name: identificador único do driver no kernel.
     * Aparece em /sys/bus/usb/drivers/ e em `lsmod`.
     */
    .name       = "rtl8188ftv",

    /*
     * .probe: chamado pelo USB core quando um dispositivo com VID/PID
     * compatível é detectado. Aqui alocamos recursos, inicializamos o
     * hardware e registramos com o mac80211.
     * Implementado em: usb.c
     */
    .probe      = rtl8188ftv_usb_probe,

    /*
     * .disconnect: chamado quando o dispositivo é removido fisicamente
     * (unplug) ou quando o módulo é removido. Deve liberar todos os
     * recursos alocados no probe.
     * Implementado em: usb.c
     */
    .disconnect = rtl8188ftv_usb_disconnect,

    /* Tabela de IDs que este driver suporta */
    .id_table   = rtl8188ftv_id_table,

    /*
     * .soft_unbind: quando 1, o driver faz o unbind "suave" —
     * não desconecta o dispositivo USB fisicamente, só deregistra
     * o driver. Útil para rmmod sem remover o hardware.
     */
    .soft_unbind = 1,
};

/* =========================================================================
 * Inicialização do módulo
 *
 * module_usb_driver() é um macro que expande para:
 *
 *   static int __init rtl8188ftv_init(void) {
 *       return usb_register(&rtl8188ftv_usb_driver);
 *   }
 *   module_init(rtl8188ftv_init);
 *
 *   static void __exit rtl8188ftv_exit(void) {
 *       usb_deregister(&rtl8188ftv_usb_driver);
 *   }
 *   module_exit(rtl8188ftv_exit);
 *
 * usb_register() adiciona o driver ao USB core. O kernel começa a
 * monitorar dispositivos com os IDs da tabela. Se já houver um
 * dispositivo conectado, probe() é chamado imediatamente.
 *
 * usb_deregister() remove o driver. O kernel chama disconnect() para
 * cada dispositivo que estava usando este driver.
 * ========================================================================= */
module_usb_driver(rtl8188ftv_usb_driver);

/* =========================================================================
 * Metadados do módulo
 *
 * Estas macros gravam strings na seção .modinfo do arquivo .ko.
 * Podem ser consultadas com: modinfo rtl8188ftv.ko
 * ========================================================================= */

MODULE_LICENSE("GPL v2");
/*
 * GPL é obrigatório para acessar símbolos EXPORT_SYMBOL_GPL como
 * ieee80211_alloc_hw(), ieee80211_register_hw(), etc.
 */

MODULE_AUTHOR("Projeto Educacional RTL8188FTV");
MODULE_DESCRIPTION("Driver USB para Realtek RTL8188FTV 802.11b/g/n WiFi");
MODULE_VERSION("0.1.0");
