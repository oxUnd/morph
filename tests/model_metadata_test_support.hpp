#ifndef MORPH_METADATA_TEST_SUPPORT_HPP
#define MORPH_METADATA_TEST_SUPPORT_HPP

#include "runtime_test_support.hpp"

extern "C" {
#include "runtime/runtime_internal.h"
#include "runtime/session_title.h"
#include "util/utf8.h"
#include "credits.h"
}

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using TitleClock = std::chrono::steady_clock;

/* A gated HTTP response makes background/rename races deterministic. */
class RuntimeMetadataTest : public RuntimeFacadeTest {
protected:
	int listener = -1;
	std::thread server;
	std::mutex lock;
	std::condition_variable condition;
	bool received = false;
	bool released = false;
	std::string request_body;
	std::string answer = R"({"title":"Fix session naming"})";
	std::vector<std::string> title_phases;
	std::vector<std::string> recap_events;

	void TearDown() override
	{
		Release();
		if (server.joinable()) {
			shutdown(listener, SHUT_RDWR);
			server.join();
		}
		if (listener >= 0)
			close(listener);
		RuntimeFacadeTest::TearDown();
		http_cleanup();
	}

	void Start()
	{
		ASSERT_EQ(http_init(NULL), 0);
		listener = socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(listener, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address),
			sizeof(address)), 0);
		ASSERT_EQ(listen(listener, 2), 0);
		socklen_t size = sizeof(address);
		ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address),
			&size), 0);
		WriteConfig("[memory]\nenabled=false\n[model.text]\nprovider=\"openai\"\n"
			"model=\"title-test\"\napi_key=\"test-key\"\napi_base=\"http://127.0.0.1:" +
			std::to_string(ntohs(address.sin_port)) + "/v1\"\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.event_user_data = this;
		options.event_cb = [](const struct morph_event *event, void *opaque) {
			auto *self = static_cast<RuntimeMetadataTest *>(opaque);
			if (event->name && strcmp(event->name, "session.title") == 0) {
				std::lock_guard<std::mutex> guard(self->lock);
				self->title_phases.emplace_back(event->phase);
			}
			if (event->name && strcmp(event->name, "session.recap") == 0) {
				char *json = morph_event_to_json_string(event);
				std::lock_guard<std::mutex> guard(self->lock);

				if (json)
					self->recap_events.emplace_back(json);
				free(json);
				self->condition.notify_all();
			}
			return 0;
		};
		Open(&options);
		ASSERT_NE(instance->context.llm, nullptr);
		ASSERT_STREQ(instance->context.llm->api_key, "test-key");
		ASSERT_EQ(std::string(instance->context.llm->api_base).find("http://127.0.0.1:"), 0u);
		server = std::thread([this] {
			struct pollfd ready{listener, POLLIN, 0};
			if (poll(&ready, 1, 3000) <= 0)
				return;
			int client = accept(listener, nullptr, nullptr);
			if (client < 0)
				return;
			int no_sigpipe = 1;
#ifdef SO_NOSIGPIPE
			setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
				sizeof(no_sigpipe));
#else
			(void)no_sigpipe;
#endif
			std::string request;
			char buffer[BUFSIZ];
			while (true) {
				ssize_t count = recv(client, buffer, sizeof(buffer), 0);
				if (count <= 0)
					break;
				request.append(buffer, static_cast<size_t>(count));
				auto headers = request.find("\r\n\r\n");
				auto length = request.find("Content-Length:");
				if (headers != std::string::npos && length != std::string::npos &&
				    request.size() >= headers + 4 + std::stoul(request.substr(length + 15)))
					break;
			}
			{
				std::unique_lock<std::mutex> guard(lock);
				request_body = request.substr(request.find("\r\n\r\n") + 4);
				received = true;
				condition.notify_all();
				condition.wait(guard, [this] { return released; });
			}
			cJSON *delta = cJSON_CreateObject();
			cJSON_AddStringToObject(delta, "content", answer.c_str());
			char *json = cJSON_PrintUnformatted(delta);
			std::string body = "data: {\"choices\":[{\"delta\":" + std::string(json) +
				",\"finish_reason\":null}]}\n\n"
				"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
				"\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8,"
				"\"total_tokens\":28}}\n\ndata: [DONE]\n\n";
			free(json);
			cJSON_Delete(delta);
			std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
				"Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
				"\r\n\r\n" + body;
			int flags = 0;
#ifdef MSG_NOSIGNAL
			flags = MSG_NOSIGNAL;
#endif
			(void)send(client, response.data(), response.size(), flags);
			close(client);
		});
	}

	void Release()
	{
		std::lock_guard<std::mutex> guard(lock);
		released = true;
		condition.notify_all();
	}

	bool WaitRequest()
	{
		std::unique_lock<std::mutex> guard(lock);
		return condition.wait_for(guard, std::chrono::seconds(3),
			[this] { return received; });
	}

	int Generate(const std::string &prompt = "Fix session titles")
	{
		struct runtime_request request{};
		request.session_id = instance->context.current_session.id;
		request.stored_user_input = prompt.c_str();
		pthread_mutex_lock(&instance->context.execution_lock);
		int rc = runtime_title_start(instance, &request);
		pthread_mutex_unlock(&instance->context.execution_lock);
		return rc;
	}

	bool WaitName(int64_t id, const std::string &name)
	{
		auto deadline = TitleClock::now() + std::chrono::seconds(3);
		do {
			struct session session{};
			if (session_get_by_id(&instance->context.database, id, &session) == 0 &&
			    session.name == name)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} while (TitleClock::now() < deadline);
		return false;
	}
};

#endif
