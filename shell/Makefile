# ====================================================================================
#  Makefile - Shell educativo EAFITOS + Editor de texto CLI (SO2026B)
# ====================================================================================
#  Targets:
#    make          -> compila el shell (eafitOS) y el editor autonomo (edi)
#    make eafitOS  -> solo el shell con el editor integrado
#    make edi      -> solo el binario autonomo del editor
#    make run      -> compila y ejecuta el shell
#    make huf      -> herramienta independiente del compresor Huffman
#    make pruebas  -> ejecuta el script de pruebas automatizadas
#    make pruebas_huf -> pruebas del compresor (ida y vuelta, cancelacion, TSan)
#    make clean    -> elimina objetos y binarios
# ====================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu11 -g -D_GNU_SOURCE -pthread

SHELL_BIN  = eafitOS
EDITOR_BIN = edi
HUF_BIN    = huf

# Modulos del editor, reutilizados por ambos binarios (biblioteca comun)
EDITOR_SRCS = editor_core.c editor_edit.c editor_meta.c editor_huf.c huf_core.c huf_job.c
EDITOR_OBJS = $(EDITOR_SRCS:.c=.o)

# Modulos del shell de clase + la nueva categoria 'edicion'
SHELL_SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c \
             cat_novatos.c cat_edicion.c
SHELL_OBJS = $(SHELL_SRCS:.c=.o)

all: $(SHELL_BIN) $(EDITOR_BIN) $(HUF_BIN)

$(SHELL_BIN): $(SHELL_OBJS) $(EDITOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(EDITOR_BIN): edi_main.o $(EDITOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Herramienta independiente del compresor (pruebas con md5sum, TSan, etc.)
$(HUF_BIN): huf_main.o huf_core.o huf_job.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c shell.h editor.h huf.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(SHELL_BIN)
	./$(SHELL_BIN)

pruebas: $(EDITOR_BIN)
	bash pruebas_editor.sh

pruebas_huf: $(HUF_BIN)
	bash pruebas_huf.sh

clean:
	rm -f $(SHELL_OBJS) $(EDITOR_OBJS) edi_main.o huf_main.o $(SHELL_BIN) $(EDITOR_BIN) $(HUF_BIN)

.PHONY: all run pruebas pruebas_huf clean
