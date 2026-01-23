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
    
    printf("[DISPENSADORES] Iniciados - total piezas a dispensar: %d\n", total_piezas);
    printf("[DISPENSADORES] Por tipo: A=%d, B=%d, C=%d, D=%d\n",
           piezas_restantes[0], piezas_restantes[1], 
           piezas_restantes[2], piezas_restantes[3]);
    
    int intervalo_us = 1000000 / sistema->banda.velocidad / 2;
    
    while (total_piezas > 0 && !sistema->terminar) {
        usleep(intervalo_us);
        
        PosicionBanda *inicio = &sistema->banda.posiciones[0];
        pthread_mutex_lock(&inicio->mutex);
        
        // Cada dispensador puede soltar una pieza (o no)
        for (int d = 0; d < sistema->config.num_dispensadores && total_piezas > 0; d++) {
            if (inicio->num_piezas >= MAX_PIEZAS_POS) break;
            
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
                        printf("[CELDA %d] Brazo %d suspendido por balanceo (piezas: %d)\n",
                               c+1, brazo_max+1, brazo->piezas_movidas);
                    }
                    pthread_mutex_unlock(&brazo->mutex);
                }
            }
        }
    }
    
    printf("[DISPENSADORES] Terminados - todas las piezas dispensadas (%d)\n",
           sistema->stats.total_piezas_dispensadas);
    
    // Esperar a que la banda se vacíe
    int tiempo_espera = (sistema->banda.longitud / sistema->banda.velocidad) + 3;
    printf("[DISPENSADORES] Esperando %d segundos para que la banda se vacíe...\n", tiempo_espera);
    sleep(tiempo_espera);
    
    // Esperar a que todos los SETs sean confirmados por el operador (con timeout)
    printf("[DISPENSADORES] Esperando confirmación de SETs pendientes...\n");
    
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
            printf("[DISPENSADORES] Todos los SETs confirmados (%d/%d). Terminando simulación.\n",
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
            printf("[DISPENSADORES] No hay suficientes piezas para más SETs "
                   "(disponibles: %d, necesarias: %d). Terminando con %d/%d completados.\n",
                   piezas_disponibles, piezas_necesarias, completados, sistema->config.num_sets);
            break;
        }
        
        // Forzar liberación de piezas si hay celdas estancadas con SETs en proceso
        // pero sin progreso durante mucho tiempo
        if (en_proceso > 0 && ciclos_sin_progreso > 6) {  // 3 segundos sin progreso con SETs en proceso
            printf("[DISPENSADORES] Detectado estancamiento con %d SETs en proceso. Forzando liberación de piezas...\n", 
                   en_proceso);
            
            // Buscar celdas estancadas y forzar liberación de piezas
            for (int c = 0; c < sistema->config.num_celdas; c++) {
                CeldaEmpaquetado *celda = &sistema->celdas[c];
                
                pthread_mutex_lock(&celda->mutex);
                bool trabajando = celda->trabajando_en_set;
                EstadoCelda estado = celda->estado;
                pthread_mutex_unlock(&celda->mutex);
                
                // Si la celda está trabajando pero no esperando al operador,
                // y no ha progresado, forzar liberación
                if (trabajando && estado == CELDA_ACTIVA) {
                    // Verificar si esta celda puede completar su SET
                    int piezas_celda = 0;
                    int faltan_celda = 0;
                    
                    pthread_mutex_lock(&celda->caja.mutex);
                    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                        piezas_celda += celda->caja.piezas_por_tipo[t];
                        faltan_celda += celda->caja.piezas_necesarias[t] - celda->caja.piezas_por_tipo[t];
                    }
                    pthread_mutex_unlock(&celda->caja.mutex);
                    
                    // Si tiene piezas pero no puede completar, liberar
                    if (piezas_celda > 0 && faltan_celda > 0) {
                        printf("[DISPENSADORES] Celda %d estancada (tiene %d piezas, faltan %d). Forzando liberación.\n",
                               c + 1, piezas_celda, faltan_celda);
                        devolver_piezas_a_banda(celda);
                    }
                }
            }
            
            // Resetear contador para dar tiempo a que el sistema se recupere
            ciclos_sin_progreso = 0;
        }
        
        // Si no hay progreso después de varios ciclos
        if (ciclos_sin_progreso > 20) {  // 10 segundos sin progreso
            printf("[DISPENSADORES] Sin progreso por %d segundos. "
                   "Terminando con %d/%d completados.\n",
                   ciclos_sin_progreso / 2, completados, sistema->config.num_sets);
            break;
        }
        
        usleep(500000);  // Revisar cada 0.5 segundos
        tiempo_esperado++;
    }
    
    if (tiempo_esperado >= timeout_confirmacion) {
        printf("[DISPENSADORES] Timeout esperando confirmaciones. Terminando simulación.\n");
    }
    
    sistema->terminar = true;
    
    return NULL;
}
