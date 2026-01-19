/**
 * LEGO Master - Funciones de utilidad
 */

#include "common.h"
#include <stdio.h>
#include <string.h>

const char* nombre_tipo_pieza(int tipo) {
    static const char* nombres[] = {"VACIO", "A", "B", "C", "D"};
    if (tipo >= 0 && tipo <= MAX_TIPOS_PIEZA) {
        return nombres[tipo];
    }
    return "?";
}

void imprimir_estadisticas(Estadisticas *stats, ConfiguracionSistema *config) {
    pthread_mutex_lock(&stats->mutex);
    
    // Calcular piezas esperadas vs usadas
    int piezas_esperadas = 0;
    for (int t = 0; t < MAX_TIPOS_PIEZA; t++) {
        piezas_esperadas += config->piezas_por_tipo[t] * config->num_sets;
    }
    int piezas_en_cajas = stats->cajas_ok * (config->piezas_por_tipo[0] + 
                                              config->piezas_por_tipo[1] +
                                              config->piezas_por_tipo[2] +
                                              config->piezas_por_tipo[3]);
    (void)piezas_esperadas; // Suprimir warning si no se usa
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║                   RESUMEN FINAL DE OPERACIÓN                      ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║ Cajas completadas correctamente (OK):     %4d                     ║\n", stats->cajas_ok);
    printf("║ Cajas completadas incorrectamente (FAIL): %4d                     ║\n", stats->cajas_fail);
    printf("║ SETs esperados:                           %4d                     ║\n", config->num_sets);
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║                    BALANCE DE PIEZAS                              ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║ Total piezas dispensadas:                 %4d                     ║\n", stats->total_piezas_dispensadas);
    printf("║ Piezas en cajas OK:                       %4d                     ║\n", piezas_en_cajas);
    printf("║ Piezas en tacho (sobrantes):              %4d                     ║\n", stats->total_piezas_tacho);
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║                  PIEZAS SOBRANTES POR TIPO                        ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    
    for (int i = 0; i < MAX_TIPOS_PIEZA; i++) {
        printf("║   Tipo %s: %4d piezas                                            ║\n", 
               nombre_tipo_pieza(i+1), stats->piezas_en_tacho[i]);
    }
    
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║                 PIEZAS MOVIDAS POR BRAZO                          ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    
    for (int c = 0; c < config->num_celdas; c++) {
        printf("║ Celda %d:                                                          ║\n", c+1);
        for (int b = 0; b < BRAZOS_POR_CELDA; b++) {
            printf("║   Brazo %d: %4d piezas                                            ║\n", 
                   b+1, stats->piezas_por_brazo[c][b]);
        }
    }
    
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    printf("║                       CONCLUSIÓN                                  ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════╣\n");
    
    if (stats->cajas_ok == config->num_sets && stats->total_piezas_tacho == 0) {
        printf("║ ✓ ÉXITO TOTAL: Todos los SETs completados sin piezas sobrantes   ║\n");
    } else if (stats->cajas_ok == config->num_sets && stats->total_piezas_tacho > 0) {
        printf("║ ⚠ ADVERTENCIA: SETs completados pero hay piezas sobrantes        ║\n");
        printf("║   Esto indica que se dispensaron piezas de más o                 ║\n");
        printf("║   los brazos no alcanzaron a retirar todas las piezas.           ║\n");
    } else if (stats->cajas_ok < config->num_sets) {
        printf("║ ✗ INCOMPLETO: No se completaron todos los SETs esperados         ║\n");
        printf("║   Completados: %d de %d                                           ║\n",
               stats->cajas_ok, config->num_sets);
        if (stats->total_piezas_tacho > 0) {
            printf("║   Las piezas sobrantes no llegaron a tiempo a las celdas        ║\n");
        }
    }
    
    if (stats->cajas_fail > 0) {
        printf("║ ✗ ERRORES: %d cajas tuvieron contenido incorrecto                 ║\n",
               stats->cajas_fail);
    }
    
    printf("╚═══════════════════════════════════════════════════════════════════╝\n");
    
    pthread_mutex_unlock(&stats->mutex);
}

void imprimir_estado_banda(BandaTransportadora *banda, int desde, int hasta) {
    printf("\nEstado de la banda [%d - %d]:\n", desde, hasta);
    printf("Pos: ");
    for (int i = desde; i <= hasta && i < banda->longitud; i++) {
        printf("%3d ", i);
    }
    printf("\n");
    printf("     ");
    for (int i = desde; i <= hasta && i < banda->longitud; i++) {
        pthread_mutex_lock(&banda->posiciones[i].mutex);
        int n = banda->posiciones[i].num_piezas;
        pthread_mutex_unlock(&banda->posiciones[i].mutex);
        if (n > 0) {
            printf("[%d] ", n);
        } else {
            printf(" .  ");
        }
    }
    printf("\n");
}

void imprimir_estado_celda(CeldaEmpaquetado *celda) {
    pthread_mutex_lock(&celda->mutex);
    
    printf("\n--- Celda %d (pos %d) ---\n", celda->id + 1, celda->posicion_banda);
    printf("Estado: ");
    switch (celda->estado) {
        case CELDA_ACTIVA: printf("ACTIVA\n"); break;
        case CELDA_ESPERANDO_OP: printf("ESPERANDO OPERADOR\n"); break;
        case CELDA_INACTIVA: printf("INACTIVA\n"); break;
    }
    
    pthread_mutex_lock(&celda->caja.mutex);
    printf("Caja: ");
    for (int i = 0; i < MAX_TIPOS_PIEZA; i++) {
        printf("%s:%d/%d ", nombre_tipo_pieza(i+1), 
               celda->caja.piezas_por_tipo[i],
               celda->caja.piezas_necesarias[i]);
    }
    printf("\n");
    pthread_mutex_unlock(&celda->caja.mutex);
    
    printf("Brazos: ");
    for (int i = 0; i < BRAZOS_POR_CELDA; i++) {
        printf("[%d:", i+1);
        switch (celda->brazos[i].estado) {
            case BRAZO_IDLE: printf("I"); break;
            case BRAZO_RETIRANDO: printf("R"); break;
            case BRAZO_COLOCANDO: printf("C"); break;
            case BRAZO_SUSPENDIDO: printf("S"); break;
        }
        printf("/%d] ", celda->brazos[i].piezas_movidas);
    }
    printf("\n");
    
    pthread_mutex_unlock(&celda->mutex);
}
