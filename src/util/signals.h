#ifndef MORPH_SIGNALS_H
#define MORPH_SIGNALS_H

#include <pthread.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The frontend owns one listener; block before starting application workers. */
int morph_signals_block(void);
void morph_signals_reset_child(void);

struct morph_signal_listener {
	pthread_t thread;
	sigset_t signals;
	sigset_t previous;
	struct sigaction previous_winch;
	int started;
	int stopping;
	void (*callback)(int signal_number, void *user_data);
	void *user_data;
};

int morph_signal_listener_start(struct morph_signal_listener *listener,
				void (*callback)(int, void *), void *user_data);
void morph_signal_listener_stop(struct morph_signal_listener *listener);
void morph_signal_flag_set(volatile sig_atomic_t *flag, int value);
int morph_signal_flag_take(volatile sig_atomic_t *flag);
int morph_signal_flag_get(const volatile sig_atomic_t *flag);

#ifdef __cplusplus
}
#endif

#endif
