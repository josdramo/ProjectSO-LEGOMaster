# Makefile para LEGO Master

CC = gcc
CFLAGS = -Wall -Wextra -pthread
LDFLAGS = -lpthread -lrt

SRC_DIR = src
BUILD_DIR = build
DEMOS_DIR = demos-4

# Archivos fuente
MAIN_SRC = $(SRC_DIR)/lego_master.c $(SRC_DIR)/utils.c
DISPENSERS_SRC = $(DEMOS_DIR)/dispensers.c
BANDA_SRC = $(DEMOS_DIR)/banda.c

# Ejecutables
MAIN_TARGET = $(BUILD_DIR)/lego_master
DISPENSERS_TARGET = $(BUILD_DIR)/dispensers
BANDA_TARGET = $(BUILD_DIR)/banda

.PHONY: all clean run demo help

all: $(BUILD_DIR) $(MAIN_TARGET) $(DISPENSERS_TARGET) $(BANDA_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(MAIN_TARGET): $(MAIN_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(DISPENSERS_TARGET): $(DISPENSERS_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BANDA_TARGET): $(BANDA_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Ejecutar simulación con parámetros por defecto
# Uso: make run DISP=4 CELDAS=2 SETS=3 PA=5 PB=3 PC=4 PD=2 VEL=2 LONG=20
run: $(MAIN_TARGET)
	./$(MAIN_TARGET) $(or $(DISP),4) $(or $(CELDAS),2) $(or $(SETS),3) \
		$(or $(PA),5) $(or $(PB),3) $(or $(PC),4) $(or $(PD),2) \
		$(or $(VEL),2) $(or $(LONG),20)

# Demo rápido
demo: $(MAIN_TARGET)
	@echo "Ejecutando demo con configuración básica..."
	./$(MAIN_TARGET) 3 2 2 3 2 2 1 2 15

# Demo de los archivos originales
demo-old: $(DISPENSERS_TARGET) $(BANDA_TARGET)
	@echo "Para usar los demos originales:"
	@echo "  Terminal 1: ./$(DISPENSERS_TARGET) 4 10 2 2 3 2 500000"
	@echo "  Terminal 2: ./$(BANDA_TARGET) 4 500000"

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(DEMOS_DIR)/dispensers $(DEMOS_DIR)/banda

help:
	@echo "LEGO Master - Sistema de Simulación"
	@echo ""
	@echo "Comandos disponibles:"
	@echo "  make all      - Compilar todo"
	@echo "  make run      - Ejecutar con parámetros por defecto"
	@echo "  make demo     - Ejecutar demo rápido"
	@echo "  make clean    - Limpiar archivos compilados"
	@echo ""
	@echo "Parámetros personalizados:"
	@echo "  make run DISP=4 CELDAS=2 SETS=3 PA=5 PB=3 PC=4 PD=2 VEL=2 LONG=20"
	@echo ""
	@echo "  DISP   - Número de dispensadores"
	@echo "  CELDAS - Número de celdas de empaquetado"
	@echo "  SETS   - Número de sets a completar"
	@echo "  PA-PD  - Piezas de cada tipo (A,B,C,D) por set"
	@echo "  VEL    - Velocidad de la banda (pasos/segundo)"
	@echo "  LONG   - Longitud de la banda (posiciones)"
