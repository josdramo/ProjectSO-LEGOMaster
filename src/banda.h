/**
 * LEGO Master - Módulo de la Banda Transportadora
 * 
 * Contiene la lógica del hilo que mueve la banda y
 * las funciones auxiliares para manipular posiciones.
 */

#ifndef BANDA_H
#define BANDA_H

#include "common.h"

// Inicializa la banda transportadora
void inicializar_banda(BandaTransportadora *banda, int longitud, int velocidad);

// Destruye los recursos de la banda
void destruir_banda(BandaTransportadora *banda);

// Función del hilo de la banda
void* thread_banda(void* arg);

// Agregar pieza a una posición
int agregar_pieza_posicion(PosicionBanda *pos, Pieza pieza);

// Retirar pieza de una posición (retorna el índice o -1)
int retirar_pieza_posicion(PosicionBanda *pos, int tipo_buscado);

#endif // BANDA_H
