/**
 * LEGO Master - Implementación de Brazos Robóticos
 */

#define _POSIX_C_SOURCE 200809L

#include "brazo.h"
#include "celda.h"
#include "operador.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

// Variable externa del sistema
extern SistemaLego *sistema;

// Contar piezas de un tipo específico en el buffer (uso interno)
static int contar_tipo_en_buffer(CeldaEmpaquetado *celda, int tipo) {
    int count = 0;
    for (int i = 0; i < celda->buffer_count; i++) {
        if (celda->buffer[i].tipo == tipo) {
            count++;
        }
    }
    return count;
}

// Verificar si la celda necesita más piezas de este tipo
static bool necesita_pieza_tipo_total(CeldaEmpaquetado *celda, int tipo) {
    if (tipo < 1 || tipo > MAX_TIPOS_PIEZA) return false;
    
    pthread_mutex_lock(&celda->caja.mutex);
    if (celda->caja.completa) {
        pthread_mutex_unlock(&celda->caja.mutex);
        return false;
    }
    int en_caja = celda->caja.piezas_por_tipo[tipo - 1];
    int necesarias = celda->caja.piezas_necesarias[tipo - 1];
    pthread_mutex_unlock(&celda->caja.mutex);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    int en_buffer = contar_tipo_en_buffer(celda, tipo);
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    return (en_caja + en_buffer) < necesarias;
}

// Agregar pieza al buffer de la celda
static bool agregar_a_buffer(CeldaEmpaquetado *celda, Pieza pieza) {
    pthread_mutex_lock(&celda->buffer_mutex);
    if (celda->buffer_count >= MAX_BUFFER_CELDA) {
        pthread_mutex_unlock(&celda->buffer_mutex);
        return false;
    }
    celda->buffer[celda->buffer_count++] = pieza;
    pthread_mutex_unlock(&celda->buffer_mutex);
    return true;
}

