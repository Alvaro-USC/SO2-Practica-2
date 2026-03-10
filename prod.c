/*
 * prod.c  —  Productor SIN semáforos (Práctica 2, Apartado 1)
 *
 * Compilar : gcc -o prod prod.c
 * Ejecutar : ./prod  (en Terminal 1, ANTES que cons)
 *
 * ── Qué hace este programa ───────────────────────────────────────────────
 * Lee caracteres de input.txt, los coloca en un buffer LIFO compartido y
 * cuenta las vocales producidas.  NO usa ningún mecanismo de sincronización,
 * por lo que pueden producirse CARRERAS CRÍTICAS con el consumidor.
 *
 * ── Dónde están las carreras críticas ───────────────────────────────────
 * La región crítica del productor es insert_item(): modifica shm->count y
 * shm->top sin exclusión mutua.  Si el consumidor entra en su propia región
 * crítica (remove_item) mientras el productor está en insert_item(), ambos
 * leen/escriben las mismas variables al mismo tiempo → carrera.
 *
 * El sleep(SLEEP_RC) colocado DENTRO de la región crítica (entre escribir
 * buf[top] y actualizar top/count) garantiza con alta probabilidad que el
 * consumidor entra en su RC durante ese intervalo, produciendo la carrera.
 *
 * ── Cómo se detecta la carrera en la salida ─────────────────────────────
 * Antes de llamar a insert_item() se guardan count y top.  Después se
 * comprueba que aumentaron exactamente en 1.  Si no es así, el consumidor
 * modificó esas variables DURANTE nuestra región crítica → [!CARRERA!].
 *
 * También se imprime el contenido del buffer antes y después para que se
 * vea cómo un '-' puede aparecer donde debería haber un carácter, o cómo
 * top apunta a una posición inesperada.
 *
 * ── Memoria compartida ──────────────────────────────────────────────────
 * mmap() sobre /tmp/shm_buf.  Estructura SharedMem: buf[N], top, count.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ── Constantes ──────────────────────────────────────────────────────── */
#define N          10
#define SHM_FILE   "/tmp/shm_buf"
#define INPUT_FILE "input.txt"
#define MAX_ITER   80

/*
 * SLEEP_RC — segundos que el productor duerme DENTRO de la región crítica,
 * entre escribir buf[top] y actualizar top/count.
 *
 * Con SLEEP_RC=1 el consumidor tiene 1 segundo para entrar en su propia
 * región crítica mientras el productor aún no ha terminado la suya.
 * Esto produce la carrera crítica con alta probabilidad.
 *
 * Para ver la ejecución SIN carrera, cambiar a SLEEP_RC=0.
 */
#define SLEEP_RC   1

/* ── Estructura de memoria compartida ───────────────────────────────── */
typedef struct {
    char buf[N];   /* pila LIFO                            */
    int  top;      /* índice de la cima (0=vacío, N=lleno) */
    int  count;    /* elementos actualmente en el buffer   */
} SharedMem;

/* ── Globales del productor ──────────────────────────────────────────── */
static SharedMem *shm = NULL;
static FILE      *fin = NULL;

/* Contadores de vocales producidas */
static int va=0, ve=0, vi=0, vo=0, vu=0;

/* ── Auxiliar: imprime el buffer ─────────────────────────────────────── */
static void print_buf(const char *prefix)
{
    printf("%s buf=[", prefix);
    for (int i = 0; i < N; i++) printf("%c", shm->buf[i]);
    printf("]  top=%d  count=%d\n", shm->top, shm->count);
}

/* ── produce_item() ─────────────────────────────────────────────────── */
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
 * insert_item() — REGIÓN CRÍTICA del productor
 * ─────────────────────────────────────────────
 * Sin semáforos: no hay exclusión mutua con remove_item() del consumidor.
 *
 * Secuencia de operaciones:
 *
 *   [A] shm->buf[shm->top] = item;   ← escribe el carácter
 *       sleep(SLEEP_RC);              ← *** HUECO DE CARRERA ***
 *                                        El consumidor puede ejecutar su RC
 *                                        completa aquí: decrementará top y
 *                                        count, y sobreescribirá buf[top-1]
 *                                        con '-', que es exactamente la
 *                                        posición donde nosotros acabamos
 *                                        de escribir el carácter nuevo.
 *   [B] shm->top++;                   ← actualiza top  (¡ya modificado!)
 *   [C] shm->count++;                 ← actualiza count (¡ya modificado!)
 *
 * Efectos posibles de la carrera:
 *   - count queda en un valor incorrecto (no refleja el nº real de elementos)
 *   - top apunta a una posición errónea
 *   - el carácter insertado en [A] es sobreescrito con '-' por el consumidor
 *     antes de que nadie lo haya leído → pérdida de dato
 */
