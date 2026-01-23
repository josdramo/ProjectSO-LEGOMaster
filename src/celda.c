/**
 * LEGO Master - Implementación de Celdas de Empaquetado
 */

#define _POSIX_C_SOURCE 199309L

#include "celda.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// Variable externa del sistema
extern SistemaLego *sistema;

void inicializar_celda(CeldaEmpaquetado *celda, int id, int posicion,
                       int piezas_por_tipo[MAX_TIPOS_PIEZA]) {
    celda->id = id;
    celda->posicion_banda = posicion;
    celda->estado = CELDA_ACTIVA;
    celda->cajas_completadas_ok = 0;
    celda->cajas_completadas_fail = 0;
    celda->trabajando_en_set = false;
    celda->devolviendo_piezas = false;
    pthread_mutex_init(&celda->mutex, NULL);
    
    // Semáforo para limitar brazos retirando (máx 2)
    sem_init(&celda->sem_brazos_retirando, 0, MAX_BRAZOS_ACTIVOS);
    
    // Inicializar caja
    pthread_mutex_init(&celda->caja.mutex, NULL);
    sem_init(&celda->caja.sem_acceso, 0, 1);  // solo 1 coloca a la vez
    celda->caja.completa = false;
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        celda->caja.piezas_por_tipo[t] = 0;
        celda->caja.piezas_necesarias[t] = piezas_por_tipo[t];
    }
    
    // Inicializar buffer de piezas
    celda->buffer_count = 0;
    pthread_mutex_init(&celda->buffer_mutex, NULL);
    
    // Inicializar control de progreso
    celda->ultimo_progreso = time(NULL);
    celda->ciclos_sin_progreso = 0;
    
    // Inicializar brazos
    for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
        celda->brazos[b].id = b;
        celda->brazos[b].celda_id = id;
        celda->brazos[b].estado = BRAZO_IDLE;
        celda->brazos[b].piezas_movidas = 0;
        celda->brazos[b].pieza_actual.tipo = 0;
        pthread_mutex_init(&celda->brazos[b].mutex, NULL);
    }
}

void destruir_celda(CeldaEmpaquetado *celda) {
    pthread_mutex_destroy(&celda->mutex);
    pthread_mutex_destroy(&celda->caja.mutex);
    pthread_mutex_destroy(&celda->buffer_mutex);
    sem_destroy(&celda->caja.sem_acceso);
    sem_destroy(&celda->sem_brazos_retirando);
    
    for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
        pthread_mutex_destroy(&celda->brazos[b].mutex);
    }
}

bool verificar_caja_completa(CajaEmpaquetado *caja) {
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        if (caja->piezas_por_tipo[t] != caja->piezas_necesarias[t]) {
            return false;
        }
    }
    return true;
}

bool necesita_pieza_tipo(CajaEmpaquetado *caja, int tipo) {
    if (tipo < 1 || tipo > MAX_TIPOS_PIEZA) return false;
    if (caja->completa) return false;
    return caja->piezas_por_tipo[tipo - 1] < caja->piezas_necesarias[tipo - 1];
}

// Verifica si la celda está estancada (tiene piezas pero no puede completar el SET)
bool celda_estancada(CeldaEmpaquetado *celda) {
    // Si no está trabajando en un SET, no está estancada
    pthread_mutex_lock(&celda->mutex);
    bool trabajando = celda->trabajando_en_set;
    int ciclos = celda->ciclos_sin_progreso;
    pthread_mutex_unlock(&celda->mutex);
    
    if (!trabajando) return false;
    
    // Consideramos estancada si han pasado muchos ciclos sin progreso
    return ciclos > 30;
}

int encontrar_brazo_max_piezas(CeldaEmpaquetado *celda) {
    int max_piezas = -1;
    int brazo_max = -1;
    
    for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
        pthread_mutex_lock(&celda->brazos[b].mutex);
        if (celda->brazos[b].estado != BRAZO_SUSPENDIDO &&
            celda->brazos[b].piezas_movidas > max_piezas) {
            max_piezas = celda->brazos[b].piezas_movidas;
            brazo_max = b;
        }
        pthread_mutex_unlock(&celda->brazos[b].mutex);
    }
    
    return brazo_max;
}

