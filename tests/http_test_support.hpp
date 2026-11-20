#ifndef MORPH_HTTP_TEST_SUPPORT_HPP
#define MORPH_HTTP_TEST_SUPPORT_HPP

#include "http/request.h"
#include <initializer_list>
#include <string>

class HttpProbe
{
      public:
	nb_http_client *client = nullptr;
	nb_http_call *call = nullptr;
	int status = 0;
	std::string body;

	HttpProbe() = default;
	HttpProbe(const HttpProbe &) = delete;
	HttpProbe &operator=(const HttpProbe &) = delete;
	~HttpProbe()
	{
		nb_http_call_free(call);
		nb_http_client_free(client);
	}

	int run(const char *method, const char *url, nb_str input = NB_STR_NULL,
		std::initializer_list<nb_http_header> headers = {},
		const nb_http_handler *handler = nullptr, int timeout_ms = 5000)
	{
		nb_http_call_free(call);
		call = nullptr;
		status = 0;
		body.clear();
		int rc = client ? NB_OK : http_client_open(nullptr, &client);
		nb_http_request request;
		http_request_init(&request, method, url);
		request.body = input;
		request.headers = {headers.begin(), headers.size()};
		request.timeouts.total_ms = timeout_ms;
		if (rc == NB_OK)
			rc = http_request_run(nullptr, client, &request, handler, &call);

		const nb_http_result *result = nb_http_call_result(call);
		if (result && result->response)
			status = result->response->status_code;

		if (result && (!handler || !handler->on_data)) {
			nb_str bytes{};
			if (nb_http_call_body(call, &bytes, nullptr) == NB_OK)
				body.assign(bytes.ptr, bytes.len);
		}

		return rc;
	}

	std::string header(const char *name) const
	{
		const nb_http_result *result = nb_http_call_result(call);
		nb_str value{};
		if (result && result->response &&
		    nb_http_headers_get(result->response->headers, nb_str_from_cstr(name), &value))
			return std::string(value.ptr, value.len);

		return {};
	}
};

#endif
