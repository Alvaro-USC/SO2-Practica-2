/*
 * cons_sem.c - Consumidor CON semáforos POSIX (Práctica 2, Apartado 2)
 *
 * Compila: gcc -o cons_sem cons_sem.c -pthread
 * Ejecutar DESPUÉS de prod_sem.c en una terminal separada.
 *
 * Semáforos (creados por el productor; el consumidor los abre sin O_CREAT):
 *   VACIAS  : nº de huecos libres
 *   LLENAS  : nº de elementos listos para consumir
 *   MUTEX   : exclusión mutua en la región crítica
 *
 * Velocidades (fuera de la región crítica):
 *   iter  0-29 : consumidor lento (sleep 2s aquí), prod rápido → buffer se llena
 *   iter 30-59 : consumidor rápido (sin sleep), prod lento → buffer se vacía
 *   iter 60-79 : aleatorio 0-3 s para ambos
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

/* ── Constantes (idénticas a prod_sem.c) ────────────────── */
#define N           10
#define MAX_ITER    80
#define SHM_FILE    "/tmp/shm_sem_buf"

#define SEM_VACIAS  "VACIAS"
#define SEM_LLENAS  "LLENAS"
#define SEM_MUTEX   "MUTEX"

/* ── Estructura de memoria compartida ───────────────────── */
typedef struct {
    char buf[N];
    int  top;
} SharedMem;

/* ── Punteros globales ──────────────────────────────────── */
static SharedMem *shm    = NULL;
static sem_t     *vacias = NULL;
static sem_t     *llenas = NULL;
static sem_t     *mutex  = NULL;

/* Contadores de vocales consumidas */
static int va=0, ve=0, vi=0, vo=0, vu=0;

/* ── Helpers ────────────────────────────────────────────── */

/*
 * remove_item(): retira el elemento de la cima de la pila LIFO
 * y deja '-' en su lugar.
 * ¡Llamar DENTRO de la región crítica (mutex tomado)!
 */
static char remove_item(void)
{
    shm->top--;
    char item = shm->buf[shm->top];
    shm->buf[shm->top] = '-';      /* reemplazar con guión */
    return item;
}

/*
 * consume_item(): procesa el carácter y actualiza contadores de vocales.
 * Se ejecuta FUERA de la región crítica.
 */
static void consume_item(char item)
{
    switch (item) {
        case 'a': case 'A': va++; break;
        case 'e': case 'E': ve++; break;
        case 'i': case 'I': vi++; break;
        case 'o': case 'O': vo++; break;
        case 'u': case 'U': vu++; break;
        default: break;
    }
}

/* ── main ───────────────────────────────────────────────── */
int main(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    /* 1. Abrir semáforos (ya creados por el productor) */
    /* Reintentar brevemente hasta que el productor los cree */
    int intentos = 0;
    while (intentos < 20) {
        vacias = sem_open(SEM_VACIAS, 0);
        llenas = sem_open(SEM_LLENAS, 0);
        mutex  = sem_open(SEM_MUTEX,  0);
        if (vacias != SEM_FAILED && llenas != SEM_FAILED && mutex != SEM_FAILED)
            break;
        printf("[CONS] Esperando a que el productor cree los semáforos...\n");
        sleep(1);
        intentos++;
    }
    if (vacias == SEM_FAILED || llenas == SEM_FAILED || mutex == SEM_FAILED) {
        perror("sem_open consumer"); exit(EXIT_FAILURE);
    }

    /* 2. Mapear memoria compartida */
    int fd = open(SHM_FILE, O_RDWR, 0666);
    if (fd < 0) { perror("open shm"); exit(EXIT_FAILURE); }

    shm = mmap(NULL, sizeof(SharedMem),
               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    close(fd);

    printf("[CONS] Iniciado (pid=%d).\n", getpid());

    /* 3. Bucle consumidor — 80 iteraciones */
    for (int iter = 0; iter < MAX_ITER; iter++) {

        /* ── Semáforos: bloquear si buffer vacío ── */
        sem_wait(llenas);   /* decrementar elementos disponibles (bloquea si ==0) */
        sem_wait(mutex);    /* entrar en región crítica                            */

        /* ══ REGIÓN CRÍTICA ══════════════════════ */
        char item    = remove_item();
        int  val_top = shm->top;  /* capturar top ANTES de liberar el mutex */
        /* ══ FIN REGIÓN CRÍTICA ══════════════════ */

        sem_post(mutex);    /* salir de región crítica   */
        sem_post(vacias);   /* señalar que hay un hueco más libre */

        /* val_top capturado dentro de la RC refleja el estado real
         * tras la extracción, sin interferencia del productor.     */
        printf("[CONS] iter=%2d  retirado='%c'  top_tras_remove=%d\n", iter, item, val_top);

        /* ── Consumir fuera de la región crítica ── */
        consume_item(item);

        /*
         * Sleep FUERA de la región crítica para controlar la velocidad.
         * Fase 1 (iter  0-29): consumidor lento (duerme 2s), productor rápido
         *                      → el buffer se irá llenando hasta N.
         * Fase 2 (iter 30-59): consumidor rápido (no duerme), productor lento
         *                      (duerme 2s en prod_sem.c) → buffer se vacía.
         * Fase 3 (iter 60-79): ambos con tiempo aleatorio 0-3 s → mixto.
         */
        if (iter < 30) {
            sleep(2);   /* consumidor lento en fase 1 */
        } else if (iter < 60) {
            /* consumidor rápido: no duerme */
        } else {
            sleep(rand() % 4);  /* aleatorio 0-3 s en fase 3 */
        }
    }

    /* 4. Mostrar vocales consumidas */
    printf("\n[CONS] === Vocales consumidas ===\n");
    printf("  a/A: %d\n  e/E: %d\n  i/I: %d\n  o/O: %d\n  u/U: %d\n",
           va, ve, vi, vo, vu);

    /* 5. Limpiar recursos del consumidor */
    munmap(shm, sizeof(SharedMem));
    sem_close(vacias);
    sem_close(llenas);
    sem_close(mutex);

    /*
     * Intentar eliminar los semáforos del kernel.
     * Si el productor los eliminó primero, las llamadas devuelven
     * ENOENT (ignorado). Así se garantiza la limpieza sea cual sea
     * el orden de terminación de los dos procesos.
     */
    sem_unlink("VACIAS");
    sem_unlink("LLENAS");
    sem_unlink("MUTEX");

    printf("[CONS] Fin.\n");
    return 0;
}
