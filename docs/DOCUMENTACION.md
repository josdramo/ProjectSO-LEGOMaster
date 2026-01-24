# LEGO Master - Sistema de Empaquetado de Bloques

**José Perez**

---

## Descripción del Sistema

Las entidades representadas en mi proyecto son: la banda transportadora, los dispensadores de piezas, las celdas de empaquetado con sus brazos robóticos, las cajas y el operador humano. Debido a la naturaleza concurrente del problema (múltiples piezas moviéndose mientras varios brazos intentan retirarlas simultáneamente), consideré que la mejor solución debería fundamentarse en hilos POSIX con sincronización mediante mutex y semáforos. El esquema de sincronización empleado evita condiciones de carrera al acceder a las posiciones de la banda, garantiza que máximo 2 brazos retiren piezas simultáneamente y que solo 1 brazo coloque piezas en la caja a la vez. El diagrama de componentes de mi solución se muestra en la Figura 1, y el flujo de datos en la Figura 2.

## Arquitectura e Implementación

Se crea el proceso principal (`lego_master.c`) que ejecuta las siguientes tareas al iniciarse:

1. **Validación de parámetros**: Parsea los argumentos de línea de comandos (celdas, sets, piezas por tipo, velocidad, longitud de banda) y valida que estén en rangos aceptables.

2. **Inicialización de estructuras compartidas**: Crea la estructura global `SistemaLego` que contiene la banda transportadora con N posiciones, hasta 4 celdas de empaquetado, y las estadísticas del sistema.

3. **Creación de hilos**:
   - `thread_banda`: Mueve las piezas cada 1/v segundos. Las piezas que llegan al final sin ser recogidas van al tacho.
   - `thread_dispensador`: Controla 3 dispensadores fijos que sueltan piezas con 80% de probabilidad cada ciclo. También implementa el balanceo de carga suspendiendo el brazo con más piezas movidas cada Y piezas dispensadas.
   - `thread_brazo` (4 por celda): Cada brazo retira piezas de la banda y las coloca en la caja. Utiliza un buffer temporal de hasta 20 piezas.
   - `thread_operador`: Verifica las cajas completadas y las marca como OK o FAIL en un tiempo aleatorio entre 0 y Δt₁ milisegundos.
   - `thread_gestor_celdas`: Monitorea la actividad de las celdas y puede activarlas/desactivarlas dinámicamente para optimizar recursos.

## Mecanismos de Sincronización

Con respecto a la sincronización, cada componente tiene sus propios mecanismos:

- **`mutex_posicion[N]`**: Un mutex por cada posición de la banda para acceso exclusivo al retirar o agregar piezas.
- **`sem_brazos_retirando`**: Semáforo inicializado en 2 que limita a máximo 2 brazos retirando piezas simultáneamente por celda.
- **`sem_acceso_caja`**: Semáforo inicializado en 1 que garantiza que solo 1 brazo coloque piezas en la caja a la vez.
- **`mutex_sets`**: Controla el acceso al contador de SETs en proceso y completados.
- **`mutex_celdas_dinamicas`**: Protege las operaciones de activar/desactivar celdas.

## Esquemas de Funcionamiento Implementados

1. **Distribución de piezas**: Las celdas están posicionadas uniformemente en la banda. Cada celda solo toma las piezas que necesita para completar su SET actual. Si una celda detecta que no puede completar su SET (piezas insuficientes), devuelve las piezas a la banda para que otras celdas las utilicen.

2. **Balanceo de carga**: Cada Y piezas dispensadas, el sistema identifica el brazo que ha movido más piezas en cada celda y lo suspende por Δt₂ milisegundos, permitiendo que otros brazos trabajen equitativamente.

3. **Gestión dinámica de celdas**: El gestor monitorea qué celdas están ociosas (sin actividad por varios ciclos) y puede desactivarlas. Si detecta que muchas piezas van al tacho, reactiva celdas desactivadas.

## Limitaciones del Proyecto

- El número de dispensadores está fijo en 3 (no configurable por parámetro).
- El máximo de celdas es 4 debido a restricciones de la estructura de datos.
- En escenarios con alta velocidad de banda y pocas celdas, algunas piezas pueden llegar al tacho antes de ser procesadas.
- La decisión del operador es automática (siempre OK si la caja está correcta), no hay intervención manual real.

## Compilación y Ejecución

Para compilar el proyecto, abrir una terminal en el directorio del proyecto y ejecutar:

```bash
make clean && make all
```

Para ejecutar una simulación:

```bash
./build/lego_master <celdas> <sets> <pA> <pB> <pC> <pD> <velocidad> <longitud>
```

**Ejemplo:**
```bash
./build/lego_master 2 5 3 2 2 1 2 25
```

Donde:
- `celdas`: Número de celdas de empaquetado (1-4)
- `sets`: Número de SETs/cajas a completar
- `pA, pB, pC, pD`: Piezas de cada tipo por SET
- `velocidad`: Velocidad de la banda en pasos/segundo
- `longitud`: Longitud de la banda en posiciones

Para ver la ayuda completa: `./build/lego_master --help`

---

**Figura 1**: Diagrama de componentes del sistema (ver `docs/componentes.puml`)

**Figura 2**: Flujo de datos del sistema (ver `docs/flujo_datos.puml`)
