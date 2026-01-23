/**
 * LEGO Master - Implementación del Gestor Dinámico de Celdas
 */

#define _POSIX_C_SOURCE 199309L

#include "gestor_celdas.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Variable externa del sistema
extern SistemaLego *sistema;

// Verifica si una celda puede ser quitada de forma segura
bool celda_puede_quitarse(CeldaEmpaquetado *celda) {
    pthread_mutex_lock(&celda->mutex);
    
    if (celda->estado == CELDA_ESPERANDO_OP) {
        pthread_mutex_unlock(&celda->mutex);
        return false;
    }
    
    if (celda->trabajando_en_set) {
        pthread_mutex_unlock(&celda->mutex);
        return false;
    }
    
    if (celda->devolviendo_piezas) {
        pthread_mutex_unlock(&celda->mutex);
        return false;
    }
    
    pthread_mutex_unlock(&celda->mutex);
    
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        if (celda->caja.piezas_por_tipo[t] > 0) {
            pthread_mutex_unlock(&celda->caja.mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&celda->caja.mutex);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    if (celda->buffer_count > 0) {
        pthread_mutex_unlock(&celda->buffer_mutex);
        return false;
    }
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
        pthread_mutex_lock(&celda->brazos[b].mutex);
        if (celda->brazos[b].estado == BRAZO_RETIRANDO || 
            celda->brazos[b].estado == BRAZO_COLOCANDO) {
            pthread_mutex_unlock(&celda->brazos[b].mutex);
            return false;
        }
        pthread_mutex_unlock(&celda->brazos[b].mutex);
    }
    
    return true;
}

// Quita una celda del sistema (la desactiva)
bool quitar_celda_dinamica(int celda_id) {
    if (celda_id < 0 || celda_id >= sistema->config.num_celdas) {
        return false;
    }
    
    pthread_mutex_lock(&sistema->mutex_celdas_dinamicas);
    
    if (!sistema->celdas_habilitadas[celda_id]) {
        pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
        return false;
    }
    
    CeldaEmpaquetado *celda = &sistema->celdas[celda_id];
    
    if (!celda_puede_quitarse(celda)) {
        pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
        return false;
    }
    
    pthread_mutex_lock(&celda->mutex);
    celda->estado = CELDA_INACTIVA;
    pthread_mutex_unlock(&celda->mutex);
    
    sistema->celdas_habilitadas[celda_id] = false;
    sistema->num_celdas_activas--;
    
    printf("[GESTOR] Celda %d desactivada (activas: %d)\n", celda_id + 1, sistema->num_celdas_activas);
    
    pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
    return true;
}

// Agrega/reactiva una celda en el sistema
bool agregar_celda_dinamica(int celda_id) {
    if (celda_id < 0 || celda_id >= MAX_CELDAS) {
        return false;
    }
    
    pthread_mutex_lock(&sistema->mutex_celdas_dinamicas);
    
    if (sistema->celdas_habilitadas[celda_id]) {
        pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
        return false;
    }
    
    if (celda_id >= sistema->config.num_celdas) {
        pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
        return false;
    }
    
    CeldaEmpaquetado *celda = &sistema->celdas[celda_id];
    
    pthread_mutex_lock(&celda->mutex);
    celda->estado = CELDA_ACTIVA;
    celda->trabajando_en_set = false;
    celda->devolviendo_piezas = false;
    celda->ciclos_sin_progreso = 0;
    pthread_mutex_unlock(&celda->mutex);
    
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        celda->caja.piezas_por_tipo[t] = 0;
    }
    celda->caja.completa = false;
    pthread_mutex_unlock(&celda->caja.mutex);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    celda->buffer_count = 0;
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    sistema->celdas_habilitadas[celda_id] = true;
    sistema->num_celdas_activas++;
    sistema->ciclos_inactiva[celda_id] = 0;
    
    printf("[GESTOR] Celda %d activada en posición %d (activas: %d)\n", 
           celda_id + 1, celda->posicion_banda, sistema->num_celdas_activas);
    
    pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
    return true;
}

