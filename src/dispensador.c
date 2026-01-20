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
    
    // Esperar a que todos los SETs sean confirmados por el operador
    printf("[DISPENSADORES] Esperando confirmación de SETs pendientes...\n");
    while (!sistema->terminar) {
        pthread_mutex_lock(&sistema->mutex_sets);
        int completados = sistema->sets_completados_total;
        int en_proceso = sistema->sets_en_proceso;
        pthread_mutex_unlock(&sistema->mutex_sets);
        
        // Si ya se completaron todos los SETs esperados, terminar
        if (completados >= sistema->config.num_sets) {
            printf("[DISPENSADORES] Todos los SETs confirmados. Terminando simulación.\n");
            break;
        }
        
        // Si no hay SETs en proceso y no se completaron todos, algo falló
        if (en_proceso == 0 && completados < sistema->config.num_sets) {
            printf("[DISPENSADORES] No hay más SETs en proceso. Terminando con %d/%d completados.\n",
                   completados, sistema->config.num_sets);
            break;
        }
        
        usleep(500000);  // Revisar cada 0.5 segundos
    }
    
    sistema->terminar = true;
    
    return NULL;
}
