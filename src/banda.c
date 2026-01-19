/**
 * LEGO Master - Implementación de la Banda Transportadora
 */

#include "banda.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Variable externa del sistema (definida en lego_master.c)
extern SistemaLego *sistema;

void inicializar_banda(BandaTransportadora *banda, int longitud, int velocidad) {
    banda->longitud = longitud;
    banda->velocidad = velocidad;
    banda->activa = true;
    pthread_mutex_init(&banda->mutex_global, NULL);
    
    for (int i = 0; i < longitud; i++) {
        banda->posiciones[i].num_piezas = 0;
        pthread_mutex_init(&banda->posiciones[i].mutex, NULL);
        for (int j = 0; j < MAX_PIEZAS_POS; j++) {
            banda->posiciones[i].piezas[j].tipo = 0;
            banda->posiciones[i].piezas[j].id_unico = 0;
        }
    }
}

void destruir_banda(BandaTransportadora *banda) {
    pthread_mutex_destroy(&banda->mutex_global);
    for (int i = 0; i < banda->longitud; i++) {
        pthread_mutex_destroy(&banda->posiciones[i].mutex);
    }
}

int agregar_pieza_posicion(PosicionBanda *pos, Pieza pieza) {
    pthread_mutex_lock(&pos->mutex);
    if (pos->num_piezas >= MAX_PIEZAS_POS) {
        pthread_mutex_unlock(&pos->mutex);
        return -1;  // Posición llena
    }
    pos->piezas[pos->num_piezas] = pieza;
    pos->num_piezas++;
    pthread_mutex_unlock(&pos->mutex);
    return 0;
}

int retirar_pieza_posicion(PosicionBanda *pos, int tipo_buscado) {
    // NOTA: El llamador debe tener el mutex de pos
    for (int p = 0; p < pos->num_piezas; p++) {
        if (pos->piezas[p].tipo == tipo_buscado || tipo_buscado == -1) {
            int tipo = pos->piezas[p].tipo;
            // Compactar el arreglo
            for (int i = p; i < pos->num_piezas - 1; i++) {
                pos->piezas[i] = pos->piezas[i + 1];
            }
            pos->num_piezas--;
            return tipo;
        }
    }
    return -1;  // No encontrada
}

void* thread_banda(void* arg) {
    (void)arg;  // Suprimir warning de parámetro no usado
    
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
                printf("[BANDA] Pieza tipo %s cayó al tacho (total tacho: %d)\n", 
                       nombre_tipo_pieza(ultima->piezas[p].tipo),
                       sistema->stats.total_piezas_tacho);
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
