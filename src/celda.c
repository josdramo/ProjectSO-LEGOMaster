/**
 * LEGO Master - Implementación de Celdas y Brazos Robóticos
 */

#include "celda.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
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

void notificar_operador(CeldaEmpaquetado *celda) {
    // Simula tiempo de espera del operador (0 a delta_t1 segundos)
    int delay = rand() % (sistema->config.delta_t1_max + 1);
    usleep(delay * 1000);
    
    // Verificar si la caja está correctamente llena
    bool ok = verificar_caja_completa(&celda->caja);
    
    pthread_mutex_lock(&sistema->stats.mutex);
    if (ok) {
        sistema->stats.cajas_ok++;
        celda->cajas_completadas_ok++;
        printf("[CELDA %d] ✓ Operador: OK - Caja #%d completada correctamente\n", 
               celda->id + 1, sistema->stats.cajas_ok);
    } else {
        sistema->stats.cajas_fail++;
        celda->cajas_completadas_fail++;
        printf("[CELDA %d] ✗ Operador: FAIL - Caja incorrecta (", celda->id + 1);
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            printf("%s:%d/%d ", nombre_tipo_pieza(t+1),
                   celda->caja.piezas_por_tipo[t],
                   celda->caja.piezas_necesarias[t]);
        }
        printf(")\n");
    }
    pthread_mutex_unlock(&sistema->stats.mutex);
    
    // Reiniciar la caja
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        celda->caja.piezas_por_tipo[t] = 0;
    }
    celda->caja.completa = false;
    pthread_mutex_unlock(&celda->caja.mutex);
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

void* thread_brazo(void* arg) {
    ArgsBrazo *args = (ArgsBrazo*)arg;
    int c = args->celda_id;
    int b = args->brazo_id;
    free(args);
    
    CeldaEmpaquetado *celda = &sistema->celdas[c];
    BrazoRobotico *brazo = &celda->brazos[b];
    
    printf("[CELDA %d][BRAZO %d] Iniciado\n", c+1, b+1);
    
    while (!sistema->terminar) {
        // Verificar si está suspendido
        pthread_mutex_lock(&brazo->mutex);
        if (brazo->estado == BRAZO_SUSPENDIDO) {
            if (time(NULL) - brazo->tiempo_suspension >= sistema->config.delta_t2 / 1000) {
                brazo->estado = BRAZO_IDLE;
                printf("[CELDA %d][BRAZO %d] Reactivado\n", c+1, b+1);
            } else {
                pthread_mutex_unlock(&brazo->mutex);
                usleep(100000);
                continue;
            }
        }
        pthread_mutex_unlock(&brazo->mutex);
        
        // Verificar si la celda está activa
        pthread_mutex_lock(&celda->mutex);
        if (celda->estado != CELDA_ACTIVA) {
            pthread_mutex_unlock(&celda->mutex);
            usleep(100000);
            continue;
        }
        pthread_mutex_unlock(&celda->mutex);
        
        // Intentar retirar una pieza de la banda (máx 2 brazos simultáneos)
        if (sem_trywait(&celda->sem_brazos_retirando) == 0) {
            PosicionBanda *pos = &sistema->banda.posiciones[celda->posicion_banda];
            pthread_mutex_lock(&pos->mutex);
            
            // Buscar una pieza que necesitemos
            int pieza_encontrada = -1;
            
            pthread_mutex_lock(&celda->caja.mutex);
            for (int p = 0; p < pos->num_piezas; p++) {
                int tipo = pos->piezas[p].tipo;
                if (tipo > 0 && necesita_pieza_tipo(&celda->caja, tipo)) {
                    pieza_encontrada = p;
                    break;
                }
            }
            pthread_mutex_unlock(&celda->caja.mutex);
            
            if (pieza_encontrada >= 0) {
                // Retirar la pieza
                pthread_mutex_lock(&brazo->mutex);
                brazo->estado = BRAZO_RETIRANDO;
                brazo->pieza_actual = pos->piezas[pieza_encontrada];
                pthread_mutex_unlock(&brazo->mutex);
                
                // Quitar pieza de la banda
                for (int p = pieza_encontrada; p < pos->num_piezas - 1; p++) {
                    pos->piezas[p] = pos->piezas[p + 1];
                }
                pos->num_piezas--;
                
                pthread_mutex_unlock(&pos->mutex);
                sem_post(&celda->sem_brazos_retirando);
                
                usleep(50000);  // Simular tiempo de retirar
                
                // Colocar en la caja (solo 1 brazo a la vez)
                sem_wait(&celda->caja.sem_acceso);
                
                pthread_mutex_lock(&brazo->mutex);
                brazo->estado = BRAZO_COLOCANDO;
                pthread_mutex_unlock(&brazo->mutex);
                
                pthread_mutex_lock(&celda->caja.mutex);
                
                int tipo = brazo->pieza_actual.tipo;
                
                // Verificar de nuevo si necesitamos esta pieza
                if (tipo > 0 && tipo <= MAX_TIPOS_PIEZA && 
                    !celda->caja.completa &&
                    celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                    
                    celda->caja.piezas_por_tipo[tipo - 1]++;
                    brazo->piezas_movidas++;
                    
                    pthread_mutex_lock(&sistema->stats.mutex);
                    sistema->stats.piezas_por_brazo[c][b]++;
                    pthread_mutex_unlock(&sistema->stats.mutex);
                    
                    printf("[CELDA %d][BRAZO %d] Colocó pieza tipo %s [%d/%d]\n",
                           c+1, b+1, nombre_tipo_pieza(tipo),
                           celda->caja.piezas_por_tipo[tipo - 1],
                           celda->caja.piezas_necesarias[tipo - 1]);
                    
                    // Verificar si el SET está completo
                    if (verificar_caja_completa(&celda->caja)) {
                        celda->caja.completa = true;
                        printf("[CELDA %d][BRAZO %d] ★ ¡SET COMPLETO! Notificando operador...\n",
                               c+1, b+1);
                        
                        pthread_mutex_unlock(&celda->caja.mutex);
                        sem_post(&celda->caja.sem_acceso);
                        
                        pthread_mutex_lock(&celda->mutex);
                        celda->estado = CELDA_ESPERANDO_OP;
                        pthread_mutex_unlock(&celda->mutex);
                        
                        notificar_operador(celda);
                        
                        pthread_mutex_lock(&celda->mutex);
                        celda->estado = CELDA_ACTIVA;
                        pthread_mutex_unlock(&celda->mutex);
                        
                        pthread_mutex_lock(&brazo->mutex);
                        brazo->estado = BRAZO_IDLE;
                        brazo->pieza_actual.tipo = 0;
                        pthread_mutex_unlock(&brazo->mutex);
                        
                        continue;
                    }
                } else if (tipo > 0) {
                    // La pieza ya no se necesita, devolverla a la banda
                    printf("[CELDA %d][BRAZO %d] Pieza tipo %s ya no necesaria, devuelta\n",
                           c+1, b+1, nombre_tipo_pieza(tipo));
                    
                    PosicionBanda *pos_dev = &sistema->banda.posiciones[celda->posicion_banda];
                    pthread_mutex_lock(&pos_dev->mutex);
                    if (pos_dev->num_piezas < MAX_PIEZAS_POS) {
                        pos_dev->piezas[pos_dev->num_piezas] = brazo->pieza_actual;
                        pos_dev->num_piezas++;
                    }
                    pthread_mutex_unlock(&pos_dev->mutex);
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
        }
        
        usleep(10000);
    }
    
    printf("[CELDA %d][BRAZO %d] Terminado\n", c+1, b+1);
    return NULL;
}
