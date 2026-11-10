#ifndef MORPH_AGENT_HTTP_TEST_SUPPORT_HPP
#define MORPH_AGENT_HTTP_TEST_SUPPORT_HPP

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <atomic>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Real HTTP requests exercise independent model clients and provider codecs. */
class AgentHttpServer
{
      public:
	std::atomic<bool> stopped{false};
	int listener = -1;
	int port = 0;
	std::thread worker;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> requests;
	std::function<std::string(const std::string &)> respond;

	void start()
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(listener, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
			  0);
		ASSERT_EQ(listen(listener, 8), 0);
		socklen_t size = sizeof(address);
		ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size), 0);
		port = ntohs(address.sin_port);
		worker = std::thread([this] {
			while (!stopped.load()) {
				pollfd pending{listener, POLLIN, 0};
				if (poll(&pending, 1, 100) <= 0)
					continue;
				int client = accept(listener, nullptr, nullptr);
				if (client < 0)
					break;
#ifdef SO_NOSIGPIPE
				int no_sigpipe = 1;
				setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
					   sizeof(no_sigpipe));
#endif
				std::string request;
				char buffer[BUFSIZ];
				size_t body_start = std::string::npos;
				size_t body_size = 0;
				for (;;) {
					ssize_t n = recv(client, buffer, sizeof(buffer), 0);
					if (n <= 0)
						break;
					request.append(buffer, static_cast<size_t>(n));
					body_start = request.find("\r\n\r\n");
					if (body_start == std::string::npos)
						continue;
					auto length = request.find("Content-Length:");
					if (length != std::string::npos)
						body_size = std::stoul(request.substr(length + 15));
					if (request.size() >= body_start + 4 + body_size)
						break;
				}
				std::string body =
					body_start == std::string::npos
						? "{}"
						: request.substr(body_start + 4, body_size);
				{
					std::lock_guard<std::mutex> lock(mutex);
					requests.push_back(body);
					changed.notify_all();
				}
				std::string delta =
					respond ? respond(body) : R"({"content":"child result"})";
				std::string payload =
					"data: "
					"{\"id\":\"test\",\"choices\":[{\"index\":0,\"delta\":" +
					delta +
					",\"finish_reason\":null}]}\n\n"
					"data: "
					"{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
					"\"stop\"}],"
					"\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":5,"
					"\"total_tokens\":17}}\n\n"
					"data: [DONE]\n\n";
				std::string response =
					"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
					"Connection: close\r\nContent-Length: " +
					std::to_string(payload.size()) + "\r\n\r\n" + payload;
				size_t offset = 0;
				while (offset < response.size()) {
					ssize_t n = send(client, response.data() + offset,
							 response.size() - offset, 0);
					if (n <= 0)
						break;
					offset += static_cast<size_t>(n);
				}
				close(client);
			}
		});
	}

	std::vector<std::string> snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return requests;
	}

	bool await_requests(size_t count)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(5),
					[&] { return requests.size() >= count; });
	}

	~AgentHttpServer()
	{
		stopped.store(true);
		if (worker.joinable())
			worker.join();
		if (listener >= 0)
			close(listener);
	}
};

#endif
