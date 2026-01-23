/**
 * LEGO Master - Módulo de Celdas de Empaquetado
 * 
 * Contiene la lógica de las celdas de empaquetado y cajas.
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

// Devuelve las piezas de la caja/buffer a la banda para que otra celda las use
void devolver_piezas_a_banda(CeldaEmpaquetado *celda);

// Verifica si alguna celda posterior necesita piezas que esta celda tiene
bool otra_celda_necesita_piezas(CeldaEmpaquetado *celda);

// Verifica si la celda está estancada y debería devolver piezas
bool celda_estancada(CeldaEmpaquetado *celda);

// Encuentra el brazo que ha movido más piezas
int encontrar_brazo_max_piezas(CeldaEmpaquetado *celda);

#endif // CELDA_H
