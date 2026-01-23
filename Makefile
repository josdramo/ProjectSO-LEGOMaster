# Makefile para LEGO Master
# Proyecto de Sistemas Operativos

CC = gcc
CFLAGS = -Wall -Wextra -pthread -Iinclude -D_DEFAULT_SOURCE
LDFLAGS = -lpthread -lrt

SRC = src
INC = include

SRCS = $(SRC)/lego_master.c $(SRC)/utils.c $(SRC)/banda.c $(SRC)/dispensador.c $(SRC)/celda.c $(SRC)/brazo.c $(SRC)/operador.c $(SRC)/gestor_celdas.c
TARGET = build/lego_master

.PHONY: all clean run demo help

all: build $(TARGET)
	@echo ""
	@echo "✓ Compilación exitosa"
	@echo "  Ejecutable: $(TARGET)"
	@echo ""
	@echo "Para ejecutar: ./$(TARGET) <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>"
	@echo "O use: make demo"
	@echo ""

build:
	mkdir -p build

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Ejecutar demo rápido
demo: $(TARGET)
	@echo "Ejecutando demo: 2 celdas, 3 sets, piezas 2-2-1-1, vel 2, long 20"
	./$(TARGET) 2 3 2 2 1 1 1 40

# Ejecutar con parámetros personalizados
run: $(TARGET)
	./$(TARGET) $(CELDAS) $(SETS) $(PA) $(PB) $(PC) $(PD) $(VEL) $(LONG)

clean:
	rm -rf build

help:
	@echo "LEGO Master - Comandos disponibles:"
	@echo ""
	@echo "  make          - Compilar el proyecto"
	@echo "  make demo     - Ejecutar demo rápido"
	@echo "  make clean    - Limpiar archivos compilados"
	@echo "  make help     - Mostrar esta ayuda"
	@echo ""
	@echo "Ejecución manual:"
	@echo "  ./build/lego_master <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>"
	@echo ""
	@echo "Ejemplo:"
	@echo "  ./build/lego_master 2 3 2 2 1 1 2 20"