static void insert_item(char item)
{
    /* Espera activa mientras el buffer está lleno */
    while (shm->count == N) {
        printf("[PROD]   (buffer lleno, esperando...)\n");
        sleep(1);
    }

    /* ════ INICIO REGIÓN CRÍTICA (SIN protección) ════ */

    shm->buf[shm->top] = item;   /* [A] escribir carácter */

    /*
     * *** SLEEP DENTRO DE LA REGIÓN CRÍTICA — AQUÍ SE PRODUCEN LAS CARRERAS ***
     *
     * Durante este sleep, el productor ha escrito el carácter en buf[top]
     * pero top y count siguen con sus valores anteriores.
     * El consumidor ve count > 0 y entra en remove_item():
     *   - decrementa top (apunta ahora a la posición que el prod acaba de escribir)
     *   - lee buf[top] → lee el carácter que el productor puso en [A]
     *   - escribe '-' en buf[top] → borra el carácter del productor
     *   - decrementa count
     * Cuando el productor se despierta, incrementa top y count sobre los
     * valores ya modificados por el consumidor → INCONSISTENCIA.
     */
    sleep(SLEEP_RC);   /* <── PROVOCADOR DE CARRERAS: sleep dentro de la RC */

    shm->top++;        /* [B] */
    shm->count++;      /* [C] */

    /* ════ FIN REGIÓN CRÍTICA ════ */
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void)
{
    /* 1. Crear y mapear memoria compartida */
    int fd = open(SHM_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open shm"); exit(EXIT_FAILURE); }
    ftruncate(fd, sizeof(SharedMem));
    shm = mmap(NULL, sizeof(SharedMem), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    close(fd);

    memset(shm->buf, '-', N);
    shm->top   = 0;
    shm->count = 0;

    /* 2. Abrir fichero de entrada */
    fin = fopen(INPUT_FILE, "r");
    if (!fin) { perror("fopen input.txt"); exit(EXIT_FAILURE); }

    printf("[PROD] Iniciado (SLEEP_RC=%ds dentro de la RC).\n", SLEEP_RC);
    printf("[PROD] %d iteraciones. Comprobando carreras criticas...\n\n", MAX_ITER);

    /* 3. Bucle productor */
    for (int iter = 0; iter < MAX_ITER; iter++) {

        char item = produce_item();

        /* Capturar estado ANTES de entrar en la RC */
        int count_antes = shm->count;
        int top_antes   = shm->top;

        printf("[PROD] ── iter=%2d ──────────────────────────────\n", iter);
        printf("[PROD] Voy a insertar '%c'\n", item);
        print_buf("[PROD] ANTES: ");

        insert_item(item);   /* <── región crítica */

        /* Comprobar estado DESPUÉS de la RC */
        int count_despues = shm->count;
        int top_despues   = shm->top;

        print_buf("[PROD] DESPUES:");

        /*
         * Si count no subió exactamente 1, o top no subió exactamente 1,
         * el consumidor modificó esas variables durante nuestra RC → CARRERA.
         */
        int count_ok = (count_despues == count_antes + 1);
        int top_ok   = (top_despues   == top_antes   + 1);

        if (!count_ok || !top_ok) {
            printf("[PROD] *** [!CARRERA CRITICA!] ***\n");
            if (!count_ok)
                printf("[PROD]   count: esperaba %d, vale %d "
                       "(el consumidor lo modifico en nuestra RC)\n",
                       count_antes + 1, count_despues);
            if (!top_ok)
                printf("[PROD]   top:   esperaba %d, vale %d "
                       "(el consumidor lo modifico en nuestra RC)\n",
                       top_antes + 1, top_despues);
        } else {
            printf("[PROD]   (insercion correcta, sin carrera esta vez)\n");
        }
        printf("\n");
    }

    /* 4. Resumen de vocales */
    printf("[PROD] === Vocales producidas ===\n");
    printf("  a/A=%d  e/E=%d  i/I=%d  o/O=%d  u/U=%d\n\n",
           va, ve, vi, vo, vu);

    fclose(fin);
    munmap(shm, sizeof(SharedMem));
    return 0;
}
