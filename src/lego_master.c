/**
 * LEGO Master - Sistema Principal de Simulación
 * 
 * Programa principal que orquesta la simulación:
 * - Inicializa el sistema y sus componentes
 * - Crea hilos para banda, dispensadores y brazos
 * - Coordina la terminación y muestra estadísticas
 * 
 * Compilar: make all
 * Ejecutar: ./build/lego_master <args...>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>

#include "common.h"
#include "banda.h"
#include "dispensador.h"
#include "celda.h"

// Sistema global (accesible desde otros módulos)
SistemaLego *sistema = NULL;

// Hilos
static pthread_t hilo_banda;
static pthread_t hilo_dispensadores;
static pthread_t hilos_brazos[MAX_CELDAS][BRAZOS_POR_CELDA];

// Prototipos locales
static void inicializar_sistema(int argc, char* argv[]);
static void limpiar_recursos(void);
static void manejador_senal(int sig);

// ============================================================================
// INICIALIZACIÓN DEL SISTEMA
// ============================================================================

static void mostrar_uso(const char* programa) {
    fprintf(stderr, "Uso: %s <dispensadores> <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>\n\n", programa);
    fprintf(stderr, "Parámetros:\n");
    fprintf(stderr, "  dispensadores : Número de dispensadores de piezas\n");
    fprintf(stderr, "  celdas        : Número de celdas de empaquetado (1-%d)\n", MAX_CELDAS);
    fprintf(stderr, "  sets          : Número de SETs a completar\n");
    fprintf(stderr, "  pA, pB, pC, pD: Piezas de cada tipo por SET\n");
    fprintf(stderr, "  velocidad     : Velocidad de la banda (pasos/segundo)\n");
    fprintf(stderr, "  longitud      : Longitud de la banda (posiciones)\n");
    fprintf(stderr, "\nEjemplo:\n");
    fprintf(stderr, "  %s 4 2 3 3 2 2 1 3 25\n", programa);
}

static void inicializar_sistema(int argc, char* argv[]) {
    if (argc < 10) {
        mostrar_uso(argv[0]);
        exit(1);
    }

    // Asignar memoria para el sistema
    sistema = (SistemaLego*)calloc(1, sizeof(SistemaLego));
    if (!sistema) {
        perror("Error asignando memoria para el sistema");
        exit(1);
    }

    // Leer configuración desde argumentos
    sistema->config.num_dispensadores = atoi(argv[1]);
    sistema->config.num_celdas = atoi(argv[2]);
    sistema->config.num_sets = atoi(argv[3]);
    sistema->config.piezas_por_tipo[0] = atoi(argv[4]);
    sistema->config.piezas_por_tipo[1] = atoi(argv[5]);
    sistema->config.piezas_por_tipo[2] = atoi(argv[6]);
    sistema->config.piezas_por_tipo[3] = atoi(argv[7]);
    sistema->config.velocidad_banda = atoi(argv[8]);
    sistema->config.longitud_banda = atoi(argv[9]);
    
    // Valores por defecto para parámetros opcionales
    sistema->config.delta_t1_max = 2000;  // máx 2 segundos para operador
    sistema->config.delta_t2 = 1000;       // 1 segundo suspensión brazo
    sistema->config.Y = 10;                // balanceo cada 10 piezas
    sistema->config.sistema_activo = true;

    // Validaciones
    if (sistema->config.num_celdas > MAX_CELDAS) {
        fprintf(stderr, "Advertencia: Máximo %d celdas, ajustando...\n", MAX_CELDAS);
        sistema->config.num_celdas = MAX_CELDAS;
    }
    if (sistema->config.longitud_banda > MAX_POSICIONES) {
        fprintf(stderr, "Advertencia: Máximo %d posiciones, ajustando...\n", MAX_POSICIONES);
        sistema->config.longitud_banda = MAX_POSICIONES;
    }
    if (sistema->config.num_dispensadores <= 0 || sistema->config.num_sets <= 0) {
        fprintf(stderr, "Error: Dispensadores y sets deben ser > 0\n");
        exit(1);
    }

    // Calcular posiciones de las celdas (distribuidas uniformemente)
    int intervalo = sistema->config.longitud_banda / (sistema->config.num_celdas + 1);
    for (int i = 0; i < sistema->config.num_celdas; i++) {
        sistema->config.posiciones_celdas[i] = (i + 1) * intervalo;
    }

    // Inicializar banda transportadora
    inicializar_banda(&sistema->banda, 
                      sistema->config.longitud_banda, 
                      sistema->config.velocidad_banda);

    // Inicializar celdas de empaquetado
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        inicializar_celda(&sistema->celdas[c], c,
                          sistema->config.posiciones_celdas[c],
                          sistema->config.piezas_por_tipo);
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
    for (int c = 0; c < MAX_CELDAS; c++) {
        for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
            sistema->stats.piezas_por_brazo[c][b] = 0;
        }
    }

    sistema->piezas_dispensadas_ciclo = 0;
    sistema->terminar = false;

    // Mostrar configuración
    int total_piezas_set = sistema->config.piezas_por_tipo[0] +
                           sistema->config.piezas_por_tipo[1] +
                           sistema->config.piezas_por_tipo[2] +
                           sistema->config.piezas_por_tipo[3];

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    LEGO MASTER - SIMULACIÓN                       ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║ Configuración:                                                    ║\n");
    printf("║   Dispensadores: %d                                               ║\n", sistema->config.num_dispensadores);
    printf("║   Celdas de empaquetado: %d                                       ║\n", sistema->config.num_celdas);
    printf("║   SETs a completar: %d                                            ║\n", sistema->config.num_sets);
    printf("║   Piezas por SET: A=%d, B=%d, C=%d, D=%d (total=%d)               ║\n",
           sistema->config.piezas_por_tipo[0], sistema->config.piezas_por_tipo[1],
           sistema->config.piezas_por_tipo[2], sistema->config.piezas_por_tipo[3],
           total_piezas_set);
    printf("║   Total piezas a dispensar: %d                                    ║\n", 
           total_piezas_set * sistema->config.num_sets);
    printf("║   Longitud banda: %d posiciones                                   ║\n", sistema->config.longitud_banda);
    printf("║   Velocidad: %d pasos/segundo                                     ║\n", sistema->config.velocidad_banda);
    printf("║   Posiciones celdas: ");
    for (int i = 0; i < sistema->config.num_celdas; i++) {
        printf("%d ", sistema->config.posiciones_celdas[i]);
    }
    printf("                                    ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n\n");
}

// ============================================================================
// LIMPIEZA DE RECURSOS
// ============================================================================

static void limpiar_recursos(void) {
    if (sistema) {
        destruir_banda(&sistema->banda);
        
        for (int c = 0; c < sistema->config.num_celdas; c++) {
            destruir_celda(&sistema->celdas[c]);
        }
        
        pthread_mutex_destroy(&sistema->stats.mutex);
        free(sistema);
        sistema = NULL;
    }
}

static void manejador_senal(int sig) {
    (void)sig;
    printf("\n\n⚠ Señal recibida. Terminando simulación...\n");
    if (sistema) {
        sistema->terminar = true;
    }
}

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

int main(int argc, char *argv[]) {
    srand(time(NULL));
    
    // Configurar manejadores de señales
    signal(SIGINT, manejador_senal);
    signal(SIGTERM, manejador_senal);
    
    // Inicializar sistema
    inicializar_sistema(argc, argv);
    
    printf("Iniciando simulación...\n\n");
    
    // Crear hilo de la banda transportadora
    if (pthread_create(&hilo_banda, NULL, thread_banda, NULL) != 0) {
        perror("Error creando hilo de banda");
        limpiar_recursos();
        exit(1);
    }
    
    // Crear hilos de brazos robóticos
    for (int c = 0; c < sistema->config.num_celdas; c++) {
        for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
            ArgsBrazo *args = malloc(sizeof(ArgsBrazo));
            if (!args) {
                perror("Error asignando memoria para args de brazo");
                sistema->terminar = true;
                break;
            }
            args->celda_id = c;
            args->brazo_id = b;
            
            if (pthread_create(&hilos_brazos[c][b], NULL, thread_brazo, args) != 0) {
                perror("Error creando hilo de brazo");
                free(args);
                sistema->terminar = true;
                break;
            }
        }
    }
    
    // Crear hilo de dispensadores
    if (pthread_create(&hilo_dispensadores, NULL, thread_dispensador, NULL) != 0) {
        perror("Error creando hilo de dispensadores");
        sistema->terminar = true;
    }
    
    // Esperar a que termine el dispensador (controla el fin de la simulación)
    pthread_join(hilo_dispensadores, NULL);
    
    // Asegurar que terminar está en true
    sistema->terminar = true;
    
    // Esperar a los demás hilos
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
