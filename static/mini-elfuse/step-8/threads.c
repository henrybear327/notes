#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *worker(void *arg)
{
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return arg;
}

int main(void)
{
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    printf("counter %ld\n", counter);
    return 0;
}
