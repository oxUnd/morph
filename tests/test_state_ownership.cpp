#include <gtest/gtest.h>
#include "nb/err.h"
#include "nb/cleanup.h"

extern "C" {
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/application.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "http/client.h"
#include "nb/signal.h"
}

#include <chrono>
#include <climits>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <thread>

static int instance_key(struct cli_application *ctx, int count, int key)
{
	ctx->input->completion_index += count;
	return ctx->input->completion_index + key;
}

TEST(StateOwnershipTest, NativeCallbacksKeepTheirOwnContext)
{
	struct cli_application first{}, second{};
	auto first_key = cli_callback_key(&first, instance_key);
	auto second_key = cli_callback_key(&second, instance_key);

	ASSERT_NE(first_key, nullptr);
	ASSERT_NE(second_key, nullptr);
	EXPECT_NE(first_key, second_key);
	EXPECT_EQ(first_key(3, 10), 13);
	EXPECT_EQ(second_key(7, 20), 27);
	EXPECT_EQ(first_key(2, 10), 15);
	EXPECT_EQ(first_key(0, INT_MIN), INT_MIN + 5);
	EXPECT_EQ(second_key(0, -8), -1);
	cli_input_cleanup(&first);
	EXPECT_EQ(second_key(1, 20), 28);
	cli_input_cleanup(&second);
}

TEST(StateOwnershipTest, OutputCaptureAndPresentationOptionsAreIndependent)
{
	struct cli_application first{}, second{};
	nb_buf first_text{}, second_text{};

	ASSERT_EQ((nb_buf_init(&first_text), nb_buf_reserve(&first_text, 32) ? 0 : NB_ERR_NOMEM),
		  0);
	ASSERT_EQ((nb_buf_init(&second_text), nb_buf_reserve(&second_text, 32) ? 0 : NB_ERR_NOMEM),
		  0);
	ASSERT_EQ(cli_output_capture_begin(&first, &first_text), 0);
	ASSERT_EQ(cli_output_capture_begin(&second, &second_text), 0);
	cli_output_set_color_enabled(&first, 0);
	cli_output_set_structured(&second, 1);
	EXPECT_FALSE(cli_output_color_enabled(&first));
	EXPECT_TRUE(cli_output_color_enabled(&second));
	EXPECT_FALSE(cli_output_is_structured(&first));
	EXPECT_TRUE(cli_output_is_structured(&second));
	cli_output_printf(&first, "first %d", 1);
	cli_output_printf(&second, "second %d", 2);
	cli_output_capture_end(&first);
	cli_output_printf(&second, " continued");
	EXPECT_STREQ(nb_buf_cstr(&first_text), "first 1");
	EXPECT_STREQ(nb_buf_cstr(&second_text), "second 2 continued");
	cli_output_capture_end(&second);
	nb_buf_cleanup(&first_text);
	nb_buf_cleanup(&second_text);
}

TEST(StateOwnershipTest, CancellationDoesNotCrossRequestBoundaries)
{
	struct nb_cancel first{}, second{};
	struct http_control first_request{}, second_request{};

	first_request.cancel_token = &first;
	second_request.cancel_token = &second;
	nb_cancel_request(&first);
	EXPECT_EQ(http_wait_cancelable(&first_request, 0), NB_ERR_CANCELLED);
	EXPECT_EQ(http_wait_cancelable(&second_request, 10), 0);
	nb_cancel_reset(&first);
	nb_cancel_request(&second);
	EXPECT_EQ(http_wait_cancelable(&first_request, 10), 0);
	EXPECT_EQ(http_wait_cancelable(&second_request, 0), NB_ERR_CANCELLED);
}

static void record_signal(int number, void *opaque)
{
	auto *received = static_cast<nb_atomic_int *>(opaque);

	nb_atomic_int_store(received, number);
}

TEST(StateOwnershipTest, ListenerReceivesResizeAndRestoresTheThreadMask)
{
	nb_signal_listener *listener NB_CLEANUP(nb_signal_listener_cleanup) = nullptr;
	const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH};
	nb_atomic_int received = 0;
	sigset_t before, after;

	ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &before), 0);
	ASSERT_EQ(
		nb_signal_listener_start(&listener, signals, 5, record_signal, &received, nullptr),
		0);
	EXPECT_EQ(nb_signal_listener_send(listener, SIGWINCH, nullptr), NB_OK);
	for (int i = 0; i < 100 && !nb_atomic_int_load(&received); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	EXPECT_EQ(nb_atomic_int_load(&received), SIGWINCH);
	ASSERT_EQ(nb_signal_listener_stop(&listener, nullptr), NB_OK);
	EXPECT_EQ(listener, nullptr);
	ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &after), 0);
	for (int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH, SIGUSR1})
		EXPECT_EQ(sigismember(&before, number), sigismember(&after, number));
}

TEST(StateOwnershipTest, BufferedInputCanBeCancelledWithoutLosingFollowingLines)
{
	int descriptors[2];
	ASSERT_EQ(pipe(descriptors), 0);
	FILE *input = fdopen(descriptors[0], "r");
	ASSERT_NE(input, nullptr);
	struct cli_application ctx{};
	char text[32];
	int original_flags = fcntl(descriptors[0], F_GETFL);
	std::thread cancel([&ctx] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		nb_atomic_int_store(&ctx.interrupted, 1);
	});
	EXPECT_EQ(cli_input_fgets(&ctx, text, sizeof(text), input), nullptr);
	EXPECT_EQ(errno, EINTR);
	cancel.join();
	EXPECT_EQ(fcntl(descriptors[0], F_GETFL), original_flags);
	nb_atomic_int_store(&ctx.interrupted, 0);
	ASSERT_EQ(write(descriptors[1], "first\nsecond\n", 13), 13);
	ASSERT_NE(cli_input_fgets(&ctx, text, sizeof(text), input), nullptr);
	EXPECT_STREQ(text, "first\n");
	ASSERT_NE(cli_input_fgets(&ctx, text, sizeof(text), input), nullptr);
	EXPECT_STREQ(text, "second\n");
	fclose(input);
	close(descriptors[1]);
}
