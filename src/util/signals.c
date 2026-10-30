#include "signals.h"
#include "error.h"
#include <errno.h>
#include <string.h>

static void application_signals(sigset_t *signals)
{
	sigemptyset(signals);
	sigaddset(signals, SIGINT);
	sigaddset(signals, SIGTERM);
	sigaddset(signals, SIGHUP);
	sigaddset(signals, SIGQUIT);
	sigaddset(signals, SIGWINCH);
	sigaddset(signals, SIGUSR1);
}

int morph_signals_block(void)
{
	sigset_t signals;

	application_signals(&signals);
	int error = pthread_sigmask(SIG_BLOCK, &signals, NULL);

	return error ? -error : 0;
}

void morph_signals_reset_child(void)
{
	sigset_t empty;

	sigemptyset(&empty);
	(void)sigprocmask(SIG_SETMASK, &empty, NULL);
}

/* Darwin discards default-ignored SIGWINCH before sigwait can consume it.
 * A stateless disposition keeps it pending while all application threads block it. */
static void keep_signal_pending(int number)
{
	(void)number;
}

static void *listen_signals(void *opaque)
{
	struct morph_signal_listener *listener = opaque;

	for (;;) {
		int number;
		int error = sigwait(&listener->signals, &number);

		if (__atomic_load_n(&listener->stopping, __ATOMIC_ACQUIRE))
			return NULL;
		if (!error && number != SIGUSR1 && listener->callback)
			listener->callback(number, listener->user_data);
	}
}

int morph_signal_listener_start(struct morph_signal_listener *listener,
				void (*callback)(int, void *), void *user_data)
{
	if (!listener || !callback)
		return -EINVAL;
	memset(listener, 0, sizeof(*listener));
	application_signals(&listener->signals);
	int error = pthread_sigmask(SIG_BLOCK, &listener->signals, &listener->previous);

	if (error)
		return -error;
	struct sigaction disposition = {0};

	disposition.sa_handler = keep_signal_pending;
	sigemptyset(&disposition.sa_mask);
	if (sigaction(SIGWINCH, &disposition, &listener->previous_winch) != 0) {
		error = errno;
		(void)pthread_sigmask(SIG_SETMASK, &listener->previous, NULL);
		return -error;
	}
	listener->callback = callback;
	listener->user_data = user_data;
	error = pthread_create(&listener->thread, NULL, listen_signals, listener);
	if (error) {
		(void)sigaction(SIGWINCH, &listener->previous_winch, NULL);
		(void)pthread_sigmask(SIG_SETMASK, &listener->previous, NULL);
		return -error;
	}
	listener->started = 1;
	return 0;
}

void morph_signal_listener_stop(struct morph_signal_listener *listener)
{
	if (!listener || !listener->started)
		return;
	__atomic_store_n(&listener->stopping, 1, __ATOMIC_RELEASE);
	(void)pthread_kill(listener->thread, SIGUSR1);
	(void)pthread_join(listener->thread, NULL);
	listener->started = 0;
	(void)sigaction(SIGWINCH, &listener->previous_winch, NULL);
	(void)pthread_sigmask(SIG_SETMASK, &listener->previous, NULL);
}

void morph_signal_flag_set(volatile sig_atomic_t *flag, int value)
{
	if (flag)
		__atomic_store_n(flag, value, __ATOMIC_RELAXED);
}

int morph_signal_flag_take(volatile sig_atomic_t *flag)
{
	return flag ? __atomic_exchange_n(flag, 0, __ATOMIC_RELAXED) : 0;
}

int morph_signal_flag_get(const volatile sig_atomic_t *flag)
{
	return flag ? __atomic_load_n(flag, __ATOMIC_RELAXED) : 0;
}
