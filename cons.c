/*
 * cons.c  —  Consumidor SIN semáforos (Práctica 2, Apartado 1)
 *
 * Compilar : gcc -o cons cons.c
 * Ejecutar : ./cons  (en Terminal 2, después de arrancar ./prod)
 *
 * ── Qué hace este programa ───────────────────────────────────────────────
 * Retira caracteres del buffer LIFO compartido y cuenta las vocales
 * consumidas.  NO usa sincronización → puede haber CARRERAS CRÍTICAS.
 *
 * ── Dónde está la carrera ────────────────────────────────────────────────
 * remove_item() modifica shm->top y shm->count sin exclusión mutua.
 * Si el productor está en insert_item() al mismo tiempo (durmiendo dentro
 * de su RC con SLEEP_RC>0), el consumidor puede:
 *   1. Leer buf[top-1] → recupera el carácter que el productor acaba de
 *      escribir (top aún no fue incrementado por el productor).
 *   2. Escribir '-' en buf[top-1] → borra el carácter antes de que el
 *      productor haya actualizado top/count.
 *   3. Decrementar top y count → el productor los incrementará luego
 *      sobre estos valores ya modificados → inconsistencia.
 *
 * El sleep(SLEEP_RC) DENTRO de remove_item() (entre leer el carácter y
 * actualizar count) también amplía la ventana de carrera desde el lado
 * del consumidor, permitiendo que el productor interfiera a su vez.
 *
 * ── Cómo se detecta la carrera en la salida ─────────────────────────────
 * Se guardan top y count antes de la RC.  Si después no bajaron en 1
 * exactamente → [!CARRERA!].  Además, si el carácter retirado es '-'
 * (valor de relleno), el productor o el propio consumidor ha pisado la
 * posición → dato perdido, marcado con [!DATO CORRUPTO!].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

/* ── Constantes (idénticas a prod.c) ─────────────────────────────────── */
#define N        10
#define SHM_FILE "/tmp/shm_buf"
#define MAX_ITER 80

/*
 * SLEEP_RC — segundos que el consumidor duerme DENTRO de su región crítica,
 * entre leer buf[top] y decrementar count.
 * Amplía la ventana de carrera desde el lado del consumidor.
 */
#define SLEEP_RC  1

/* ── Estructura de memoria compartida ───────────────────────────────── */
typedef struct {
    char buf[N];
    int  top;
    int  count;
} SharedMem;

/* ── Globales del consumidor ─────────────────────────────────────────── */
static SharedMem *shm = NULL;

/* Contadores de vocales consumidas */
static int va=0, ve=0, vi=0, vo=0, vu=0;

/* ── Auxiliar: imprime el buffer ─────────────────────────────────────── */
static void print_buf(const char *prefix)
{
    printf("%s buf=[", prefix);
    for (int i = 0; i < N; i++) printf("%c", shm->buf[i]);
    printf("]  top=%d  count=%d\n", shm->top, shm->count);
}

/* ── consume_item() ─────────────────────────────────────────────────── */
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

/*
 * remove_item() — REGIÓN CRÍTICA del consumidor
 * ──────────────────────────────────────────────
 * Sin semáforos: no hay exclusión mutua con insert_item() del productor.
 *
 * Secuencia de operaciones:
 *
 *   [A] shm->top--;                        ← decrementa top
 *   [B] item = shm->buf[shm->top];         ← lee el carácter de la cima
 *   [C] shm->buf[shm->top] = '-';          ← marca la posición como libre
 *       sleep(SLEEP_RC);                   ← *** HUECO DE CARRERA ***
 *                                             El productor puede escribir
 *                                             en buf[top] (que acabamos de
 *                                             marcar con '-') y luego
 *                                             incrementar top y count.
 *   [D] shm->count--;                      ← actualiza count
 *
 * Efectos posibles de la carrera (desde el lado del consumidor):
 *   - Si el productor estaba en su sleep (RC abierta), el consumidor
 *     decrementa top/count sobre valores que el productor no ha terminado
 *     de actualizar → top y count quedan desfasados entre sí.
 *   - El consumidor puede retirar el carácter que el productor acaba de
 *     escribir en buf[top] antes de que el productor incremente top.
 *     Ese carácter quedará contabilizado en el consumidor pero el productor
 *     no sabrá que fue consumido (su top y count serán incorrectos).
 */
