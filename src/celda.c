/**
 * LEGO Master - Implementación de Celdas y Brazos Robóticos
 */

#include "celda.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

void notificar_operador(CeldaEmpaquetado *celda) {
    char respuesta[10];
    bool respuesta_valida = false;
    
    // Mostrar contenido de la caja al operador
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║          🔔 CELDA %d - CAJA LISTA PARA REVISIÓN              ║\n", celda->id + 1);
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  Contenido de la caja:                                        ║\n");
    
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        printf("║    Tipo %s: %d / %d piezas                                    ║\n",
               nombre_tipo_pieza(t + 1),
               celda->caja.piezas_por_tipo[t],
               celda->caja.piezas_necesarias[t]);
    }
    pthread_mutex_unlock(&celda->caja.mutex);
    
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  ¿La caja está correcta? (ok/fail):                           ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    
    // Esperar respuesta del operador humano
    while (!respuesta_valida && !sistema->terminar) {
        printf("[CELDA %d] Operador, ingrese 'ok' o 'fail': ", celda->id + 1);
        fflush(stdout);
        
        if (fgets(respuesta, sizeof(respuesta), stdin) != NULL) {
            // Eliminar salto de línea
            respuesta[strcspn(respuesta, "\n")] = 0;
            
            if (strcasecmp(respuesta, "ok") == 0) {
                respuesta_valida = true;
                pthread_mutex_lock(&sistema->stats.mutex);
                sistema->stats.cajas_ok++;
                celda->cajas_completadas_ok++;
                printf("[CELDA %d] ✓ Operador confirmó: OK - Caja #%d completada correctamente\n", 
                       celda->id + 1, sistema->stats.cajas_ok);
                pthread_mutex_unlock(&sistema->stats.mutex);
                
                // Incrementar contador global de SETs completados
                pthread_mutex_lock(&sistema->mutex_sets);
                sistema->sets_completados_total++;
                printf("[SISTEMA] SETs completados: %d / %d\n", 
                       sistema->sets_completados_total, sistema->config.num_sets);
                pthread_mutex_unlock(&sistema->mutex_sets);
                
            } else if (strcasecmp(respuesta, "fail") == 0) {
                respuesta_valida = true;
                pthread_mutex_lock(&sistema->stats.mutex);
                sistema->stats.cajas_fail++;
                celda->cajas_completadas_fail++;
                printf("[CELDA %d] ✗ Operador confirmó: FAIL - Caja marcada como incorrecta\n", 
                       celda->id + 1);
                pthread_mutex_unlock(&sistema->stats.mutex);
                
            } else {
                printf("[CELDA %d] Respuesta no válida. Use 'ok' o 'fail'.\n", celda->id + 1);
            }
        }
    }
    
    // Reiniciar la caja para el siguiente SET
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        celda->caja.piezas_por_tipo[t] = 0;
    }
    celda->caja.completa = false;
    pthread_mutex_unlock(&celda->caja.mutex);
    
    // Marcar que esta celda ya no está trabajando en un SET
    pthread_mutex_lock(&celda->mutex);
    celda->trabajando_en_set = false;
    pthread_mutex_unlock(&celda->mutex);
    
    // Decrementar contador de SETs en proceso
    pthread_mutex_lock(&sistema->mutex_sets);
    if (sistema->sets_en_proceso > 0) {
        sistema->sets_en_proceso--;
    }
    pthread_mutex_unlock(&sistema->mutex_sets);
    
    printf("[CELDA %d] Caja retirada. Celda reactivada con caja vacía.\n\n", celda->id + 1);
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

