/**
 * LEGO Master - Módulo de Dispensadores
 * 
 * Genera piezas aleatorias y las coloca en el inicio de la banda.
 */

#ifndef DISPENSADOR_H
#define DISPENSADOR_H

#include "common.h"

// Genera un ID único para cada pieza
int generar_id_pieza(void);

// Función del hilo de dispensadores
void* thread_dispensador(void* arg);

#endif // DISPENSADOR_H
