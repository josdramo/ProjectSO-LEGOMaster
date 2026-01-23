/**
 * LEGO Master - Implementación de Dispensadores
 */

#include "dispensador.h"
#include "celda.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

// Variables externas
extern SistemaLego *sistema;

// Variables locales del módulo
static int id_pieza_global = 0;
static pthread_mutex_t mutex_id_pieza = PTHREAD_MUTEX_INITIALIZER;

int generar_id_pieza(void) {
    pthread_mutex_lock(&mutex_id_pieza);
    int id = ++id_pieza_global;
    pthread_mutex_unlock(&mutex_id_pieza);
    return id;
}

void* thread_dispensador(void* arg) {
    (void)arg;
    
    int total_piezas = 0;
    int piezas_restantes[MAX_TIPOS_PIEZA];
    
    // Calcular total de piezas a dispensar
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        piezas_restantes[t] = sistema->config.piezas_por_tipo[t] * sistema->config.num_sets;
        total_piezas += piezas_restantes[t];
    }
    
    // Mensajes de inicio eliminados para reducir ruido
    
    int intervalo_us = 1000000 / sistema->banda.velocidad / 2;
    
    while (total_piezas > 0 && !sistema->terminar) {
        usleep(intervalo_us);
        
        PosicionBanda *inicio = &sistema->banda.posiciones[0];
        pthread_mutex_lock(&inicio->mutex);
        
        // Cada dispensador puede soltar una pieza (o no)
        // Límite por ciclo = número de dispensadores (no más piezas de las que pueden dispensar)
        int limite_piezas_ciclo = sistema->config.num_dispensadores;
        for (int d = 0; d < sistema->config.num_dispensadores && total_piezas > 0; d++) {
            if (inicio->num_piezas >= limite_piezas_ciclo) break;
            
            // Decidir aleatoriamente si dispensar y qué tipo
            if (rand() % 5 < 4) {  // 80% probabilidad de dispensar
                int tipo = rand() % MAX_TIPOS_PIEZA;
                
                // Buscar un tipo que aún tenga piezas
                int intentos = 0;
                while (piezas_restantes[tipo] <= 0 && intentos < MAX_TIPOS_PIEZA) {
                    tipo = (tipo + 1) % MAX_TIPOS_PIEZA;
                    intentos++;
                }
                
                if (piezas_restantes[tipo] > 0) {
                    int idx = inicio->num_piezas;
                    inicio->piezas[idx].tipo = tipo + 1;
                    inicio->piezas[idx].id_unico = generar_id_pieza();
                    inicio->num_piezas++;
                    piezas_restantes[tipo]--;
                    total_piezas--;
                    
                    pthread_mutex_lock(&sistema->stats.mutex);
                    sistema->stats.total_piezas_dispensadas++;
                    pthread_mutex_unlock(&sistema->stats.mutex);
                    
                    sistema->piezas_dispensadas_ciclo++;
                }
            }
        }
        
        pthread_mutex_unlock(&inicio->mutex);
        
        // Verificar si hay que suspender algún brazo (cada Y piezas)
        if (sistema->piezas_dispensadas_ciclo >= sistema->config.Y) {
            sistema->piezas_dispensadas_ciclo = 0;
            
            for (int c = 0; c < sistema->config.num_celdas; c++) {
                int brazo_max = encontrar_brazo_max_piezas(&sistema->celdas[c]);
                if (brazo_max >= 0) {
                    BrazoRobotico *brazo = &sistema->celdas[c].brazos[brazo_max];
                    pthread_mutex_lock(&brazo->mutex);
                    if (brazo->estado == BRAZO_IDLE) {
                        brazo->estado = BRAZO_SUSPENDIDO;
                        brazo->tiempo_suspension = time(NULL);
                        // Mensaje de balanceo eliminado para reducir ruido en consola
                    }
                    pthread_mutex_unlock(&brazo->mutex);
                }
            }
        }
    }
    
    printf("[SISTEMA] Todas las piezas dispensadas (%d). Esperando que la banda se vacíe...\n",
           sistema->stats.total_piezas_dispensadas);
    
    // Esperar a que la banda se vacíe
    int tiempo_espera = (sistema->banda.longitud / sistema->banda.velocidad) + 3;
    sleep(tiempo_espera);
    
    // Esperar a que todos los SETs sean confirmados por el operador (con timeout)
    
    // Calcular timeout basado en el número de SETs y tiempo máximo del operador
    int timeout_confirmacion = sistema->config.num_sets * 
                               (sistema->config.delta_t1_max / 1000 + 2) + 15;
    int tiempo_esperado = 0;
    int ultimo_completado = 0;
    int ciclos_sin_progreso = 0;
    
    while (!sistema->terminar && tiempo_esperado < timeout_confirmacion) {
        pthread_mutex_lock(&sistema->mutex_sets);
        int completados = sistema->sets_completados_total;
        int en_proceso = sistema->sets_en_proceso;
        pthread_mutex_unlock(&sistema->mutex_sets);
        
        // Si ya se completaron todos los SETs esperados, terminar
        if (completados >= sistema->config.num_sets) {
            printf("\n[SISTEMA] ✓ Todos los SETs completados (%d/%d)\n",
                   completados, sistema->config.num_sets);
            break;
        }
        
        // Verificar si hay progreso
        if (completados > ultimo_completado) {
            ultimo_completado = completados;
            ciclos_sin_progreso = 0;
        } else {
            ciclos_sin_progreso++;
        }
        
        // Contar piezas totales disponibles (banda + buffers + cajas)
        int piezas_disponibles = 0;
        
        // Piezas en la banda
        for (int i = 0; i < sistema->banda.longitud; i++) {
            PosicionBanda *pos = &sistema->banda.posiciones[i];
            pthread_mutex_lock(&pos->mutex);
            piezas_disponibles += pos->num_piezas;
            pthread_mutex_unlock(&pos->mutex);
        }
        
        // Piezas en buffers y cajas de las celdas
        for (int c = 0; c < sistema->config.num_celdas; c++) {
            CeldaEmpaquetado *celda = &sistema->celdas[c];
            pthread_mutex_lock(&celda->buffer_mutex);
            piezas_disponibles += celda->buffer_count;
            pthread_mutex_unlock(&celda->buffer_mutex);
            
            pthread_mutex_lock(&celda->caja.mutex);
            for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                piezas_disponibles += celda->caja.piezas_por_tipo[t];
            }
            pthread_mutex_unlock(&celda->caja.mutex);
        }
        
        // Calcular piezas necesarias para completar los SETs restantes
        int sets_restantes = sistema->config.num_sets - completados;
        int piezas_por_set = 0;
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            piezas_por_set += sistema->config.piezas_por_tipo[t];
        }
        int piezas_necesarias = sets_restantes * piezas_por_set;
        
        // Si no hay suficientes piezas para completar más SETs, terminar
        // NOTA: Aunque haya SETs en proceso, si no hay piezas suficientes,
        // las celdas deberían liberar sus piezas para que otras las usen
        if (piezas_disponibles < piezas_necesarias && en_proceso == 0) {
            printf("\n[SISTEMA] ✗ Piezas insuficientes. Completados: %d/%d\n",
                   completados, sistema->config.num_sets);
            break;
        }
        
        // Forzar liberación de piezas si hay celdas estancadas con SETs en proceso
        // pero sin progreso durante mucho tiempo
        if (en_proceso > 0 && ciclos_sin_progreso > 10) {  // 5 segundos sin progreso con SETs en proceso
            // Buscar celdas estancadas y forzar liberación de piezas (silencioso)
            for (int c = 0; c < sistema->config.num_celdas; c++) {
                CeldaEmpaquetado *celda = &sistema->celdas[c];
                
                pthread_mutex_lock(&celda->mutex);
                bool trabajando = celda->trabajando_en_set;
                EstadoCelda estado = celda->estado;
                bool devolviendo = celda->devolviendo_piezas;
                pthread_mutex_unlock(&celda->mutex);
                
                // Si la celda está trabajando pero no esperando al operador ni devolviendo
                if (trabajando && estado == CELDA_ACTIVA && !devolviendo) {
                    // Verificar si esta celda puede completar su SET por tipo
                    int piezas_faltan_por_tipo[MAX_TIPOS_PIEZA] = {0};
                    int piezas_celda = 0;
                    int faltan_celda = 0;
                    
                    pthread_mutex_lock(&celda->caja.mutex);
                    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                        piezas_celda += celda->caja.piezas_por_tipo[t];
                        int faltan = celda->caja.piezas_necesarias[t] - celda->caja.piezas_por_tipo[t];
                        if (faltan > 0) {
                            piezas_faltan_por_tipo[t] = faltan;
                            faltan_celda += faltan;
                        }
                    }
                    pthread_mutex_unlock(&celda->caja.mutex);
                    
                    // Si no le falta nada, no hacer nada
                    if (faltan_celda == 0) continue;
                    
                    // Contar piezas disponibles por tipo (banda + buffer)
                    int piezas_disponibles_por_tipo[MAX_TIPOS_PIEZA] = {0};
                    
                    // Piezas en buffer
                    pthread_mutex_lock(&celda->buffer_mutex);
                    for (int i = 0; i < celda->buffer_count; i++) {
                        int tipo = celda->buffer[i].tipo;
                        if (tipo >= 1 && tipo <= MAX_TIPOS_PIEZA) {
                            piezas_disponibles_por_tipo[tipo - 1]++;
                        }
                    }
                    pthread_mutex_unlock(&celda->buffer_mutex);
                    
                    // Piezas en banda antes de esta celda
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
                    
                    // Verificar si puede completar
                    bool puede_completar = true;
                    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                        if (piezas_faltan_por_tipo[t] > piezas_disponibles_por_tipo[t]) {
                            puede_completar = false;
                            break;
                        }
                    }
                    
                    // Solo forzar liberación si NO puede completar y NO es la última celda
                    bool es_ultima_celda = (c == sistema->config.num_celdas - 1);
                    if (!puede_completar && !es_ultima_celda && piezas_celda > 0) {
                        devolver_piezas_a_banda(celda);
                    }
                }
            }
            
            // Resetear contador para dar tiempo a que el sistema se recupere
            ciclos_sin_progreso = 0;
        }
        
        // Verificar si hay alguna celda esperando al operador
        bool hay_celda_esperando_operador = false;
        for (int c = 0; c < sistema->config.num_celdas; c++) {
            pthread_mutex_lock(&sistema->celdas[c].mutex);
            if (sistema->celdas[c].estado == CELDA_ESPERANDO_OP) {
                hay_celda_esperando_operador = true;
            }
            pthread_mutex_unlock(&sistema->celdas[c].mutex);
            if (hay_celda_esperando_operador) break;
        }
        
        // Si no hay progreso después de varios ciclos Y no hay celda esperando al operador
        if (ciclos_sin_progreso > 20 && !hay_celda_esperando_operador) {  // 10 segundos sin progreso
            printf("\n[SISTEMA] Sin progreso. Completados: %d/%d\n",
                   completados, sistema->config.num_sets);
            break;
        }
        
        // Si hay una celda esperando al operador, resetear timeout parcialmente
        if (hay_celda_esperando_operador && ciclos_sin_progreso > 10) {
            ciclos_sin_progreso = 10;  // No dejar que crezca demasiado mientras espera operador
        }
        
        usleep(500000);  // Revisar cada 0.5 segundos
        tiempo_esperado++;
    }
    
    if (tiempo_esperado >= timeout_confirmacion) {
        printf("\n[SISTEMA] Timeout. Terminando simulación.\n");
    }
    
    sistema->terminar = true;
    
    return NULL;
}