// Buscar y sacar pieza del buffer que necesitemos
static Pieza sacar_del_buffer(CeldaEmpaquetado *celda, int tipo_necesario) {
    Pieza resultado = {0, 0};
    pthread_mutex_lock(&celda->buffer_mutex);
    
    for (int i = 0; i < celda->buffer_count; i++) {
        if (tipo_necesario == -1 || celda->buffer[i].tipo == tipo_necesario) {
            resultado = celda->buffer[i];
            // Compactar buffer
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
        
        // Verificar estado de la celda
        pthread_mutex_lock(&celda->mutex);
        EstadoCelda estado_celda = celda->estado;
        pthread_mutex_unlock(&celda->mutex);
        
        if (estado_celda == CELDA_INACTIVA) {
            usleep(100000);
            continue;
        }
        
        // Si la celda está esperando al operador, NO tomar más piezas de la banda
        // Las piezas deben pasar a la siguiente celda
        if (estado_celda == CELDA_ESPERANDO_OP) {
            usleep(50000);
            continue;
        }
        
        // Verificar si ya se completaron todos los SETs necesarios
        pthread_mutex_lock(&sistema->mutex_sets);
        int sets_completados = sistema->sets_completados_total;
        int sets_en_proceso = sistema->sets_en_proceso;
        int total_ocupados = sets_completados + sets_en_proceso;
        pthread_mutex_unlock(&sistema->mutex_sets);
        
        // Verificar si esta celda ya está trabajando en un SET
        pthread_mutex_lock(&celda->mutex);
        bool ya_trabajando = celda->trabajando_en_set;
        pthread_mutex_unlock(&celda->mutex);
        
        // Si ya hay suficientes SETs (completados + en proceso) Y esta celda NO está trabajando
        // en uno, entonces no debe tomar nuevas piezas
        if (!ya_trabajando && total_ocupados >= sistema->config.num_sets) {
            usleep(50000);
            continue;
        }
        
        // FASE 1: Intentar retirar pieza de la banda (solo si celda activa)
        pthread_mutex_lock(&celda->buffer_mutex);
        int buffer_actual = celda->buffer_count;
        pthread_mutex_unlock(&celda->buffer_mutex);
        
        if (buffer_actual < MAX_BUFFER_CELDA - 2) {
            if (sem_trywait(&celda->sem_brazos_retirando) == 0) {
                PosicionBanda *pos = &sistema->banda.posiciones[celda->posicion_banda];
                pthread_mutex_lock(&pos->mutex);
                
                // Buscar pieza que necesitamos para el SET actual
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
                    Pieza pieza_tomada = pos->piezas[pieza_encontrada];
                    
                    // Quitar pieza de la banda
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
                    
                    usleep(30000);  // Tiempo de retirar
                    
                    // FASE 2: Colocar en la caja
                    sem_wait(&celda->caja.sem_acceso);
                    
                    pthread_mutex_lock(&brazo->mutex);
                    brazo->estado = BRAZO_COLOCANDO;
                    pthread_mutex_unlock(&brazo->mutex);
                    
                    pthread_mutex_lock(&celda->caja.mutex);
                    
                    int tipo = brazo->pieza_actual.tipo;
                    
                    if (tipo > 0 && tipo <= MAX_TIPOS_PIEZA && 
                        !celda->caja.completa &&
                        celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                        
                        // Verificar si es la primera pieza del SET
                        bool es_primera_pieza = true;
                        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                            if (celda->caja.piezas_por_tipo[t] > 0) {
                                es_primera_pieza = false;
                                break;
                            }
                        }
                        
                        // Si es la primera pieza, marcar que esta celda está trabajando en un SET
                        if (es_primera_pieza) {
                            pthread_mutex_lock(&celda->mutex);
                            celda->trabajando_en_set = true;
                            pthread_mutex_unlock(&celda->mutex);
                            
                            pthread_mutex_lock(&sistema->mutex_sets);
                            sistema->sets_en_proceso++;
                            printf("[SISTEMA] Celda %d inicia nuevo SET. En proceso: %d\n", 
                                   c+1, sistema->sets_en_proceso);
                            pthread_mutex_unlock(&sistema->mutex_sets);
                        }
                        
                        celda->caja.piezas_por_tipo[tipo - 1]++;
                        brazo->piezas_movidas++;
                        
                        pthread_mutex_lock(&sistema->stats.mutex);
                        sistema->stats.piezas_por_brazo[c][b]++;
                        pthread_mutex_unlock(&sistema->stats.mutex);
                        
                        printf("[CELDA %d][BRAZO %d] Colocó pieza tipo %s [%d/%d]\n",
                               c+1, b+1, nombre_tipo_pieza(tipo),
                               celda->caja.piezas_por_tipo[tipo - 1],
                               celda->caja.piezas_necesarias[tipo - 1]);
                        
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
                        // No necesitamos esta pieza, al buffer
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
            }
        }
        
        // FASE 3: Si la celda está activa, intentar usar piezas del buffer
        if (estado_celda == CELDA_ACTIVA && hay_pieza_en_buffer(celda, &celda->caja)) {
            sem_wait(&celda->caja.sem_acceso);
            
            pthread_mutex_lock(&celda->caja.mutex);
            
            // Buscar en buffer una pieza que necesitemos
            for (int tipo = 1; tipo <= MAX_TIPOS_PIEZA; tipo++) {
                if (celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                    Pieza p = sacar_del_buffer(celda, tipo);
                    if (p.tipo > 0) {
                        // Verificar si es la primera pieza del SET
                        bool es_primera = true;
                        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                            if (celda->caja.piezas_por_tipo[t] > 0) {
                                es_primera = false;
                                break;
                            }
                        }
                        
                        if (es_primera) {
                            pthread_mutex_lock(&celda->mutex);
                            celda->trabajando_en_set = true;
                            pthread_mutex_unlock(&celda->mutex);
                            
                            pthread_mutex_lock(&sistema->mutex_sets);
                            sistema->sets_en_proceso++;
                            printf("[SISTEMA] Celda %d inicia nuevo SET (buffer). En proceso: %d\n", 
                                   c+1, sistema->sets_en_proceso);
                            pthread_mutex_unlock(&sistema->mutex_sets);
                        }
                        
                        celda->caja.piezas_por_tipo[tipo - 1]++;
                        brazo->piezas_movidas++;
                        
                        pthread_mutex_lock(&sistema->stats.mutex);
                        sistema->stats.piezas_por_brazo[c][b]++;
                        pthread_mutex_unlock(&sistema->stats.mutex);
                        
                        printf("[CELDA %d][BRAZO %d] Del buffer: pieza tipo %s [%d/%d]\n",
                               c+1, b+1, nombre_tipo_pieza(tipo),
                               celda->caja.piezas_por_tipo[tipo - 1],
                               celda->caja.piezas_necesarias[tipo - 1]);
                        
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
                            
                            continue;
                        }
                        break;  // Solo una pieza por iteración
                    }
                }
            }
            
            pthread_mutex_unlock(&celda->caja.mutex);
            sem_post(&celda->caja.sem_acceso);
        }
        
        usleep(10000);
    }
    
    printf("[CELDA %d][BRAZO %d] Terminado\n", c+1, b+1);
    return NULL;
}
