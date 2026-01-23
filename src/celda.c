/**
 * LEGO Master - Implementación de Celdas y Brazos Robóticos
 */

#define _POSIX_C_SOURCE 199309L

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

// Contar piezas de un tipo específico en el buffer
static int contar_tipo_en_buffer(CeldaEmpaquetado *celda, int tipo) {
    int count = 0;
    // NOTA: El llamador debe tener buffer_mutex
    for (int i = 0; i < celda->buffer_count; i++) {
        if (celda->buffer[i].tipo == tipo) {
            count++;
        }
    }
    return count;
}

// Verificar si la celda realmente necesita más piezas de este tipo
// considerando tanto la caja como el buffer
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
    
    // Solo necesitamos más si caja + buffer < necesarias
    return (en_caja + en_buffer) < necesarias;
}

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
        printf("[CELDA %d] ✓ Operador confirmó: OK - Caja #%d completada correctamente\n", 
               celda_id + 1, sistema->stats.cajas_ok);
        pthread_mutex_unlock(&sistema->stats.mutex);
        
        pthread_mutex_lock(&sistema->mutex_sets);
        sistema->sets_completados_total++;
        printf("[SISTEMA] SETs completados: %d / %d\n", 
               sistema->sets_completados_total, sistema->config.num_sets);
        pthread_mutex_unlock(&sistema->mutex_sets);
        
    } else if (strcasecmp(respuesta, "fail") == 0) {
        pthread_mutex_lock(&sistema->stats.mutex);
        sistema->stats.cajas_fail++;
        celda->cajas_completadas_fail++;
        printf("[CELDA %d] ✗ Operador confirmó: FAIL - Caja marcada como incorrecta\n", 
               celda_id + 1);
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
    
    printf("[CELDA %d] Caja retirada. Celda reactivada con caja vacía.\n\n", celda_id + 1);
}

// Hilo dedicado del operador - AUTOMÁTICO con tiempo aleatorio
static void* thread_operador(void* arg) {
    (void)arg;
    
    printf("[OPERADOR] Hilo del operador automático iniciado\n");
    
    while (!sistema->terminar) {
        // Esperar a que haya celdas en cola
        pthread_mutex_lock(&mutex_cola);
        
        // Esperar con timeout para poder verificar sistema->terminar
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
        
        // Obtener la siguiente celda de la cola
        int celda_id = -1;
        if (cola_count > 0) {
            celda_id = cola_celdas_esperando[cola_inicio];
            cola_inicio = (cola_inicio + 1) % MAX_COLA_OPERADOR;
            cola_count--;
        }
        pthread_mutex_unlock(&mutex_cola);
        
        if (celda_id < 0) continue;
        
        CeldaEmpaquetado *celda = &sistema->celdas[celda_id];
        
        // Mostrar contenido de la caja
        printf("\n");
        printf("╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║          🔔 CELDA %d - CAJA LISTA PARA REVISIÓN              ║\n", celda_id + 1);
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
        printf("║  Contenido de la caja:                                        ║\n");
        
        pthread_mutex_lock(&celda->caja.mutex);
        bool caja_correcta = true;
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            printf("║    Tipo %s: %d / %d piezas                                    ║\n",
                   nombre_tipo_pieza(t + 1),
                   celda->caja.piezas_por_tipo[t],
                   celda->caja.piezas_necesarias[t]);
            // Verificar si la cantidad es correcta
            if (celda->caja.piezas_por_tipo[t] != celda->caja.piezas_necesarias[t]) {
                caja_correcta = false;
            }
        }
        pthread_mutex_unlock(&celda->caja.mutex);
        
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
        
        // Simular tiempo del operador revisando (entre 0 y delta_t1_max ms)
        int tiempo_revision_ms = rand() % (sistema->config.delta_t1_max + 1);
        printf("[OPERADOR] Revisando caja de Celda %d... (tiempo: %d ms)\n", 
               celda_id + 1, tiempo_revision_ms);
        usleep(tiempo_revision_ms * 1000);
        
        // El operador determina OK o FAIL basándose en si la caja está correcta
        // (en la realidad siempre debería estar correcta si verificar_caja_completa funcionó)
        const char* resultado = caja_correcta ? "ok" : "fail";
        procesar_respuesta_operador(celda_id, resultado);
    }
    
    printf("[OPERADOR] Hilo del operador terminado\n");
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
        
        // Procesar rápidamente las celdas que quedaron pendientes en la cola
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

