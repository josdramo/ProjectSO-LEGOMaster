/**
 * LEGO Master - Archivo de definiciones comunes
 * 
 * Define estructuras de datos compartidas entre procesos:
 * - Piezas y tipos
 * - Banda transportadora
 * - Celdas de empaquetado
 * - Brazos robóticos
 * - Estadísticas
 */

#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

// Configuración del sistema
#define MAX_TIPOS_PIEZA     4       // Tipos de piezas: A, B, C, D
#define MAX_POSICIONES      100     // Posiciones en la banda
#define MAX_PIEZAS_POS      10      // Máximo de piezas por posición
#define MAX_CELDAS          4       // Máximo de celdas de empaquetado
#define BRAZOS_POR_CELDA    4       // Brazos robóticos por celda
#define MAX_BRAZOS_ACTIVOS  2       // Máx brazos retirando piezas simultáneamente
#define MAX_BUFFER_CELDA    20      // Buffer de piezas esperando en celda

// Keys para memoria compartida
#define SHM_KEY_BANDA       2222
#define SHM_KEY_CELDAS      2223
#define SHM_KEY_CONFIG      2224
#define SHM_KEY_STATS       2225

// Semáforos nombrados
#define SEM_BANDA           "/lego_banda"
#define SEM_CELDA_PREFIX    "/lego_celda_"
#define SEM_CAJA_PREFIX     "/lego_caja_"
#define SEM_DISPENSADOR     "/lego_dispensador"

// Estados de los brazos robóticos
typedef enum {
    BRAZO_IDLE,             // Esperando
    BRAZO_RETIRANDO,        // Retirando pieza de la banda
    BRAZO_COLOCANDO,        // Colocando pieza en la caja
    BRAZO_SUSPENDIDO        // Suspendido por balanceo de carga
} EstadoBrazo;

// Estados de la celda
typedef enum {
    CELDA_ACTIVA,           // Operando normalmente
    CELDA_ESPERANDO_OP,     // Esperando al operador humano
    CELDA_INACTIVA          // Fuera de operación
} EstadoCelda;

// Representación de una pieza
typedef struct {
    int tipo;               // Tipo de pieza (1-4, 0 = vacío)
    int id_unico;           // ID único para tracking
} Pieza;

// Posición en la banda transportadora
typedef struct {
    Pieza piezas[MAX_PIEZAS_POS];   // Piezas en esta posición
    int num_piezas;                  // Cantidad de piezas actual
    pthread_mutex_t mutex;           // Mutex para acceso exclusivo
} PosicionBanda;

// Banda transportadora completa
typedef struct {
    PosicionBanda posiciones[MAX_POSICIONES];
    int longitud;                    // N - longitud real de la banda
    int velocidad;                   // v - pasos por segundo
    bool activa;                     // Si la banda está en operación
    pthread_mutex_t mutex_global;    // Para operaciones globales
} BandaTransportadora;

// Brazo robótico
typedef struct {
    int id;
    int celda_id;
    EstadoBrazo estado;
    int piezas_movidas;              // Total de piezas movidas
    Pieza pieza_actual;              // Pieza que está manipulando
    pthread_mutex_t mutex;
    time_t tiempo_suspension;        // Cuándo fue suspendido
} BrazoRobotico;

// Caja de empaquetado
typedef struct {
    int piezas_por_tipo[MAX_TIPOS_PIEZA];   // Piezas actuales por tipo
    int piezas_necesarias[MAX_TIPOS_PIEZA]; // Piezas requeridas por tipo
    bool completa;                           // Si el SET está completo
    pthread_mutex_t mutex;                   // Mutex para acceso a la caja
    sem_t sem_acceso;                        // Solo 1 brazo coloca a la vez
} CajaEmpaquetado;

// Celda de empaquetado
typedef struct {
    int id;
    int posicion_banda;              // xi - posición en la banda
    EstadoCelda estado;
    BrazoRobotico brazos[BRAZOS_POR_CELDA];
    CajaEmpaquetado caja;
    sem_t sem_brazos_retirando;      // Controla máx 2 brazos retirando
    pthread_mutex_t mutex;
    int cajas_completadas_ok;
    int cajas_completadas_fail;
    bool trabajando_en_set;          // Si ya tomó piezas para un SET
    // Buffer de piezas retiradas esperando a ser colocadas
    Pieza buffer[MAX_BUFFER_CELDA];
    int buffer_count;
    pthread_mutex_t buffer_mutex;
    // Control de tiempo sin progreso (para devolución de piezas)
    time_t ultimo_progreso;          // Última vez que se colocó una pieza
    int ciclos_sin_progreso;         // Contador de ciclos sin avance
} CeldaEmpaquetado;

// Configuración del sistema
typedef struct {
    int num_dispensadores;
    int num_celdas;
    int num_sets;
    int piezas_por_tipo[MAX_TIPOS_PIEZA];   // Ci - piezas de cada tipo por SET
    int longitud_banda;              // N
    int velocidad_banda;             // v (pasos/segundo)
    int delta_t1_max;                // Máx tiempo operador (ms)
    int delta_t2;                    // Tiempo suspensión brazo (ms)
    int Y;                           // Piezas para trigger de balanceo
    int posiciones_celdas[MAX_CELDAS];      // Posiciones xi
    bool sistema_activo;
} ConfiguracionSistema;

// Estadísticas globales
typedef struct {
    int total_piezas_dispensadas;
    int piezas_en_tacho[MAX_TIPOS_PIEZA];   // Piezas sobrantes por tipo
    int total_piezas_tacho;
    int cajas_ok;
    int cajas_fail;
    int piezas_por_brazo[MAX_CELDAS][BRAZOS_POR_CELDA];
    pthread_mutex_t mutex;
} Estadisticas;

// Estructura principal del sistema compartido
typedef struct {
    ConfiguracionSistema config;
    BandaTransportadora banda;
    CeldaEmpaquetado celdas[MAX_CELDAS];
    Estadisticas stats;
    int piezas_dispensadas_ciclo;    // Para trigger de balanceo cada Y piezas
    bool terminar;                   // Flag para terminar simulación
    // Control de SETs completados
    int sets_en_proceso;             // SETs que están siendo llenados actualmente
    int sets_completados_total;      // Total de SETs completados (OK + pendientes de confirmar)
    pthread_mutex_t mutex_sets;      // Mutex para control de sets
    // Control de turno de celdas
    int celda_activa;                // Índice de la celda que tiene el turno (-1 = ninguna)
} SistemaLego;

// Funciones de utilidad
const char* nombre_tipo_pieza(int tipo);
void imprimir_estadisticas(Estadisticas *stats, ConfiguracionSistema *config);
void imprimir_estado_banda(BandaTransportadora *banda, int desde, int hasta);
void imprimir_estado_celda(CeldaEmpaquetado *celda);

#endif // COMMON_H
