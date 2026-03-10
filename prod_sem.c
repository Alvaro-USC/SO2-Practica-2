/*
 * prod_sem.c - Productor CON semáforos POSIX (Práctica 2, Apartado 2)
 *
 * Compila: gcc -o prod_sem prod_sem.c -pthread
 * Ejecutar ANTES que cons_sem.c en una terminal separada.
 *
 * Semáforos usados:
 *   VACIAS  : inicializado a N  → nº de huecos libres en el buffer
 *   LLENAS  : inicializado a 0  → nº de elementos listos para consumir
 *   MUTEX   : inicializado a 1  → exclusión mutua en la región crítica
 *
 * Velocidades (fuera de la región crítica):
 *   iter  0-29 : productor rápido (sleep 0) → buffer se llena
 *   iter 30-59 : consumidor rápido (prod sleep 2) → buffer se vacía
 *   iter 60-79 : aleatorio entre 0 y 3 s para ambos
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

/* ── Constantes ─────────────────────────────────────────── */
#define N           10
#define MAX_ITER    80
#define SHM_FILE    "/tmp/shm_sem_buf"
#define INPUT_FILE  "input.txt"

#define SEM_VACIAS  "VACIAS"
#define SEM_LLENAS  "LLENAS"
#define SEM_MUTEX   "MUTEX"

/* ── Estructura de memoria compartida ───────────────────── */
typedef struct {
    char buf[N];   /* pila LIFO                             */
    int  top;      /* índice libre en la cima               */
} SharedMem;

/* ── Punteros globales ──────────────────────────────────── */
static SharedMem *shm   = NULL;
static sem_t     *vacias = NULL;
static sem_t     *llenas = NULL;
static sem_t     *mutex  = NULL;
static FILE      *fin    = NULL;

/* Contadores de vocales */
static int va=0, ve=0, vi=0, vo=0, vu=0;

/* ── Helpers ────────────────────────────────────────────── */

/* Elimina semáforos del kernel (ignorar error si no existen) */
static void unlink_sems(void)
{
    sem_unlink(SEM_VACIAS);
    sem_unlink(SEM_LLENAS);
    sem_unlink(SEM_MUTEX);
}

/*
 * produce_item(): lee un carácter del fichero de entrada,
 * contabiliza vocales y devuelve el carácter.
 */
static char produce_item(void)
{
    int c = fgetc(fin);
    if (c == EOF) { rewind(fin); c = fgetc(fin); }

    char ch = (char)c;
    switch (ch) {
        case 'a': case 'A': va++; break;
        case 'e': case 'E': ve++; break;
        case 'i': case 'I': vi++; break;
        case 'o': case 'O': vo++; break;
        case 'u': case 'U': vu++; break;
        default: break;
    }
    return ch;
}

/*
 * insert_item(): inserta 'item' en la cima de la pila.
 * ¡Llamar DENTRO de la región crítica (mutex tomado)!
 */
static void insert_item(char item)
{
    shm->buf[shm->top] = item;
    shm->top++;
}

/* ── main ───────────────────────────────────────────────── */
int main(void)
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    /* 1. Eliminar semáforos residuales antes de crearlos */
    unlink_sems();

    /* 2. Crear semáforos e inicializarlos */
    vacias = sem_open(SEM_VACIAS, O_CREAT, 0700, N);  /* N huecos libres */
    llenas = sem_open(SEM_LLENAS, O_CREAT, 0700, 0);  /* 0 elementos     */
    mutex  = sem_open(SEM_MUTEX,  O_CREAT, 0700, 1);  /* acceso exclusivo*/

    if (vacias == SEM_FAILED || llenas == SEM_FAILED || mutex == SEM_FAILED) {
        perror("sem_open"); exit(EXIT_FAILURE);
    }

    /* 3. Crear / mapear memoria compartida */
    int fd = open(SHM_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open shm"); exit(EXIT_FAILURE); }
    if (ftruncate(fd, sizeof(SharedMem)) < 0) {
        perror("ftruncate"); exit(EXIT_FAILURE);
    }
    shm = mmap(NULL, sizeof(SharedMem),
               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    close(fd);

    memset(shm->buf, '-', N);
    shm->top = 0;

    /* 4. Abrir fichero de texto */
    fin = fopen(INPUT_FILE, "r");
    if (!fin) { perror("fopen"); exit(EXIT_FAILURE); }

    printf("[PROD] Iniciado (pid=%d).\n", getpid());

    /* 5. Bucle productor — 80 iteraciones */
    for (int iter = 0; iter < MAX_ITER; iter++) {

        /* ── Producir fuera de la región crítica ── */
        char item = produce_item();
        printf("[PROD] iter=%2d  producido='%c'\n", iter, item);

        /* ── Semáforos: bloquear si buffer lleno ── */
        sem_wait(vacias);   /* decrementar huecos libres (bloquea si ==0) */
        sem_wait(mutex);    /* entrar en región crítica                   */

        /* ══ REGIÓN CRÍTICA ══════════════════════ */
        insert_item(item);
        int val_top = shm->top;  /* capturar top ANTES de liberar el mutex */
        /* ══ FIN REGIÓN CRÍTICA ══════════════════ */

        sem_post(mutex);    /* salir de región crítica  */
        sem_post(llenas);   /* señalar que hay un elemento más para consumir */

        /* Imprimir DESPUÉS de liberar el mutex (no bloquea a otros),
         * pero usamos val_top capturado dentro de la RC para que sea
         * el valor real en el momento de la inserción.             */
        printf("[PROD] iter=%2d  insertado   top_tras_insert=%d\n", iter, val_top);

        /* ── Sleep fuera de la RC según la fase ── */
        /*
         * Sleep FUERA de la región crítica para controlar la velocidad.
         * Fase 1 (iter  0-29): productor rápido, consumidor lento (duerme 2s
         *                      en cons_sem.c) → el buffer se irá llenando.
         * Fase 2 (iter 30-59): productor lento (duerme 2s aquí), consumidor
         *                      rápido (sleep 0 en cons_sem.c) → buffer se vacía.
         * Fase 3 (iter 60-79): ambos con tiempo aleatorio 0-3 s → mixto.
         */
        if (iter < 30) {
            /* productor rápido: no duerme */
        } else if (iter < 60) {
            sleep(2);   /* productor lento en fase 2 */
        } else {
            sleep(rand() % 4);  /* aleatorio 0-3 s en fase 3 */
        }
    }

    /* 6. Mostrar vocales producidas */
    printf("\n[PROD] === Vocales producidas ===\n");
    printf("  a/A: %d\n  e/E: %d\n  i/I: %d\n  o/O: %d\n  u/U: %d\n",
           va, ve, vi, vo, vu);

    /* 7. Limpiar recursos */
    fclose(fin);
    munmap(shm, sizeof(SharedMem));
    sem_close(vacias);
    sem_close(llenas);
    sem_close(mutex);

    /*
     * Eliminar los semáforos del kernel.
     * Si el consumidor ya los eliminó primero, sem_unlink devuelve
     * ENOENT, que unlink_sems() ignora. El segundo en llamarlo
     * simplemente no hace nada dañino.
     */
    unlink_sems();
    printf("[PROD] Semáforos eliminados. Fin.\n");

    return 0;
}