// Nueva versión de notificar_operador - NO BLOQUEANTE
void notificar_operador(CeldaEmpaquetado *celda) {
    // Solo encolar la celda para que el hilo del operador la procese
    encolar_celda_operador(celda->id);
    
    // NO bloqueamos aquí - el brazo puede continuar su loop
    // El hilo del operador se encargará de procesar la respuesta
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

// Cuenta cuántas piezas tiene la celda (en caja + buffer)
static int contar_piezas_celda(CeldaEmpaquetado *celda) {
    int total = 0;
    
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        total += celda->caja.piezas_por_tipo[t];
    }
    pthread_mutex_unlock(&celda->caja.mutex);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    total += celda->buffer_count;
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    return total;
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
    // Reducido a 30 ciclos (~3 segundos) para ser más reactivo
    return ciclos > 30;
}

// Cuenta cuántas piezas de un tipo específico hay en toda la banda
static int contar_piezas_tipo_en_banda(int tipo) {
    int total = 0;
    for (int i = 0; i < sistema->banda.longitud; i++) {
        PosicionBanda *pos = &sistema->banda.posiciones[i];
        pthread_mutex_lock(&pos->mutex);
        for (int p = 0; p < pos->num_piezas; p++) {
            if (pos->piezas[p].tipo == tipo) {
                total++;
            }
        }
        pthread_mutex_unlock(&pos->mutex);
    }
    return total;
}

// Cuenta cuántas piezas de un tipo tiene una celda (caja + buffer)
static int contar_piezas_tipo_en_celda(CeldaEmpaquetado *celda, int tipo) {
    int total = 0;
    
    pthread_mutex_lock(&celda->caja.mutex);
    if (tipo >= 1 && tipo <= MAX_TIPOS_PIEZA) {
        total += celda->caja.piezas_por_tipo[tipo - 1];
    }
    pthread_mutex_unlock(&celda->caja.mutex);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    for (int i = 0; i < celda->buffer_count; i++) {
        if (celda->buffer[i].tipo == tipo) {
            total++;
        }
    }
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    return total;
}

// Verifica si una celda puede completar su SET con las piezas disponibles globalmente
// NOTA: Esta función es una aproximación - no toma locks para evitar deadlocks
static bool celda_puede_completar_set(CeldaEmpaquetado *celda) {
    // Copiar lo que necesitamos sin mantener locks mucho tiempo
    int necesita[MAX_TIPOS_PIEZA];
    int en_mi_buffer[MAX_TIPOS_PIEZA] = {0};
    
    pthread_mutex_lock(&celda->caja.mutex);
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        necesita[t] = celda->caja.piezas_necesarias[t] - celda->caja.piezas_por_tipo[t];
    }
    pthread_mutex_unlock(&celda->caja.mutex);
    
    pthread_mutex_lock(&celda->buffer_mutex);
    for (int i = 0; i < celda->buffer_count; i++) {
        int tipo = celda->buffer[i].tipo;
        if (tipo >= 1 && tipo <= MAX_TIPOS_PIEZA) {
            en_mi_buffer[tipo - 1]++;
        }
    }
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    // Verificar cada tipo
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        if (necesita[t] > 0) {
            // Contar piezas de este tipo en la banda (sin lock - aproximación)
            int en_banda = contar_piezas_tipo_en_banda(t + 1);
            
            // Si no hay suficientes piezas disponibles, no puedo completar
            if (en_banda + en_mi_buffer[t] < necesita[t]) {
                return false;
            }
        }
    }
    
    return true;
}