// Sacar pieza del buffer
static Pieza sacar_del_buffer(CeldaEmpaquetado *celda, int tipo_necesario) {
    Pieza resultado = {0, 0};
    pthread_mutex_lock(&celda->buffer_mutex);
    
    for (int i = 0; i < celda->buffer_count; i++) {
        if (tipo_necesario == -1 || celda->buffer[i].tipo == tipo_necesario) {
            resultado = celda->buffer[i];
            for (int j = i; j < celda->buffer_count - 1; j++) {
                celda->buffer[j] = celda->buffer[j + 1];
            }
            celda->buffer_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&celda->buffer_mutex);
    return resultado;
}

// Verificar si hay pieza en buffer que necesitemos
static bool hay_pieza_en_buffer(CeldaEmpaquetado *celda, CajaEmpaquetado *caja) {
    bool encontrada = false;
    pthread_mutex_lock(&celda->buffer_mutex);
    pthread_mutex_lock(&caja->mutex);
    
    for (int i = 0; i < celda->buffer_count && !encontrada; i++) {
        int tipo = celda->buffer[i].tipo;
        if (tipo > 0 && tipo <= MAX_TIPOS_PIEZA) {
            if (caja->piezas_por_tipo[tipo - 1] < caja->piezas_necesarias[tipo - 1]) {
                encontrada = true;
            }
        }
    }
    
    pthread_mutex_unlock(&caja->mutex);
    pthread_mutex_unlock(&celda->buffer_mutex);
    return encontrada;
}

void* thread_brazo(void* arg) {
    ArgsBrazo *args = (ArgsBrazo*)arg;
    int c = args->celda_id;
    int b = args->brazo_id;
    free(args);
    
    CeldaEmpaquetado *celda = &sistema->celdas[c];
    BrazoRobotico *brazo = &celda->brazos[b];
    
    while (!sistema->terminar) {
        // Verificar si la celda está habilitada
        pthread_mutex_lock(&sistema->mutex_celdas_dinamicas);
        bool celda_activa = sistema->celdas_habilitadas[c];
        pthread_mutex_unlock(&sistema->mutex_celdas_dinamicas);
        
        if (!celda_activa) {
            usleep(100000);
            continue;
        }
        
        // Verificar si está suspendido
        pthread_mutex_lock(&brazo->mutex);
        if (brazo->estado == BRAZO_SUSPENDIDO) {
            if (time(NULL) - brazo->tiempo_suspension >= sistema->config.delta_t2 / 1000) {
                brazo->estado = BRAZO_IDLE;
            } else {
                pthread_mutex_unlock(&brazo->mutex);
                usleep(100000);
                continue;
            }
        }
        pthread_mutex_unlock(&brazo->mutex);
        
        // Verificar estado de la celda
        pthread_mutex_lock(&celda->mutex);
        EstadoCelda estado_celda = celda->estado;
        bool devolviendo = celda->devolviendo_piezas;
        pthread_mutex_unlock(&celda->mutex);
        
        if (estado_celda == CELDA_INACTIVA) {
            usleep(100000);
            continue;
        }
        
        if (devolviendo) {
            usleep(50000);
            continue;
        }
        
        if (estado_celda == CELDA_ESPERANDO_OP) {
            usleep(50000);
            continue;
        }
        
        // Sistema de asignación de SETs
        pthread_mutex_lock(&sistema->mutex_sets);
        int sets_completados = sistema->sets_completados_total;
        int sets_necesarios = sistema->config.num_sets;
        pthread_mutex_unlock(&sistema->mutex_sets);
        
        if (sets_completados >= sets_necesarios) {
            usleep(50000);
            continue;
        }
        
        pthread_mutex_lock(&celda->mutex);
        bool ya_trabajando = celda->trabajando_en_set;
        pthread_mutex_unlock(&celda->mutex);
        
        // FASE 1: RETIRAR PIEZA DE LA BANDA
        pthread_mutex_lock(&celda->buffer_mutex);
        int buffer_actual = celda->buffer_count;
        pthread_mutex_unlock(&celda->buffer_mutex);
        
        if (buffer_actual < MAX_BUFFER_CELDA - 2) {
            if (sem_trywait(&celda->sem_brazos_retirando) == 0) {
                PosicionBanda *pos = &sistema->banda.posiciones[celda->posicion_banda];
                pthread_mutex_lock(&pos->mutex);
                
                int pieza_encontrada = -1;
                for (int p = 0; p < pos->num_piezas; p++) {
                    int tipo = pos->piezas[p].tipo;
                    if (tipo > 0 && necesita_pieza_tipo_total(celda, tipo)) {
                        pieza_encontrada = p;
                        break;
                    }
                }
                
                if (pieza_encontrada >= 0) {
                    if (!ya_trabajando) {
                        pthread_mutex_lock(&sistema->mutex_sets);
                        pthread_mutex_lock(&celda->mutex);
                        
                        if (!celda->trabajando_en_set && 
                            sistema->sets_completados_total + sistema->sets_en_proceso < sistema->config.num_sets) {
                            celda->trabajando_en_set = true;
                            sistema->sets_en_proceso++;
                            ya_trabajando = true;
                            printf("[CELDA %d] Inició SET #%d\n", 
                                   c+1, sistema->sets_completados_total + sistema->sets_en_proceso);
                        }
                        
                        pthread_mutex_unlock(&celda->mutex);
                        pthread_mutex_unlock(&sistema->mutex_sets);
                    }
                    
                    if (ya_trabajando) {
                        Pieza pieza_tomada = pos->piezas[pieza_encontrada];
                        
                        for (int p = pieza_encontrada; p < pos->num_piezas - 1; p++) {
                            pos->piezas[p] = pos->piezas[p + 1];
                        }
                        pos->num_piezas--;
                        pthread_mutex_unlock(&pos->mutex);
                        sem_post(&celda->sem_brazos_retirando);
                        
                        pthread_mutex_lock(&brazo->mutex);
                        brazo->estado = BRAZO_RETIRANDO;
                        brazo->pieza_actual = pieza_tomada;
                        pthread_mutex_unlock(&brazo->mutex);
                        
                        usleep(30000);
                        
                        // FASE 2: COLOCAR EN LA CAJA
                        sem_wait(&celda->caja.sem_acceso);
                        
                        pthread_mutex_lock(&brazo->mutex);
                        brazo->estado = BRAZO_COLOCANDO;
                        pthread_mutex_unlock(&brazo->mutex);
                        
                        pthread_mutex_lock(&celda->caja.mutex);
                        
                        int tipo = brazo->pieza_actual.tipo;
                        
                        if (tipo > 0 && tipo <= MAX_TIPOS_PIEZA && 
                            !celda->caja.completa &&
                            celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                            
                            celda->caja.piezas_por_tipo[tipo - 1]++;
                            brazo->piezas_movidas++;
                            
                            pthread_mutex_lock(&celda->mutex);
                            celda->ciclos_sin_progreso = 0;
                            pthread_mutex_unlock(&celda->mutex);
                            
                            pthread_mutex_lock(&sistema->stats.mutex);
                            sistema->stats.piezas_por_brazo[c][b]++;
                            pthread_mutex_unlock(&sistema->stats.mutex);
                            
                            printf("[CELDA %d][BRAZO %d] Colocó pieza tipo %s [%d/%d]\n",
                                   c+1, b+1, nombre_tipo_pieza(tipo),
                                   celda->caja.piezas_por_tipo[tipo - 1],
                                   celda->caja.piezas_necesarias[tipo - 1]);
                            
                            if (verificar_caja_completa(&celda->caja)) {
                                celda->caja.completa = true;
                                printf("[CELDA %d] ★ SET COMPLETO - Esperando revisión\n", c+1);
                                
                                pthread_mutex_unlock(&celda->caja.mutex);
                                sem_post(&celda->caja.sem_acceso);
                                
                                pthread_mutex_lock(&celda->mutex);
                                celda->estado = CELDA_ESPERANDO_OP;
                                pthread_mutex_unlock(&celda->mutex);
                                
                                notificar_operador(celda);
                                
                                pthread_mutex_lock(&brazo->mutex);
                                brazo->estado = BRAZO_IDLE;
                                brazo->pieza_actual.tipo = 0;
                                pthread_mutex_unlock(&brazo->mutex);
                                
                                continue;
                            }
                        } else if (tipo > 0) {
                            agregar_a_buffer(celda, brazo->pieza_actual);
                        }
                        
                        pthread_mutex_unlock(&celda->caja.mutex);
                        sem_post(&celda->caja.sem_acceso);
                        
                        pthread_mutex_lock(&brazo->mutex);
                        brazo->estado = BRAZO_IDLE;
                        brazo->pieza_actual.tipo = 0;
                        pthread_mutex_unlock(&brazo->mutex);
                        
                    } else {
                        pthread_mutex_unlock(&pos->mutex);
                        sem_post(&celda->sem_brazos_retirando);
                    }
                } else {
                    pthread_mutex_unlock(&pos->mutex);
                    sem_post(&celda->sem_brazos_retirando);
                }
            }
        }
        
        // FASE 3: USAR PIEZAS DEL BUFFER
        if (ya_trabajando && estado_celda == CELDA_ACTIVA && hay_pieza_en_buffer(celda, &celda->caja)) {
            sem_wait(&celda->caja.sem_acceso);
            pthread_mutex_lock(&celda->caja.mutex);
            
            for (int tipo = 1; tipo <= MAX_TIPOS_PIEZA; tipo++) {
                if (celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                    Pieza p = sacar_del_buffer(celda, tipo);
                    if (p.tipo > 0) {
                        celda->caja.piezas_por_tipo[tipo - 1]++;
                        brazo->piezas_movidas++;
                        
                        pthread_mutex_lock(&celda->mutex);
                        celda->ciclos_sin_progreso = 0;
                        pthread_mutex_unlock(&celda->mutex);
                        
                        pthread_mutex_lock(&sistema->stats.mutex);
                        sistema->stats.piezas_por_brazo[c][b]++;
                        pthread_mutex_unlock(&sistema->stats.mutex);
                        
                        printf("[CELDA %d][BRAZO %d] Del buffer: pieza tipo %s [%d/%d]\n",
                               c+1, b+1, nombre_tipo_pieza(tipo),
                               celda->caja.piezas_por_tipo[tipo - 1],
                               celda->caja.piezas_necesarias[tipo - 1]);
                        
                        if (verificar_caja_completa(&celda->caja)) {
                            celda->caja.completa = true;
                            printf("[CELDA %d] ★ SET COMPLETO - Esperando revisión\n", c+1);
                            
                            pthread_mutex_unlock(&celda->caja.mutex);
                            sem_post(&celda->caja.sem_acceso);
                            
                            pthread_mutex_lock(&celda->mutex);
                            celda->estado = CELDA_ESPERANDO_OP;
                            pthread_mutex_unlock(&celda->mutex);
                            
                            notificar_operador(celda);
                            continue;
                        }
                        break;
                    }
                }
            }
            
            pthread_mutex_unlock(&celda->caja.mutex);
            sem_post(&celda->caja.sem_acceso);
        }
        
        // FASE 4: VERIFICAR SI DEBEMOS LIBERAR PIEZAS
        if (b == 0 && ya_trabajando && estado_celda == CELDA_ACTIVA) {
            pthread_mutex_lock(&celda->mutex);
            celda->ciclos_sin_progreso++;
            int ciclos = celda->ciclos_sin_progreso;
            pthread_mutex_unlock(&celda->mutex);
            
            if (ciclos > 200) {
                int piezas_faltan_por_tipo[MAX_TIPOS_PIEZA] = {0};
                int piezas_faltan_total = 0;
                
                pthread_mutex_lock(&celda->caja.mutex);
                for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                    int faltan = celda->caja.piezas_necesarias[t] - celda->caja.piezas_por_tipo[t];
                    if (faltan > 0) {
                        piezas_faltan_por_tipo[t] = faltan;
                        piezas_faltan_total += faltan;
                    }
                }
                pthread_mutex_unlock(&celda->caja.mutex);
                
                if (piezas_faltan_total == 0) {
                    pthread_mutex_lock(&celda->mutex);
                    celda->ciclos_sin_progreso = 0;
                    pthread_mutex_unlock(&celda->mutex);
                    usleep(10000);
                    continue;
                }
                
                int piezas_disponibles_por_tipo[MAX_TIPOS_PIEZA] = {0};
                
                pthread_mutex_lock(&celda->buffer_mutex);
                for (int i = 0; i < celda->buffer_count; i++) {
                    int tipo = celda->buffer[i].tipo;
                    if (tipo >= 1 && tipo <= MAX_TIPOS_PIEZA) {
                        piezas_disponibles_por_tipo[tipo - 1]++;
                    }
                }
                pthread_mutex_unlock(&celda->buffer_mutex);
                
                for (int i = 0; i <= celda->posicion_banda; i++) {
                    PosicionBanda *pos = &sistema->banda.posiciones[i];
                    pthread_mutex_lock(&pos->mutex);
                    for (int p = 0; p < pos->num_piezas; p++) {
                        int tipo = pos->piezas[p].tipo;
                        if (tipo >= 1 && tipo <= MAX_TIPOS_PIEZA) {
                            piezas_disponibles_por_tipo[tipo - 1]++;
                        }
                    }
                    pthread_mutex_unlock(&pos->mutex);
                }
                
                bool puedo_completar = true;
                for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                    if (piezas_faltan_por_tipo[t] > piezas_disponibles_por_tipo[t]) {
                        puedo_completar = false;
                        break;
                    }
                }
                
                bool es_ultima_celda = (c == sistema->config.num_celdas - 1);
                
                bool banda_vacia = true;
                for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                    if (piezas_disponibles_por_tipo[t] > 0) {
                        banda_vacia = false;
                        break;
                    }
                }
                
                bool debo_liberar = !puedo_completar && (!es_ultima_celda || banda_vacia);
                
                if (debo_liberar) {
                    devolver_piezas_a_banda(celda);
                    ya_trabajando = false;
                } else {
                    pthread_mutex_lock(&celda->mutex);
                    celda->ciclos_sin_progreso = 0;
                    pthread_mutex_unlock(&celda->mutex);
                }
            }
        }
        
        usleep(10000);
    }
    
    return NULL;
}
