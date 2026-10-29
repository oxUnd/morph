#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "sapi/cli/callbacks.h"
#include "http/client.h"
#include "util/signals.h"
int cli_command_capture_begin(struct cli_context *, morph_buf_t *);
void cli_command_capture_end(struct cli_context *);
}

#include <chrono>
#include <climits>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <thread>

static int instance_key(struct cli_context *ctx, int count, int key)
{
	ctx->completion_index += count;
	return ctx->completion_index + key;
}

TEST(StateOwnershipTest, NativeCallbacksKeepTheirOwnContext)
{
	struct cli_context first{}, second{};
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
	cli_callbacks_cleanup(&first);
	EXPECT_EQ(second_key(1, 20), 28);
	cli_callbacks_cleanup(&second);
}

TEST(StateOwnershipTest, OutputCaptureAndPresentationOptionsAreIndependent)
{
	struct cli_context first{}, second{};
	morph_buf_t first_text{}, second_text{};

	ASSERT_EQ(morph_buf_init(&first_text, 32), 0);
	ASSERT_EQ(morph_buf_init(&second_text, 32), 0);
	ASSERT_EQ(cli_command_capture_begin(&first, &first_text), 0);
	ASSERT_EQ(cli_command_capture_begin(&second, &second_text), 0);
	cli_set_color_enabled(&first, 0);
	cli_set_structured_output(&second, 1);
	EXPECT_FALSE(cli_color_enabled(&first));
	EXPECT_TRUE(cli_color_enabled(&second));
	EXPECT_FALSE(cli_structured_output_enabled(&first));
	EXPECT_TRUE(cli_structured_output_enabled(&second));
	cli_printf(&first, "first %d", 1);
	cli_printf(&second, "second %d", 2);
	cli_command_capture_end(&first);
	cli_printf(&second, " continued");
	EXPECT_STREQ(morph_buf_cstr(&first_text), "first 1");
	EXPECT_STREQ(morph_buf_cstr(&second_text), "second 2 continued");
	cli_command_capture_end(&second);
	morph_buf_cleanup(&first_text);
	morph_buf_cleanup(&second_text);
}

TEST(StateOwnershipTest, CancellationDoesNotCrossRequestBoundaries)
{
	struct morph_cancel_token first{}, second{};
	struct http_control first_request{}, second_request{};

	first_request.cancel_token = &first;
	second_request.cancel_token = &second;
	morph_cancel_token_cancel(&first);
	EXPECT_EQ(http_wait_cancelable(&first_request, 0), -ECANCELED);
	EXPECT_EQ(http_wait_cancelable(&second_request, 10), 0);
	morph_cancel_token_reset(&first);
	morph_cancel_token_cancel(&second);
	EXPECT_EQ(http_wait_cancelable(&first_request, 10), 0);
	EXPECT_EQ(http_wait_cancelable(&second_request, 0), -ECANCELED);
}

static void record_signal(int number, void *opaque)
{
	auto *received = static_cast<volatile sig_atomic_t *>(opaque);

	morph_signal_flag_set(received, number);
}

TEST(StateOwnershipTest, ListenerReceivesResizeAndRestoresTheThreadMask)
{
	struct morph_signal_listener listener{};
	volatile sig_atomic_t received = 0;
	sigset_t before, after;

	ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &before), 0);
	ASSERT_EQ(morph_signal_listener_start(&listener, record_signal,
					      const_cast<sig_atomic_t *>(&received)),
		  0);
	EXPECT_EQ(pthread_kill(listener.thread, SIGWINCH), 0);
	for (int i = 0; i < 100 && !morph_signal_flag_get(&received); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	EXPECT_EQ(morph_signal_flag_get(&received), SIGWINCH);
	morph_signal_listener_stop(&listener);
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
	struct cli_context ctx{};
	char text[32];
	int original_flags = fcntl(descriptors[0], F_GETFL);
	std::thread cancel([&ctx] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		morph_signal_flag_set(&ctx.interrupted, 1);
	});
	EXPECT_EQ(cli_input_fgets(&ctx, text, sizeof(text), input), nullptr);
	EXPECT_EQ(errno, EINTR);
	cancel.join();
	EXPECT_EQ(fcntl(descriptors[0], F_GETFL), original_flags);
	morph_signal_flag_set(&ctx.interrupted, 0);
	ASSERT_EQ(write(descriptors[1], "first\nsecond\n", 13), 13);
	ASSERT_NE(cli_input_fgets(&ctx, text, sizeof(text), input), nullptr);
	EXPECT_STREQ(text, "first\n");
	ASSERT_NE(cli_input_fgets(&ctx, text, sizeof(text), input), nullptr);
	EXPECT_STREQ(text, "second\n");
	fclose(input);
	close(descriptors[1]);
}