// Hilo gestor que monitorea y gestiona celdas dinámicamente
void* thread_gestor_celdas(void* arg) {
    (void)arg;
    
    int ultimo_tacho = 0;
    int ciclos_sin_cambios = 0;
    
    sleep(3);
    
    while (!sistema->terminar) {
        sleep(2);
        
        if (sistema->terminar) break;
        
        pthread_mutex_lock(&sistema->mutex_celdas_dinamicas);
        
        pthread_mutex_lock(&sistema->stats.mutex);
        int piezas_tacho_actual = sistema->stats.total_piezas_tacho;
        pthread_mutex_unlock(&sistema->stats.mutex);
        
        pthread_mutex_lock(&sistema->mutex_sets);
        int sets_completados = sistema->sets_completados_total;
        int sets_en_proceso = sistema->sets_en_proceso;
        int sets_pendientes = sistema->config.num_sets - sets_completados - sets_en_proceso;
        pthread_mutex_unlock(&sistema->mutex_sets);
        
        int piezas_tacho_recientes = piezas_tacho_actual - ultimo_tacho;
        ultimo_tacho = piezas_tacho_actual;
        
        int celdas_trabajando = 0;
        int celdas_ociosas = 0;
        
        for (int c = 0; c < sistema->config.num_celdas; c++) {
            if (!sistema->celdas_habilitadas[c]) continue;
            
            CeldaEmpaquetado *celda = &sistema->celdas[c];
            pthread_mutex_lock(&celda->mutex);
            bool trabajando = celda->trabajando_en_set;
            EstadoCelda estado = celda->estado;
            pthread_mutex_unlock(&celda->mutex);
            
            if (trabajando || estado == CELDA_ESPERANDO_OP) {
                celdas_trabajando++;
                sistema->ciclos_inactiva[c] = 0;
            } else {
                sistema->ciclos_inactiva[c]++;
                if (sistema->ciclos_inactiva[c] > 5) {
                    celdas_ociosas++;
                }
            }
        }
        
        pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
        
        // DECISIÓN: QUITAR CELDA
        if (celdas_ociosas > 0 && sets_pendientes <= sistema->num_celdas_activas / 2) {
            int celda_mas_ociosa = -1;
            int max_ciclos_ociosa = 0;
            
            for (int c = sistema->config.num_celdas - 1; c >= 0; c--) {
                if (sistema->celdas_habilitadas[c] && 
                    sistema->ciclos_inactiva[c] > max_ciclos_ociosa &&
                    sistema->num_celdas_activas > 1) {
                    celda_mas_ociosa = c;
                    max_ciclos_ociosa = sistema->ciclos_inactiva[c];
                }
            }
            
            if (celda_mas_ociosa >= 0 && max_ciclos_ociosa > 8) {
                quitar_celda_dinamica(celda_mas_ociosa);
                ciclos_sin_cambios = 0;
            }
        }
        
        // DECISIÓN: AGREGAR CELDA
        if (piezas_tacho_recientes > 2 && sets_pendientes > 0) {
            for (int c = 0; c < sistema->config.num_celdas; c++) {
                if (!sistema->celdas_habilitadas[c]) {
                    agregar_celda_dinamica(c);
                    ciclos_sin_cambios = 0;
                    break;
                }
            }
        }
        
        if (celdas_trabajando == sistema->num_celdas_activas && 
            sets_pendientes > sistema->num_celdas_activas) {
            for (int c = 0; c < sistema->config.num_celdas; c++) {
                if (!sistema->celdas_habilitadas[c]) {
                    agregar_celda_dinamica(c);
                    ciclos_sin_cambios = 0;
                    break;
                }
            }
        }
        
        ciclos_sin_cambios++;
    }
    
    return NULL;
}
