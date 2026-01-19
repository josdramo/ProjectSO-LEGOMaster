/**
 * LEGO Master - Módulo de Celdas y Brazos Robóticos
 * 
 * Contiene la lógica de las celdas de empaquetado,
 * brazos robóticos, cajas y operador humano.
 */

#ifndef CELDA_H
#define CELDA_H

#include "common.h"

// Inicializa una celda de empaquetado
void inicializar_celda(CeldaEmpaquetado *celda, int id, int posicion, 
                       int piezas_por_tipo[MAX_TIPOS_PIEZA]);

// Destruye los recursos de una celda
void destruir_celda(CeldaEmpaquetado *celda);

// Verifica si la caja tiene todas las piezas necesarias
bool verificar_caja_completa(CajaEmpaquetado *caja);

// Verifica si se necesita una pieza de cierto tipo
bool necesita_pieza_tipo(CajaEmpaquetado *caja, int tipo);

// Notifica al operador humano y espera su respuesta
void notificar_operador(CeldaEmpaquetado *celda);

// Encuentra el brazo que ha movido más piezas
int encontrar_brazo_max_piezas(CeldaEmpaquetado *celda);

// Función del hilo de un brazo robótico
void* thread_brazo(void* arg);

// Estructura para pasar argumentos al hilo del brazo
typedef struct {
    int celda_id;
    int brazo_id;
} ArgsBrazo;

#endif // CELDA_H
