# Makefile para LEGO Master

CC = gcc
CFLAGS = -Wall -Wextra -pthread -Iinclude
LDFLAGS = -lpthread -lrt

SRC = src
INC = include

SRCS = $(SRC)/lego_master.c $(SRC)/utils.c $(SRC)/banda.c $(SRC)/dispensador.c $(SRC)/celda.c $(SRC)/brazo.c $(SRC)/operador.c $(SRC)/gestor_celdas.c
TARGET = build/lego_master

.PHONY: all clean

all: build $(TARGET)

build:
	mkdir -p build

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -rf build