static char remove_item(void)
{
    /* Espera activa mientras el buffer está vacío */
    while (shm->count == 0) {
        printf("[CONS]   (buffer vacio, esperando...)\n");
        sleep(1);
    }

    /* ════ INICIO REGIÓN CRÍTICA (SIN protección) ════ */

    shm->top--;                          /* [A] */
    char item = shm->buf[shm->top];      /* [B] */
    shm->buf[shm->top] = '-';            /* [C] marca como libre */

    /*
     * *** SLEEP DENTRO DE LA REGIÓN CRÍTICA — AMPLÍA LA VENTANA DE CARRERA ***
     *
     * El consumidor ha retirado el carácter y marcado la posición con '-',
     * pero count todavía NO se ha decrementado.  Durante este sleep:
     *   - El productor ve count > 0 y puede no detectar que hay un hueco.
     *   - Si el productor entra en su RC y escribe en buf[top] (la posición
     *     que acabamos de liberar), luego incrementará top y count, pero
     *     cuando nosotros decrementemos count en [D], el valor resultante
     *     estará desfasado → INCONSISTENCIA.
     */
    sleep(SLEEP_RC);   /* <── AMPLIFICADOR DE CARRERAS: sleep dentro de la RC */

    shm->count--;                        /* [D] */

    /* ════ FIN REGIÓN CRÍTICA ════ */

    return item;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void)
{
    /* 1. Abrir memoria compartida (creada por el productor) */
    int intentos = 0;
    int fd = -1;
    while (fd < 0 && intentos < 10) {
        fd = open(SHM_FILE, O_RDWR, 0666);
        if (fd < 0) {
            printf("[CONS] Esperando a que el productor cree la memoria...\n");
            sleep(1); intentos++;
        }
    }
    if (fd < 0) { perror("open shm"); exit(EXIT_FAILURE); }

    shm = mmap(NULL, sizeof(SharedMem), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    close(fd);

    printf("[CONS] Iniciado (SLEEP_RC=%ds dentro de la RC).\n", SLEEP_RC);
    printf("[CONS] %d iteraciones. Comprobando carreras criticas...\n\n", MAX_ITER);

    /* 2. Bucle consumidor */
    for (int iter = 0; iter < MAX_ITER; iter++) {

        /* Capturar estado ANTES de entrar en la RC */
        int count_antes = shm->count;
        int top_antes   = shm->top;

        printf("[CONS] ── iter=%2d ──────────────────────────────\n", iter);
        print_buf("[CONS] ANTES: ");

        char item = remove_item();   /* <── región crítica */

        /* Comprobar estado DESPUÉS de la RC */
        int count_despues = shm->count;
        int top_despues   = shm->top;

        print_buf("[CONS] DESPUES:");
        printf("[CONS] Retirado: '%c'\n", item);

        /* Carrera si top o count no bajaron exactamente en 1 */
        int count_ok = (count_despues == count_antes - 1);
        int top_ok   = (top_despues   == top_antes   - 1);

        if (!count_ok || !top_ok) {
            printf("[CONS] *** [!CARRERA CRITICA!] ***\n");
            if (!count_ok)
                printf("[CONS]   count: esperaba %d, vale %d "
                       "(el productor lo modifico en nuestra RC)\n",
                       count_antes - 1, count_despues);
            if (!top_ok)
                printf("[CONS]   top:   esperaba %d, vale %d "
                       "(el productor lo modifico en nuestra RC)\n",
                       top_antes - 1, top_despues);
        }

        /* Dato corrupto: el carácter retirado es '-' (relleno de buffer vacío) */
        if (item == '-') {
            printf("[CONS] *** [!DATO CORRUPTO!] *** "
                   "Se retiró '-' en lugar de un carácter real\n");
        } else {
            if (count_ok && top_ok)
                printf("[CONS]   (extraccion correcta, sin carrera esta vez)\n");
        }

        consume_item(item);
        printf("\n");
    }

    /* 3. Resumen de vocales */
    printf("[CONS] === Vocales consumidas ===\n");
    printf("  a/A=%d  e/E=%d  i/I=%d  o/O=%d  u/U=%d\n\n",
           va, ve, vi, vo, vu);

    munmap(shm, sizeof(SharedMem));
    return 0;
}
