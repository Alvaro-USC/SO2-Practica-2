# Práctica 2 - Sistemas Operativos II: Sincronización de Procesos

## Descripción General
Este repositorio contiene el código fuente correspondiente a la Práctica 2 de Sistemas Operativos II. El objetivo principal de la práctica es ilustrar los problemas derivados de la concurrencia entre procesos y demostrar cómo resolverlos utilizando mecanismos de sincronización proporcionados por el estándar POSIX (semáforos e hilos).

El eje central del ejercicio es el clásico **Problema del Productor-Consumidor**, implementado inicialmente mediante procesos independientes que se comunican a través de memoria compartida, y posteriormente evolucionado hacia arquitecturas multihilo y topologías más complejas.

## Estructura del Proyecto y Archivos Fuente

El proyecto se divide en diferentes apartados, desde la demostración del problema hasta implementaciones avanzadas opcionales:

### 1. Apartado 1: Concurrencia sin Sincronización (Carreras Críticas)
Estos programas comparten un buffer circular LIFO mediante `mmap` pero carecen de mecanismos de exclusión mutua. Su ejecución concurrente provoca condiciones de carrera (race conditions) e inconsistencias en los datos compartidos.
* `prod.c`: Proceso productor. Lee caracteres de un archivo de texto (`input.txt`) y los inserta en la memoria compartida.
* `cons.c`: Proceso consumidor. Extrae caracteres de la memoria compartida y contabiliza las vocales. 

### 2. Apartado 2: Sincronización con Semáforos POSIX
Solución al problema del apartado anterior. Se implementan tres semáforos POSIX con nombre (`VACIAS`, `LLENAS`, `MUTEX`) para garantizar la exclusión mutua en la región crítica y sincronizar los estados de espera sin utilizar espera activa (busy-waiting).
* `prod_sem.c`: Productor sincronizado. Crea e inicializa los semáforos.
* `cons_sem.c`: Consumidor sincronizado. Se acopla a los semáforos existentes creados por el productor.

### 3. Ejercicios Opcionales: Hilos y Topologías Avanzadas
Implementaciones que sustituyen el modelo de múltiples procesos por un modelo multihilo (pthreads), compartiendo memoria de forma nativa en el mismo espacio de direcciones.
* `opc1_threads.c`: Versión del productor-consumidor utilizando un único proceso con dos hilos (un hilo productor y un hilo consumidor).
* `opc2_multi.c`: Generalización del modelo anterior. Permite instanciar un número arbitrario `N` de hilos productores y `M` hilos consumidores operando sobre el mismo buffer de forma segura.
* `opc3_pipeline.c`: Implementación de una arquitectura en tubería (pipeline) donde `P` hilos se pasan información de forma secuencial, actuando cada etapa intermedia como consumidor de la etapa anterior y productor de la siguiente.

### Otros archivos
* `input.txt`: Archivo de texto plano utilizado como fuente de datos para los productores.
* `Makefile`: Script de automatización para compilar todos los binarios con las directivas y librerías adecuadas.

## Instrucciones de Compilación

El proyecto incluye un `Makefile` para automatizar la construcción de todos los binarios. En la raíz del directorio, ejecute:

```bash
make
