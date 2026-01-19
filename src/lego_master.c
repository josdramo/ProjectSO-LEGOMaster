/**
 * LEGO Master - Sistema Principal de Simulación
 * 
 * Este programa orquesta la simulación completa:
 * - Inicializa memoria compartida y semáforos
 * - Crea hilos para: banda, dispensadores, celdas, brazos
 * - Coordina la sincronización entre componentes
 * - Genera estadísticas finales
 * 
 * Compilar: gcc -o lego_master lego_master.c utils.c -lpthread -lrt
 * Ejecutar: ./lego_master <dispensadores> <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#include "common.h"

// Sistema global
SistemaLego *sistema = NULL;
int id_pieza_global = 0;
pthread_mutex_t mutex_id_pieza = PTHREAD_MUTEX_INITIALIZER;

// Semáforos
sem_t *sem_dispensador = NULL;

// Hilos
pthread_t hilo_banda;
pthread_t hilos_dispensadores;
pthread_t hilos_celdas[MAX_CELDAS];
pthread_t hilos_brazos[MAX_CELDAS][BRAZOS_POR_CELDA];

// Prototipos
void* thread_banda(void* arg);
void* thread_dispensador(void* arg);
void* thread_celda(void* arg);
void* thread_brazo(void* arg);
void inicializar_sistema(int argc, char* argv[]);
void limpiar_recursos();
void manejador_senal(int sig);
int generar_id_pieza();
bool verificar_caja_completa(CajaEmpaquetado *caja);
void notificar_operador(CeldaEmpaquetado *celda);
int encontrar_brazo_max_piezas(CeldaEmpaquetado *celda);

// ============================================================================
// INICIALIZACIÓN DEL SISTEMA
// ============================================================================

void inicializar_sistema(int argc, char* argv[]) {
    if (argc < 10) {
        fprintf(stderr, "Uso: %s <dispensadores> <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>\n", argv[0]);
        fprintf(stderr, "  dispensadores: número de dispensadores\n");
        fprintf(stderr, "  celdas: número de celdas de empaquetado (1-%d)\n", MAX_CELDAS);
        fprintf(stderr, "  sets: número de sets a completar\n");
        fprintf(stderr, "  pA-pD: piezas de cada tipo por set\n");
        fprintf(stderr, "  velocidad: pasos/segundo de la banda\n");
        fprintf(stderr, "  longitud: longitud de la banda (posiciones)\n");
        exit(1);
    }

    // Asignar memoria para el sistema
    sistema = (SistemaLego*)calloc(1, sizeof(SistemaLego));
    if (!sistema) {
        perror("Error asignando memoria");
        exit(1);
    }

    // Configuración desde argumentos
    sistema->config.num_dispensadores = atoi(argv[1]);
    sistema->config.num_celdas = atoi(argv[2]);
    sistema->config.num_sets = atoi(argv[3]);
    sistema->config.piezas_por_tipo[0] = atoi(argv[4]);
    sistema->config.piezas_por_tipo[1] = atoi(argv[5]);
    sistema->config.piezas_por_tipo[2] = atoi(argv[6]);
    sistema->config.piezas_por_tipo[3] = atoi(argv[7]);
    sistema->config.velocidad_banda = atoi(argv[8]);
    sistema->config.longitud_banda = atoi(argv[9]);
    
    // Valores por defecto
    sistema->config.delta_t1_max = 2000;  // máx 2 segundos para operador
    sistema->config.delta_t2 = 1000;       // 1 segundo suspensión
    sistema->config.Y = 10;                // balanceo cada 10 piezas
    sistema->config.sistema_activo = true;

    // Validaciones
    if (sistema->config.num_celdas > MAX_CELDAS) {
        sistema->config.num_celdas = MAX_CELDAS;
    }
    if (sistema->config.longitud_banda > MAX_POSICIONES) {
        sistema->config.longitud_banda = MAX_POSICIONES;
    }

    // Calcular posiciones de las celdas (distribuidas uniformemente)
    int intervalo = sistema->config.longitud_banda / (sistema->config.num_celdas + 1);
    for (int i = 0; i < sistema->config.num_celdas; i++) {
        sistema->config.posiciones_celdas[i] = (i + 1) * intervalo;
    }

    // Inicializar banda
    sistema->banda.longitud = sistema->config.longitud_banda;
    sistema->banda.velocidad = sistema->config.velocidad_banda;
    sistema->banda.activa = true;
    pthread_mutex_init(&sistema->banda.mutex_global, NULL);

    for (int i = 0; i < sistema->banda.longitud; i++) {
        sistema->banda.posiciones[i].num_piezas = 0;
        pthread_mutex_init(&sistema->banda.posiciones[i].mutex, NULL);
        for (int j = 0; j < MAX_PIEZAS_POS; j++) {
            sistema->banda.posiciones[i].piezas[j].tipo = 0;
            sistema->banda.posiciones[i].piezas[j].id_unico = 0;
        }
    }

    // Inicializar celdas
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        sistema->celdas[c].id = c;
        sistema->celdas[c].posicion_banda = sistema->config.posiciones_celdas[c];
        sistema->celdas[c].estado = CELDA_ACTIVA;
        sistema->celdas[c].cajas_completadas_ok = 0;
        sistema->celdas[c].cajas_completadas_fail = 0;
        pthread_mutex_init(&sistema->celdas[c].mutex, NULL);
        
        // Semáforo para limitar brazos retirando (máx 2)
        sem_init(&sistema->celdas[c].sem_brazos_retirando, 0, MAX_BRAZOS_ACTIVOS);
        
        // Inicializar caja
        pthread_mutex_init(&sistema->celdas[c].caja.mutex, NULL);
        sem_init(&sistema->celdas[c].caja.sem_acceso, 0, 1);  // solo 1 coloca
        sistema->celdas[c].caja.completa = false;
        for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
            sistema->celdas[c].caja.piezas_por_tipo[t] = 0;
            sistema->celdas[c].caja.piezas_necesarias[t] = sistema->config.piezas_por_tipo[t];
        }
        
        // Inicializar brazos
        for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
            sistema->celdas[c].brazos[b].id = b;
            sistema->celdas[c].brazos[b].celda_id = c;
            sistema->celdas[c].brazos[b].estado = BRAZO_IDLE;
            sistema->celdas[c].brazos[b].piezas_movidas = 0;
            sistema->celdas[c].brazos[b].pieza_actual.tipo = 0;
            pthread_mutex_init(&sistema->celdas[c].brazos[b].mutex, NULL);
        }
    }

    // Inicializar estadísticas
    pthread_mutex_init(&sistema->stats.mutex, NULL);
    sistema->stats.total_piezas_dispensadas = 0;
    sistema->stats.total_piezas_tacho = 0;
    sistema->stats.cajas_ok = 0;
    sistema->stats.cajas_fail = 0;
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        sistema->stats.piezas_en_tacho[t] = 0;
    }

    sistema->piezas_dispensadas_ciclo = 0;
    sistema->terminar = false;

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                 LEGO MASTER - SIMULACIÓN                     ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║ Configuración:                                               ║\n");
    printf("║   Dispensadores: %d                                          ║\n", sistema->config.num_dispensadores);
    printf("║   Celdas: %d                                                  ║\n", sistema->config.num_celdas);
    printf("║   Sets a completar: %d                                       ║\n", sistema->config.num_sets);
    printf("║   Piezas por SET: A=%d, B=%d, C=%d, D=%d                      ║\n",
           sistema->config.piezas_por_tipo[0], sistema->config.piezas_por_tipo[1],
           sistema->config.piezas_por_tipo[2], sistema->config.piezas_por_tipo[3]);
    printf("║   Longitud banda: %d posiciones                              ║\n", sistema->config.longitud_banda);
    printf("║   Velocidad: %d pasos/segundo                                ║\n", sistema->config.velocidad_banda);
    printf("║   Posiciones celdas: ");
    for (int i = 0; i < sistema->config.num_celdas; i++) {
        printf("%d ", sistema->config.posiciones_celdas[i]);
    }
    printf("                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");
}

// ============================================================================
// FUNCIONES AUXILIARES
// ============================================================================

int generar_id_pieza() {
    pthread_mutex_lock(&mutex_id_pieza);
    int id = ++id_pieza_global;
    pthread_mutex_unlock(&mutex_id_pieza);
    return id;
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
    if (caja->completa) return false;  // Ya está completa
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
        printf("[CELDA %d] Operador: OK - Caja #%d completada correctamente\n", 
               celda->id + 1, sistema->stats.cajas_ok);
    } else {
        sistema->stats.cajas_fail++;
        celda->cajas_completadas_fail++;
        printf("[CELDA %d] Operador: FAIL - Caja incorrecta\n", celda->id + 1);
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

// ============================================================================
// HILO DE LA BANDA TRANSPORTADORA
// ============================================================================

void* thread_banda(void* arg) {
    int intervalo_us = 1000000 / sistema->banda.velocidad;
    
    printf("[BANDA] Iniciada - velocidad: %d pasos/seg, longitud: %d\n",
           sistema->banda.velocidad, sistema->banda.longitud);
    
    while (!sistema->terminar) {
        usleep(intervalo_us);
        
        pthread_mutex_lock(&sistema->banda.mutex_global);
        
        // Mover piezas desde el final hacia el inicio
        // Las piezas en la última posición caen al tacho
        PosicionBanda *ultima = &sistema->banda.posiciones[sistema->banda.longitud - 1];
        pthread_mutex_lock(&ultima->mutex);
        
        for (int p = 0; p < ultima->num_piezas; p++) {
            if (ultima->piezas[p].tipo > 0) {
                int tipo = ultima->piezas[p].tipo - 1;
                pthread_mutex_lock(&sistema->stats.mutex);
                sistema->stats.piezas_en_tacho[tipo]++;
                sistema->stats.total_piezas_tacho++;
                pthread_mutex_unlock(&sistema->stats.mutex);
                printf("[BANDA] Pieza tipo %s cayó al tacho\n", 
                       nombre_tipo_pieza(ultima->piezas[p].tipo));
            }
        }
        ultima->num_piezas = 0;
        pthread_mutex_unlock(&ultima->mutex);
        
        // Mover todas las demás piezas una posición
        for (int i = sistema->banda.longitud - 1; i > 0; i--) {
            PosicionBanda *actual = &sistema->banda.posiciones[i];
            PosicionBanda *anterior = &sistema->banda.posiciones[i - 1];
            
            pthread_mutex_lock(&actual->mutex);
            pthread_mutex_lock(&anterior->mutex);
            
            // Copiar piezas de la posición anterior a la actual
            actual->num_piezas = anterior->num_piezas;
            for (int p = 0; p < anterior->num_piezas; p++) {
                actual->piezas[p] = anterior->piezas[p];
            }
            anterior->num_piezas = 0;
            
            pthread_mutex_unlock(&anterior->mutex);
            pthread_mutex_unlock(&actual->mutex);
        }
        
        pthread_mutex_unlock(&sistema->banda.mutex_global);
    }
    
    printf("[BANDA] Terminada\n");
    return NULL;
}

// ============================================================================
// HILO DE LOS DISPENSADORES
// ============================================================================

void* thread_dispensador(void* arg) {
    int total_piezas = 0;
    int piezas_restantes[MAX_TIPOS_PIEZA];
    
    // Calcular total de piezas a dispensar
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        piezas_restantes[t] = sistema->config.piezas_por_tipo[t] * sistema->config.num_sets;
        total_piezas += piezas_restantes[t];
    }
    
    printf("[DISPENSADORES] Iniciados - total piezas: %d\n", total_piezas);
    
    int intervalo_us = 1000000 / sistema->banda.velocidad / 2;  // Dispensar más rápido que banda
    
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
                    
                    // printf("[DISP %d] Pieza tipo %s dispensada\n", d+1, nombre_tipo_pieza(tipo+1));
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
    
    printf("[DISPENSADORES] Terminados - todas las piezas dispensadas\n");
    
    // Esperar a que la banda se vacíe
    sleep((sistema->banda.longitud / sistema->banda.velocidad) + 2);
    sistema->terminar = true;
    
    return NULL;
}

// ============================================================================
// HILO DE BRAZO ROBÓTICO
// ============================================================================

typedef struct {
    int celda_id;
    int brazo_id;
} ArgsBrazo;

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
                usleep(100000);  // Esperar 100ms
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
            // Acceder a la posición de la banda
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
                
                // Simular tiempo de retirar
                usleep(50000);  // 50ms
                
                // Colocar en la caja (solo 1 brazo a la vez)
                sem_wait(&celda->caja.sem_acceso);
                
                pthread_mutex_lock(&brazo->mutex);
                brazo->estado = BRAZO_COLOCANDO;
                pthread_mutex_unlock(&brazo->mutex);
                
                pthread_mutex_lock(&celda->caja.mutex);
                
                int tipo = brazo->pieza_actual.tipo;
                // Verificar de nuevo si necesitamos esta pieza (otra thread pudo haberla colocado)
                if (tipo > 0 && tipo <= MAX_TIPOS_PIEZA && 
                    !celda->caja.completa &&
                    celda->caja.piezas_por_tipo[tipo - 1] < celda->caja.piezas_necesarias[tipo - 1]) {
                    
                    celda->caja.piezas_por_tipo[tipo - 1]++;
                    brazo->piezas_movidas++;
                    
                    // Actualizar estadísticas
                    pthread_mutex_lock(&sistema->stats.mutex);
                    sistema->stats.piezas_por_brazo[c][b]++;
                    pthread_mutex_unlock(&sistema->stats.mutex);
                    
                    printf("[CELDA %d][BRAZO %d] Colocó pieza tipo %s en caja [%d/%d]\n",
                           c+1, b+1, nombre_tipo_pieza(tipo),
                           celda->caja.piezas_por_tipo[tipo - 1],
                           celda->caja.piezas_necesarias[tipo - 1]);
                    
                    // Verificar si el SET está completo
                    if (verificar_caja_completa(&celda->caja)) {
                        celda->caja.completa = true;
                        printf("[CELDA %d][BRAZO %d] ¡SET COMPLETO! Notificando operador...\n",
                               c+1, b+1);
                        
                        pthread_mutex_unlock(&celda->caja.mutex);
                        sem_post(&celda->caja.sem_acceso);
                        
                        // Suspender celda y notificar operador
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
                    // para que siga a la siguiente celda
                    printf("[CELDA %d][BRAZO %d] Pieza tipo %s ya no necesaria, devuelta a banda\n",
                           c+1, b+1, nombre_tipo_pieza(tipo));
                    
                    // Devolver a la banda
                    PosicionBanda *pos_devolver = &sistema->banda.posiciones[celda->posicion_banda];
                    pthread_mutex_lock(&pos_devolver->mutex);
                    if (pos_devolver->num_piezas < MAX_PIEZAS_POS) {
                        pos_devolver->piezas[pos_devolver->num_piezas] = brazo->pieza_actual;
                        pos_devolver->num_piezas++;
                    }
                    pthread_mutex_unlock(&pos_devolver->mutex);
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
        
        usleep(10000);  // Pequeña pausa entre intentos
    }
    
    printf("[CELDA %d][BRAZO %d] Terminado\n", c+1, b+1);
    return NULL;
}

// ============================================================================
// LIMPIEZA DE RECURSOS
// ============================================================================

void limpiar_recursos() {
    if (sistema) {
        // Destruir mutex de banda
        pthread_mutex_destroy(&sistema->banda.mutex_global);
        for (int i = 0; i < sistema->banda.longitud; i++) {
            pthread_mutex_destroy(&sistema->banda.posiciones[i].mutex);
        }
        
        // Destruir recursos de celdas
        for (int c = 0; c < sistema->config.num_celdas; c++) {
            pthread_mutex_destroy(&sistema->celdas[c].mutex);
            pthread_mutex_destroy(&sistema->celdas[c].caja.mutex);
            sem_destroy(&sistema->celdas[c].caja.sem_acceso);
            sem_destroy(&sistema->celdas[c].sem_brazos_retirando);
            
            for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
                pthread_mutex_destroy(&sistema->celdas[c].brazos[b].mutex);
            }
        }
        
        pthread_mutex_destroy(&sistema->stats.mutex);
        free(sistema);
    }
}

void manejador_senal(int sig) {
    printf("\n\nSeñal recibida. Terminando simulación...\n");
    if (sistema) {
        sistema->terminar = true;
    }
}

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

int main(int argc, char *argv[]) {
    srand(time(NULL));
    
    // Configurar manejador de señales
    signal(SIGINT, manejador_senal);
    signal(SIGTERM, manejador_senal);
    
    // Inicializar sistema
    inicializar_sistema(argc, argv);
    
    // Crear hilo de la banda
    if (pthread_create(&hilo_banda, NULL, thread_banda, NULL) != 0) {
        perror("Error creando hilo banda");
        exit(1);
    }
    
    // Crear hilos de brazos robóticos
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
            ArgsBrazo *args = malloc(sizeof(ArgsBrazo));
            args->celda_id = c;
            args->brazo_id = b;
            
            if (pthread_create(&hilos_brazos[c][b], NULL, thread_brazo, args) != 0) {
                perror("Error creando hilo brazo");
                exit(1);
            }
        }
    }
    
    // Crear hilo de dispensadores
    if (pthread_create(&hilos_dispensadores, NULL, thread_dispensador, NULL) != 0) {
        perror("Error creando hilo dispensadores");
        exit(1);
    }
    
    // Esperar a que terminen los hilos
    pthread_join(hilos_dispensadores, NULL);
    
    // Señalizar terminación
    sistema->terminar = true;
    
    pthread_join(hilo_banda, NULL);
    
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
            pthread_join(hilos_brazos[c][b], NULL);
        }
    }
    
    // Imprimir estadísticas finales
    imprimir_estadisticas(&sistema->stats, &sistema->config);
    
    // Limpiar recursos
    limpiar_recursos();
    
    return 0;
}