// Verifica si alguna OTRA celda necesita las piezas que esta celda tiene
bool otra_celda_necesita_piezas(CeldaEmpaquetado *celda) {
    int mi_id = celda->id;
    
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        if (c == mi_id) continue;
        
        CeldaEmpaquetado *otra = &sistema->celdas[c];
        
        pthread_mutex_lock(&otra->mutex);
        bool otra_trabajando = otra->trabajando_en_set;
        EstadoCelda otra_estado = otra->estado;
        pthread_mutex_unlock(&otra->mutex);
        
        if (otra_estado != CELDA_ACTIVA || !otra_trabajando) continue;
        
        pthread_mutex_lock(&celda->caja.mutex);
        pthread_mutex_lock(&otra->caja.mutex);
        
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            if (celda->caja.piezas_por_tipo[t] > 0 &&
                otra->caja.piezas_por_tipo[t] < otra->caja.piezas_necesarias[t]) {
                pthread_mutex_unlock(&otra->caja.mutex);
                pthread_mutex_unlock(&celda->caja.mutex);
                return true;
            }
        }
        
        pthread_mutex_unlock(&otra->caja.mutex);
        pthread_mutex_unlock(&celda->caja.mutex);
        
        pthread_mutex_lock(&celda->buffer_mutex);
        pthread_mutex_lock(&otra->caja.mutex);
        
        for (int i = 0; i < celda->buffer_count; i++) {
            int tipo = celda->buffer[i].tipo;
            if (tipo > 0 && tipo <= MAX_TIPOS_PIEZA &&
                otra->caja.piezas_por_tipo[tipo - 1] < otra->caja.piezas_necesarias[tipo - 1]) {
                pthread_mutex_unlock(&otra->caja.mutex);
                pthread_mutex_unlock(&celda->buffer_mutex);
                return true;
            }
        }
        
        pthread_mutex_unlock(&otra->caja.mutex);
        pthread_mutex_unlock(&celda->buffer_mutex);
    }
    
    return false;
}

// Devuelve las piezas de la caja y buffer a la banda
void devolver_piezas_a_banda(CeldaEmpaquetado *celda) {
    pthread_mutex_lock(&celda->mutex);
    celda->devolviendo_piezas = true;
    pthread_mutex_unlock(&celda->mutex);
    
    int posicion_devolucion = celda->posicion_banda + 1;
    
    bool es_ultima_celda = (celda->id == sistema->config.num_celdas - 1);
    if (es_ultima_celda) {
        posicion_devolucion = sistema->banda.longitud - 1;
    }
    
    if (posicion_devolucion >= sistema->banda.longitud) {
        posicion_devolucion = sistema->banda.longitud - 1;
    }
    
    PosicionBanda *pos = &sistema->banda.posiciones[posicion_devolucion];
    
    int total_devolver = 0;
    
    sem_wait(&celda->caja.sem_acceso);
    pthread_mutex_lock(&celda->caja.mutex);
    
    int limite_piezas = sistema->config.num_dispensadores;
    
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        while (celda->caja.piezas_por_tipo[t] > 0) {
            pthread_mutex_lock(&pos->mutex);
            if (pos->num_piezas < limite_piezas) {
                Pieza p;
                p.tipo = t + 1;
                p.id_unico = -1;
                pos->piezas[pos->num_piezas++] = p;
                celda->caja.piezas_por_tipo[t]--;
                total_devolver++;
            }
            pthread_mutex_unlock(&pos->mutex);
            
            if (celda->caja.piezas_por_tipo[t] > 0) {
                usleep(50000);
            }
        }
    }
    
    celda->caja.completa = false;
    pthread_mutex_unlock(&celda->caja.mutex);
    sem_post(&celda->caja.sem_acceso);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    while (celda->buffer_count > 0) {
        Pieza p = celda->buffer[--celda->buffer_count];
        pthread_mutex_lock(&pos->mutex);
        if (pos->num_piezas < limite_piezas) {
            pos->piezas[pos->num_piezas++] = p;
            total_devolver++;
        }
        pthread_mutex_unlock(&pos->mutex);
    }
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    pthread_mutex_lock(&celda->mutex);
    celda->trabajando_en_set = false;
    celda->ciclos_sin_progreso = 0;
    celda->devolviendo_piezas = false;
    pthread_mutex_unlock(&celda->mutex);
    
    pthread_mutex_lock(&sistema->mutex_sets);
    if (sistema->sets_en_proceso > 0) {
        sistema->sets_en_proceso--;
    }
    pthread_mutex_unlock(&sistema->mutex_sets);
    
    printf("[CELDA %d] Devolvió %d piezas a la banda (pos %d)\n", 
           celda->id + 1, total_devolver, posicion_devolucion);
}
