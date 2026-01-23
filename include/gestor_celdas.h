/**
 * LEGO Master - Módulo de Gestión Dinámica de Celdas
 * 
 * Contiene la lógica para activar/desactivar celdas
 * dinámicamente según la carga del sistema.
 */

#ifndef GESTOR_CELDAS_H
#define GESTOR_CELDAS_H

#include "common.h"

// Verifica si una celda puede ser quitada de forma segura
bool celda_puede_quitarse(CeldaEmpaquetado *celda);

// Quita una celda del sistema (la desactiva)
bool quitar_celda_dinamica(int celda_id);

// Agrega/reactiva una celda en el sistema
bool agregar_celda_dinamica(int celda_id);

// Hilo gestor que monitorea y gestiona celdas dinámicamente
void* thread_gestor_celdas(void* arg);

#endif // GESTOR_CELDAS_H
