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

// Inicia el hilo del operador que maneja entrada de forma no bloqueante
void iniciar_hilo_operador(void);

// Termina el hilo del operador
void terminar_hilo_operador(void);

// Encuentra el brazo que ha movido más piezas
int encontrar_brazo_max_piezas(CeldaEmpaquetado *celda);

// Verifica si la celda está estancada y debería devolver piezas
bool celda_estancada(CeldaEmpaquetado *celda);

// Devuelve las piezas de la caja/buffer a la banda para que otra celda las use
void devolver_piezas_a_banda(CeldaEmpaquetado *celda);

// Verifica si alguna celda posterior necesita piezas que esta celda tiene
bool otra_celda_necesita_piezas(CeldaEmpaquetado *celda);

// Función del hilo de un brazo robótico
void* thread_brazo(void* arg);

// Estructura para pasar argumentos al hilo del brazo
typedef struct {
    int celda_id;
    int brazo_id;
} ArgsBrazo;

// ============= GESTIÓN DINÁMICA DE CELDAS =============

// Verifica si una celda puede ser quitada de forma segura
bool celda_puede_quitarse(CeldaEmpaquetado *celda);

// Quita una celda del sistema (la desactiva)
bool quitar_celda_dinamica(int celda_id);

// Agrega/reactiva una celda en el sistema
bool agregar_celda_dinamica(int celda_id);

// Hilo gestor que monitorea y gestiona celdas dinámicamente
void* thread_gestor_celdas(void* arg);

#endif // CELDA_H
