/**
 * LEGO Master - Módulo del Operador Humano
 * 
 * Contiene la lógica del operador que verifica las cajas
 * completadas y las marca como OK o FAIL.
 */

#ifndef OPERADOR_H
#define OPERADOR_H

#include "common.h"

// Notifica al operador humano que una caja está lista
void notificar_operador(CeldaEmpaquetado *celda);

// Inicia el hilo del operador
void iniciar_hilo_operador(void);

// Termina el hilo del operador
void terminar_hilo_operador(void);

#endif // OPERADOR_H
