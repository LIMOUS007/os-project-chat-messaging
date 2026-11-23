/*
 * signals.c — the sigwait thread and the shutdown flag.
 */
#include "signals.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#include "eventloop.h"

/* Written by the signal thread, read by the event loop: atomic, no lock. */
static atomic_int shutting_down = 0;
static atomic_int shutdown_signal = 0;
static pthread_t  signal_thread;

static sigset_t shutdown_signals(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    return set;
}

void signals_block(void)
{
    sigset_t set = shutdown_signals();
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    /* Writing to a socket the client closed must give an error, not kill us. */
    signal(SIGPIPE, SIG_IGN);
}

static void *signal_main(void *arg)
{
    (void)arg;
    sigset_t set = shutdown_signals();
    int sig = 0;
    sigwait(&set, &sig);              /* ordinary thread: no handler restrictions */
    atomic_store(&shutdown_signal, sig);
    atomic_store(&shutting_down, 1);
    eventloop_wake();
    return NULL;
}

void signals_start(void) { pthread_create(&signal_thread, NULL, signal_main, NULL); }

bool shutdown_requested(void) { return atomic_load(&shutting_down) != 0; }

const char *shutdown_signal_name(void)
{
    switch (atomic_load(&shutdown_signal)) {
    case SIGINT:  return "SIGINT";
    case SIGTERM: return "SIGTERM";
    default:      return "internal";
    }
}

void signals_stop_and_join(void)
{
    /* If the server stopped for another reason the thread is still in
     * sigwait: send it SIGTERM (blocked, so sigwait just returns it). */
    if (!shutdown_requested())
        pthread_kill(signal_thread, SIGTERM);
    pthread_join(signal_thread, NULL);
}
