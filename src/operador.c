/**
 * LEGO Master - Implementación del Operador Humano
 */

#define _POSIX_C_SOURCE 200809L

#include "operador.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

// Variable externa del sistema
extern SistemaLego *sistema;

// Variables para el hilo del operador
static pthread_t hilo_operador;
static pthread_mutex_t mutex_operador = PTHREAD_MUTEX_INITIALIZER;
static bool operador_activo = false;

// Cola de celdas esperando confirmación del operador
#define MAX_COLA_OPERADOR 10
static int cola_celdas_esperando[MAX_COLA_OPERADOR];
static int cola_inicio = 0;
static int cola_fin = 0;
static int cola_count = 0;
static pthread_mutex_t mutex_cola = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_cola = PTHREAD_COND_INITIALIZER;

// Agregar celda a la cola de espera del operador
static void encolar_celda_operador(int celda_id) {
    pthread_mutex_lock(&mutex_cola);
    if (cola_count < MAX_COLA_OPERADOR) {
        cola_celdas_esperando[cola_fin] = celda_id;
        cola_fin = (cola_fin + 1) % MAX_COLA_OPERADOR;
        cola_count++;
        pthread_cond_signal(&cond_cola);
    }
    pthread_mutex_unlock(&mutex_cola);
}

// Procesar respuesta del operador para una celda específica
static void procesar_respuesta_operador(int celda_id, const char* respuesta) {
    CeldaEmpaquetado *celda = &sistema->celdas[celda_id];
    
    if (strcasecmp(respuesta, "ok") == 0) {
        pthread_mutex_lock(&sistema->stats.mutex);
        sistema->stats.cajas_ok++;
        celda->cajas_completadas_ok++;
        pthread_mutex_unlock(&sistema->stats.mutex);
        
        pthread_mutex_lock(&sistema->mutex_sets);
        sistema->sets_completados_total++;
        printf("[CELDA %d] ✓ SET #%d OK (%d/%d completados)\n", 
               celda_id + 1, sistema->sets_completados_total,
               sistema->sets_completados_total, sistema->config.num_sets);
        pthread_mutex_unlock(&sistema->mutex_sets);
        
    } else if (strcasecmp(respuesta, "fail") == 0) {
        pthread_mutex_lock(&sistema->stats.mutex);
        sistema->stats.cajas_fail++;
        celda->cajas_completadas_fail++;
        printf("[CELDA %d] ✗ SET marcado FAIL\n", celda_id + 1);
        pthread_mutex_unlock(&sistema->stats.mutex);
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
    celda->estado = CELDA_ACTIVA;
    pthread_mutex_unlock(&celda->mutex);
    
    // Decrementar contador de SETs en proceso
    pthread_mutex_lock(&sistema->mutex_sets);
    if (sistema->sets_en_proceso > 0) {
        sistema->sets_en_proceso--;
    }
    pthread_mutex_unlock(&sistema->mutex_sets);
}

// Hilo dedicado del operador - AUTOMÁTICO con tiempo aleatorio
static void* thread_operador(void* arg) {
    (void)arg;
    
    while (!sistema->terminar) {
        pthread_mutex_lock(&mutex_cola);
        
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; // 100ms timeout
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        
        while (cola_count == 0 && !sistema->terminar) {
            pthread_cond_timedwait(&cond_cola, &mutex_cola, &ts);
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
        }
        
        if (sistema->terminar) {
            pthread_mutex_unlock(&mutex_cola);
            break;
        }
        
        int celda_id = -1;
        if (cola_count > 0) {
            celda_id = cola_celdas_esperando[cola_inicio];
            cola_inicio = (cola_inicio + 1) % MAX_COLA_OPERADOR;
            cola_count--;
        }
        pthread_mutex_unlock(&mutex_cola);
        
        if (celda_id < 0) continue;
        
        CeldaEmpaquetado *celda = &sistema->celdas[celda_id];
        
        pthread_mutex_lock(&celda->caja.mutex);
        bool caja_correcta = true;
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            if (celda->caja.piezas_por_tipo[t] != celda->caja.piezas_necesarias[t]) {
                caja_correcta = false;
            }
        }
        pthread_mutex_unlock(&celda->caja.mutex);
        
        int tiempo_revision_ms = rand() % (sistema->config.delta_t1_max + 1);
        usleep(tiempo_revision_ms * 1000);
        
        const char* resultado = caja_correcta ? "ok" : "fail";
        procesar_respuesta_operador(celda_id, resultado);
    }
    
    return NULL;
}

void iniciar_hilo_operador(void) {
    pthread_mutex_lock(&mutex_operador);
    if (!operador_activo) {
        operador_activo = true;
        cola_inicio = 0;
        cola_fin = 0;
        cola_count = 0;
        pthread_create(&hilo_operador, NULL, thread_operador, NULL);
    }
    pthread_mutex_unlock(&mutex_operador);
}

void terminar_hilo_operador(void) {
    pthread_mutex_lock(&mutex_operador);
    if (operador_activo) {
        operador_activo = false;
        pthread_cond_broadcast(&cond_cola);
        pthread_mutex_unlock(&mutex_operador);
        pthread_join(hilo_operador, NULL);
        
        pthread_mutex_lock(&mutex_cola);
        while (cola_count > 0) {
            int celda_id = cola_celdas_esperando[cola_inicio];
            cola_inicio = (cola_inicio + 1) % MAX_COLA_OPERADOR;
            cola_count--;
            pthread_mutex_unlock(&mutex_cola);
            
            printf("[OPERADOR] Procesando celda %d pendiente (cierre del sistema)\n", celda_id + 1);
            procesar_respuesta_operador(celda_id, "ok");
            
            pthread_mutex_lock(&mutex_cola);
        }
        pthread_mutex_unlock(&mutex_cola);
    } else {
        pthread_mutex_unlock(&mutex_operador);
    }
}

void notificar_operador(CeldaEmpaquetado *celda) {
    encolar_celda_operador(celda->id);
}