// Verifica si alguna OTRA celda necesita las piezas que esta celda tiene
// y además ESA otra celda PUEDE completar su SET si recibe las piezas
bool otra_celda_necesita_piezas(CeldaEmpaquetado *celda) {
    int mi_id = celda->id;
    
    // Revisar TODAS las celdas (no solo las posteriores)
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        if (c == mi_id) continue;  // No compararme conmigo mismo
        
        CeldaEmpaquetado *otra = &sistema->celdas[c];
        
        pthread_mutex_lock(&otra->mutex);
        bool otra_trabajando = otra->trabajando_en_set;
        EstadoCelda otra_estado = otra->estado;
        pthread_mutex_unlock(&otra->mutex);
        
        // Solo considerar celdas activas que están trabajando en un SET
        if (otra_estado != CELDA_ACTIVA || !otra_trabajando) continue;
        
        // Verificar si la otra celda necesita piezas que yo tengo
        pthread_mutex_lock(&celda->caja.mutex);
        pthread_mutex_lock(&otra->caja.mutex);
        
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            // Si yo tengo piezas de este tipo Y la otra celda las necesita
            if (celda->caja.piezas_por_tipo[t] > 0 &&
                otra->caja.piezas_por_tipo[t] < otra->caja.piezas_necesarias[t]) {
                pthread_mutex_unlock(&otra->caja.mutex);
                pthread_mutex_unlock(&celda->caja.mutex);
                return true;
            }
        }
        
        pthread_mutex_unlock(&otra->caja.mutex);
        pthread_mutex_unlock(&celda->caja.mutex);
        
        // También revisar el buffer
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
    printf("[CELDA %d] ⚠ Devolviendo piezas a la banda (SET incompleto, ayudando a otras celdas)\n", 
           celda->id + 1);
    
    // Calcular posición donde devolver las piezas
    // Las piezas deben ir DESPUÉS de esta celda (posición + 1) para que las celdas posteriores las tomen
    int posicion_devolucion = celda->posicion_banda + 1;
    
    // Si esta es la última celda, las piezas van al final (tacho)
    bool es_ultima_celda = (celda->id == sistema->config.num_celdas - 1);
    if (es_ultima_celda) {
        // Devolver al final de la banda (irán al tacho)
        posicion_devolucion = sistema->banda.longitud - 1;
        printf("[CELDA %d] Es la última celda, piezas irán al tacho\n", celda->id + 1);
    } else {
        // Devolver justo después de esta celda
        printf("[CELDA %d] Devolviendo piezas a posición %d (después de esta celda)\n", 
               celda->id + 1, posicion_devolucion);
    }
    
    // Asegurar que la posición es válida
    if (posicion_devolucion >= sistema->banda.longitud) {
        posicion_devolucion = sistema->banda.longitud - 1;
    }
    
    PosicionBanda *pos = &sistema->banda.posiciones[posicion_devolucion];
    
    // Devolver piezas de la caja
    // IMPORTANTE: Primero el semáforo, luego el mutex (mismo orden que en thread_brazo)
    sem_wait(&celda->caja.sem_acceso);
    pthread_mutex_lock(&celda->caja.mutex);
    
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        while (celda->caja.piezas_por_tipo[t] > 0) {
            pthread_mutex_lock(&pos->mutex);
            if (pos->num_piezas < MAX_PIEZAS_POS) {
                Pieza p;
                p.tipo = t + 1;
                p.id_unico = -1;  // Marcamos como pieza devuelta
                pos->piezas[pos->num_piezas++] = p;
                celda->caja.piezas_por_tipo[t]--;
                printf("[CELDA %d] Devolvió pieza tipo %s a la banda (pos %d)\n", 
                       celda->id + 1, nombre_tipo_pieza(t + 1), posicion_devolucion);
            }
            pthread_mutex_unlock(&pos->mutex);
            
            if (celda->caja.piezas_por_tipo[t] > 0) {
                usleep(50000);  // Esperar un poco entre devoluciones
            }
        }
    }
    
    celda->caja.completa = false;
    pthread_mutex_unlock(&celda->caja.mutex);
    sem_post(&celda->caja.sem_acceso);
    
    // Devolver piezas del buffer
    pthread_mutex_lock(&celda->buffer_mutex);
    while (celda->buffer_count > 0) {
        Pieza p = celda->buffer[--celda->buffer_count];
        pthread_mutex_lock(&pos->mutex);
        if (pos->num_piezas < MAX_PIEZAS_POS) {
            pos->piezas[pos->num_piezas++] = p;
            printf("[CELDA %d] Devolvió pieza tipo %s (buffer) a la banda (pos %d)\n", 
                   celda->id + 1, nombre_tipo_pieza(p.tipo), posicion_devolucion);
        }
        pthread_mutex_unlock(&pos->mutex);
    }
    pthread_mutex_unlock(&celda->buffer_mutex);
    
    // Marcar que la celda ya no está trabajando en un SET
    pthread_mutex_lock(&celda->mutex);
    celda->trabajando_en_set = false;
    celda->ciclos_sin_progreso = 0;
    pthread_mutex_unlock(&celda->mutex);
    
    // Decrementar SETs en proceso
    pthread_mutex_lock(&sistema->mutex_sets);
    if (sistema->sets_en_proceso > 0) {
        sistema->sets_en_proceso--;
    }
    pthread_mutex_unlock(&sistema->mutex_sets);
    
    printf("[CELDA %d] Piezas devueltas. Celda lista para nuevo SET.\n", celda->id + 1);
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
        
        // Si la celda está esperando al operador, NO tomar más piezas
        if (estado_celda == CELDA_ESPERANDO_OP) {
            usleep(50000);
            continue;
        }
        
        // ============= SISTEMA DE ASIGNACIÓN DE SETs =============
        // Cada celda puede tener máximo 1 SET activo a la vez
        // Todas las celdas pueden trabajar en paralelo
        pthread_mutex_lock(&sistema->mutex_sets);
        int sets_completados = sistema->sets_completados_total;
        int sets_en_proceso = sistema->sets_en_proceso;
        int sets_necesarios = sistema->config.num_sets;
        pthread_mutex_unlock(&sistema->mutex_sets);
        
        // Si ya se completaron todos los SETs, no hacer nada
        if (sets_completados >= sets_necesarios) {
            usleep(50000);
            continue;
        }
        
        // Verificar si esta celda ya está trabajando en un SET
        pthread_mutex_lock(&celda->mutex);
        bool ya_trabajando = celda->trabajando_en_set;
        pthread_mutex_unlock(&celda->mutex);
        
        // ============= FASE 1: RETIRAR PIEZA DE LA BANDA =============
        pthread_mutex_lock(&celda->buffer_mutex);
        int buffer_actual = celda->buffer_count;
        pthread_mutex_unlock(&celda->buffer_mutex);
        
        if (buffer_actual < MAX_BUFFER_CELDA - 2) {
            if (sem_trywait(&celda->sem_brazos_retirando) == 0) {
                PosicionBanda *pos = &sistema->banda.posiciones[celda->posicion_banda];
                pthread_mutex_lock(&pos->mutex);
                
                // Buscar pieza que necesitamos
                int pieza_encontrada = -1;
                for (int p = 0; p < pos->num_piezas; p++) {
                    int tipo = pos->piezas[p].tipo;
                    if (tipo > 0 && necesita_pieza_tipo_total(celda, tipo)) {
                        pieza_encontrada = p;
                        break;
                    }
                }
                
                if (pieza_encontrada >= 0) {
                    // Si no estoy trabajando, intentar reservar el SET
                    if (!ya_trabajando) {
                        pthread_mutex_lock(&sistema->mutex_sets);
                        pthread_mutex_lock(&celda->mutex);
                        
                        if (!celda->trabajando_en_set && 
                            sistema->sets_completados_total + sistema->sets_en_proceso < sistema->config.num_sets) {
                            celda->trabajando_en_set = true;
                            sistema->sets_en_proceso++;
                            ya_trabajando = true;
                            printf("[SISTEMA] Celda %d reservó SET. En proceso: %d, Completados: %d\n", 
                                   c+1, sistema->sets_en_proceso, sistema->sets_completados_total);
                        }
                        
                        pthread_mutex_unlock(&celda->mutex);
                        pthread_mutex_unlock(&sistema->mutex_sets);
                    }
                    
                    if (ya_trabajando) {
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
                        
                        // ============= FASE 2: COLOCAR EN LA CAJA =============
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
                            
                            // Resetear contador de ciclos sin progreso
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
                                printf("[CELDA %d][BRAZO %d] ★ ¡SET COMPLETO! Notificando operador...\n",
                                       c+1, b+1);
                                
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
        
        // ============= FASE 3: USAR PIEZAS DEL BUFFER =============
        if (ya_trabajando && estado_celda == CELDA_ACTIVA && hay_pieza_en_buffer(celda, &celda->caja)) {
            sem_wait(&celda->caja.sem_acceso);
            pthread_mutex_lock(&celda->caja.mutex);
            
            for (int tipo = 1; tipo <= MAX_TIPOS_PIEZA; tipo++) {
                if (celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                    Pieza p = sacar_del_buffer(celda, tipo);
                    if (p.tipo > 0) {
                        celda->caja.piezas_por_tipo[tipo - 1]++;
                        brazo->piezas_movidas++;
                        
                        // Resetear contador de ciclos sin progreso
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
                            printf("[CELDA %d][BRAZO %d] ★ ¡SET COMPLETO! Notificando operador...\n",
                                   c+1, b+1);
                            
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
        
        // ============= FASE 4: VERIFICAR SI DEBEMOS LIBERAR PIEZAS =============
        // Solo el brazo 1 hace esta verificación para evitar múltiples devoluciones
        if (b == 0 && ya_trabajando && estado_celda == CELDA_ACTIVA) {
            // Incrementar contador de ciclos sin progreso
            pthread_mutex_lock(&celda->mutex);
            celda->ciclos_sin_progreso++;
            int ciclos = celda->ciclos_sin_progreso;
            pthread_mutex_unlock(&celda->mutex);
            
            // Después de ~1 segundo (100 ciclos * 10ms), verificar si podemos completar
            // Reducido de 200 a 100 para ser más reactivo
            if (ciclos > 100) {
                // Contar piezas que me faltan
                int piezas_faltan = 0;
                int piezas_disponibles = 0;
                
                pthread_mutex_lock(&celda->caja.mutex);
                for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
                    int faltan_tipo = celda->caja.piezas_necesarias[t] - celda->caja.piezas_por_tipo[t];
                    if (faltan_tipo > 0) {
                        piezas_faltan += faltan_tipo;
                    }
                }
                pthread_mutex_unlock(&celda->caja.mutex);
                
                // Contar piezas en mi buffer
                pthread_mutex_lock(&celda->buffer_mutex);
                piezas_disponibles += celda->buffer_count;
                pthread_mutex_unlock(&celda->buffer_mutex);
                
                // Contar piezas en la banda que aún no han pasado mi posición
                for (int i = 0; i <= celda->posicion_banda; i++) {
                    PosicionBanda *pos = &sistema->banda.posiciones[i];
                    pthread_mutex_lock(&pos->mutex);
                    piezas_disponibles += pos->num_piezas;
                    pthread_mutex_unlock(&pos->mutex);
                }
                
                // Si no hay suficientes piezas para completar el SET y NO soy la última celda
                bool es_ultima_celda = (c == sistema->config.num_celdas - 1);
                
                if (piezas_disponibles < piezas_faltan && !es_ultima_celda) {
                    printf("[CELDA %d] No hay suficientes piezas (disponibles: %d, faltan: %d). Liberando para siguiente celda.\n",
                           c+1, piezas_disponibles, piezas_faltan);
                    devolver_piezas_a_banda(celda);
                    ya_trabajando = false;
                } else {
                    // Resetear el contador si aún hay esperanza
                    pthread_mutex_lock(&celda->mutex);
                    celda->ciclos_sin_progreso = 0;
                    pthread_mutex_unlock(&celda->mutex);
                }
            }
        }
        
        usleep(10000);
    }
    
    printf("[CELDA %d][BRAZO %d] Terminado\n", c+1, b+1);
    return NULL;
}
