# LEGO Master - Sistema de Simulación de Empaquetado

Simulación de una planta empacadora de bloques LEGO usando programación concurrente en C.

## 📋 Descripción del Problema

El sistema simula una banda transportadora con:
- **Dispensadores**: Sueltan piezas de diferentes tipos (A, B, C, D) al inicio de la banda
- **Banda transportadora**: Mueve las piezas a velocidad constante
- **Celdas de empaquetado**: Ubicadas a lo largo de la banda, con 4 brazos robóticos cada una
- **Brazos robóticos**: Retiran piezas y las colocan en cajas para completar SETs
- **Operador humano**: Verifica y retira las cajas completadas

## 🏗️ Arquitectura

```
[Dispensadores] → [Banda Transportadora] → [Celdas con Brazos] → [Tacho]
                        ↓                        ↓
                   Piezas moviéndose       Cajas con SETs
```

### Restricciones de Sincronización

1. **Máximo 2 brazos** pueden retirar piezas de la banda simultáneamente (por celda)
2. **Solo 1 brazo** puede colocar piezas en la caja a la vez
3. **Balanceo de carga**: Cada Y piezas, el brazo con más movimientos se suspende por Δt2 segundos
4. **Operador**: Al completar un SET, la celda se suspende hasta que el operador retire la caja

## 📁 Estructura del Proyecto

```
ProjectSO-LEGOMaster/
├── src/
│   ├── common.h         # Definiciones y estructuras compartidas
│   ├── utils.c          # Funciones de utilidad
│   └── lego_master.c    # Programa principal de simulación
├── demos-4/
│   ├── dispensers.c     # Demo original de dispensadores
│   └── banda.c          # Demo original de banda
├── Makefile
└── README.md
```

## 🔧 Compilación

```bash
# Compilar todo
make all

# O compilar solo el programa principal
make build/lego_master
```

## 🚀 Ejecución

### Programa Principal

```bash
./build/lego_master <dispensadores> <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>
```

Parámetros:
- `dispensadores`: Número de dispensadores
- `celdas`: Número de celdas de empaquetado (1-4)
- `sets`: Número de SETs a completar
- `pA, pB, pC, pD`: Piezas de cada tipo requeridas por SET
- `velocidad`: Velocidad de la banda (pasos/segundo)
- `longitud`: Longitud de la banda (posiciones)

### Ejemplos

```bash
# Demo rápido
make demo

# Configuración personalizada
make run DISP=4 CELDAS=2 SETS=5 PA=3 PB=2 PC=2 PD=1 VEL=2 LONG=25

# Ejecución directa
./build/lego_master 4 2 3 5 3 4 2 2 20
```

## 🔄 Mecanismos de Sincronización

### Mutex (pthread_mutex_t)
- `mutex_global` de banda: Operaciones globales de movimiento
- `mutex` por posición: Acceso a piezas en cada posición
- `mutex` por celda: Estado de la celda
- `mutex` por caja: Conteo de piezas
- `mutex` por brazo: Estado del brazo

### Semáforos (sem_t)
- `sem_brazos_retirando`: Limita a 2 brazos retirando simultáneamente
- `sem_acceso` de caja: Solo 1 brazo colocando a la vez

## 📊 Estadísticas de Salida

Al finalizar, el programa muestra:
- Cajas completadas correctamente (OK)
- Cajas completadas incorrectamente (FAIL)
- Piezas sobrantes por tipo (en el tacho)
- Piezas movidas por cada brazo

## 🎯 Respuestas a las Preguntas del Diseño

### 1. Representación de piezas
Las piezas se representan con la estructura `Pieza` que contiene tipo (1-4) e ID único. Los SETs se definen por un arreglo de contadores por tipo.

### 2. Sincronización de acceso
- Mutex protegen el acceso a cada posición de la banda y a la caja
- Semáforos controlan la cantidad de brazos operando simultáneamente
- Se verifica si el SET está completo después de cada colocación

### 3. Minimizar tiempo para encontrar brazo a suspender
La función `encontrar_brazo_max_piezas()` recorre los 4 brazos en O(1) constante, ya que siempre son 4 brazos por celda.

### 4. Garantía de llenado correcto
Si hay exactamente las piezas necesarias y la velocidad/cantidad de celdas es suficiente, se llenarán correctamente. Factores:
- Velocidad de la banda vs. capacidad de los brazos
- Posiciones de las celdas
- Tiempo de operación del operador

### 5. Celdas dinámicas
El campo `estado` de `CeldaEmpaquetado` permite activar/desactivar celdas en tiempo de ejecución. Los brazos verifican el estado antes de operar.

## 🐛 Debugging

Para ver más información de depuración, descomenta los printf en las funciones de dispensado y movimiento de banda.

## 📝 Notas de Implementación

- Se usa `pthread` para hilos y sincronización
- La banda se modela como un arreglo de posiciones
- Cada posición puede contener múltiples piezas
- Los brazos operan como hilos independientes coordinados por semáforos