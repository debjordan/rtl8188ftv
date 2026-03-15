# =============================================================================
# Makefile — Driver RTL8188FTV
#
# O sistema de build do kernel Linux usa o "Kbuild". Para módulos externos
# (out-of-tree), o fluxo é em duas fases:
#
#   1. 'make' no diretório do driver invoca o Makefile do kernel passando
#      M=$(PWD) — o kernel saberá que é um módulo externo.
#   2. O Kbuild relê este mesmo Makefile, mas desta vez a variável
#      KERNELRELEASE estará definida, e ele usa a variável obj-m para saber
#      quais arquivos compilar.
#
# Referência: Documentation/kbuild/modules.rst
# =============================================================================

# Nome do módulo final (.ko)
MODULE_NAME := rtl8188ftv

# Lista de arquivos-fonte que compõem o módulo.
# A notação $(MODULE_NAME)-objs diz ao Kbuild que o módulo é formado
# por múltiplos arquivos objeto.
$(MODULE_NAME)-objs := src/main.o \
                       src/usb.o  \
                       src/hal.o  \
                       src/mac.o

# Registra o módulo no sistema Kbuild
obj-m := $(MODULE_NAME).o

# Diretório do kernel: usa o link simbólico padrão do sistema instalado
KDIR := /lib/modules/$(shell uname -r)/build

# Diretório corrente — usa $(src) dentro do Kbuild, $(shell pwd) fora dele.
# Quando o Kbuild re-invoca este Makefile, $(src) é o caminho absoluto do
# diretório do módulo. Usamos isso para montar o caminho do include.
PWD := $(shell pwd)

# Flags extras de compilação passadas ao gcc via Kbuild.
# $(src) é definido pelo Kbuild como o diretório absoluto do módulo.
# Fora do Kbuild (fase 1), usamos $(PWD).
ccflags-y := -I$(if $(src),$(src),$(PWD))/include -Wall -Wno-unused-function

# -----------------------------------------------------------------------
# Alvo padrão: compila o módulo
# -----------------------------------------------------------------------
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# -----------------------------------------------------------------------
# Limpa os artefatos de build
# -----------------------------------------------------------------------
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f src/*.o src/.*.cmd *.o .*.cmd

# -----------------------------------------------------------------------
# Instala o módulo no sistema (requer root)
# -----------------------------------------------------------------------
install: all
	sudo insmod $(MODULE_NAME).ko

# -----------------------------------------------------------------------
# Remove o módulo do kernel
# -----------------------------------------------------------------------
uninstall:
	sudo rmmod $(MODULE_NAME) 2>/dev/null || true

# -----------------------------------------------------------------------
# Recarrega: remove e insere novamente
# -----------------------------------------------------------------------
reload: uninstall install

.PHONY: all clean install uninstall reload
