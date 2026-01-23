/**
 * LEGO Master - Módulo de Brazos Robóticos
 * 
 * Contiene la lógica de los brazos robóticos que retiran
 * piezas de la banda y las colocan en las cajas.
 */

#ifndef BRAZO_H
#define BRAZO_H

#include "common.h"

// Estructura para pasar argumentos al hilo del brazo
typedef struct {
    int celda_id;
    int brazo_id;
} ArgsBrazo;

// Función del hilo de un brazo robótico
void* thread_brazo(void* arg);

#endif // BRAZO_H
