#include "react_stream.h"

#include "nb/signal.h"
#include "nb/utf8.h"
#include "errors.h"
#include <errno.h>
#include <string.h>

/* Returns zero for a mismatch, one for a prefix and two for a full marker. */
static int final_marker_match(const char *text, size_t length)
{
	if (length <= 6 && memcmp(text, "Final:", length) == 0)
		return length == 6 ? 2 : 1;
	if (length <= 13 && (memcmp(text, "Final answer:", length) == 0 ||
			     memcmp(text, "Final Answer:", length) == 0))
		return length == 13 ? 2 : 1;
	return 0;
}

const char *react_stream_final_borrow(const char *base, const char *from, size_t *marker_len)
{
	const char *position = from;

	if (!base || !from || !marker_len)
		return NULL;
	while (position && *position) {
		const char *line = position;

		while (*line == ' ' || *line == '\t')
			line++;
		if (position == base || position[-1] == '\n' || position[-1] == '\r') {
			size_t length = 0;
			int match = 1;

			while (line[length] && match == 1) {
				length++;
				match = final_marker_match(line, length);
			}
			if (match == 2) {
				*marker_len = (size_t)(line - position) + length;
				return position;
			}
		}
		position = strpbrk(position, "\r\n");
		if (position)
			position++;
	}
	return NULL;
}

int react_stream_init(struct react_stream *stream, const struct react_stream_options *options)
{
	if (!stream || !options)
		return NB_ERR_INVAL;
	memset(stream, 0, sizeof(*stream));
	stream->options = *options;
	stream->candidate_possible = 1;
	stream->error = (nb_buf_init(&stream->text),
			 nb_buf_reserve(&stream->text, 8192) ? 0 : NB_ERR_NOMEM);
	return stream->error;
}

void react_stream_cleanup(struct react_stream *stream)
{
	if (!stream)
		return;
	nb_buf_cleanup(&stream->text);
	nb_buf_cleanup(&stream->reasoning);
	memset(stream, 0, sizeof(*stream));
}

static int stream_status(const struct react_stream *stream)
{
	if (!stream)
		return NB_ERR_INVAL;
	if (stream->emitting)
		return NB_ERR_BUSY;
	if (stream->error != 0)
		return stream->error;
	if (stream->options.cancelled && nb_atomic_int_load(stream->options.cancelled))
		return NB_ERR_INTERRUPTED;
	return 0;
}

static int stream_emit(struct react_stream *stream, enum react_step_type type, nb_buf *buffer,
		       size_t offset, size_t length)
{
	char *text = buffer->ptr ? buffer->ptr + buffer->off + offset : NULL;
	char saved;
	int rc;

	if (type == REACT_STEP_FINAL) {
		while (stream->skip_separator && length > 0 && (*text == ' ' || *text == '\t')) {
			text++;
			length--;
		}
		if (length > 0)
			stream->skip_separator = 0;
	}
	if (length == 0 || !stream->options.emit)
		return 0;
	/* Slice the existing buffer for the synchronous callback, without copying. */
	saved = text[length];
	text[length] = '\0';
	stream->emitting = 1;
	rc = stream->options.emit(type, text, stream->options.user_data);
	stream->emitting = 0;
	text[length] = saved;
	return rc > 0 ? NB_ERR_PROTOCOL : rc;
}

static int stream_drain_reasoning(struct react_stream *stream)
{
	nb_buf *buffer = &stream->reasoning;
	size_t safe;
	int rc;

	if (nb_buf_empty(buffer))
		return 0;
	safe = nb_utf8_clamp_bytes(nb_buf_cstr(buffer), nb_buf_len(buffer));
	rc = stream_emit(stream, REACT_STEP_REASONING, buffer, 0, safe);
	if (rc != 0)
		return rc;
	nb_buf_consume(buffer, safe);
	return 0;
}

/* Each byte is examined once. Only an unfinished line-prefix is held back. */
static int stream_scan_final(struct react_stream *stream, size_t *marker_end)
{
	const char *base = stream->text.ptr;

	while (stream->scanned < stream->text.len) {
		if (!stream->candidate_possible) {
			size_t remaining = stream->text.len - stream->scanned;
			const char *start = base + stream->scanned;
			const char *newline = memchr(start, '\n', remaining);
			const char *carriage = memchr(start, '\r', remaining);
			const char *next =
			    !newline || (carriage && carriage < newline) ? carriage : newline;

			if (!next) {
				stream->scanned = stream->text.len;
				return 0;
			}
			stream->scanned = (size_t)(next - base);
		}
		size_t position = stream->scanned++;
		char byte = base[position];
		int match;

		if (byte == '\n' || byte == '\r') {
			stream->candidate = position + 1;
			stream->prefix_length = 0;
			stream->candidate_possible = 1;
			continue;
		}
		if (!stream->candidate_possible)
			continue;
		if (stream->prefix_length == 0) {
			if (byte == ' ' || byte == '\t')
				continue;
			stream->prefix_start = position;
		}
		stream->prefix_length++;
		match = final_marker_match(base + stream->prefix_start, stream->prefix_length);
		if (match == 2) {
			*marker_end = position + 1;
			return 1;
		}
		if (match == 0)
			stream->candidate_possible = 0;
	}
	return 0;
}

static int stream_drain(struct react_stream *stream, int finish)
{
	const char *base = stream->text.ptr;
	size_t available = stream->text.len - stream->emitted;
	enum react_step_type type = stream->typed_content ? REACT_STEP_CONTENT
				    : stream->in_final	  ? REACT_STEP_FINAL
							  : REACT_STEP_THOUGHT;
	int rc;

	if (available == 0)
		return 0;
	if (!stream->typed_content && !stream->in_final) {
		size_t marker_end = 0;

		if (stream_scan_final(stream, &marker_end)) {
			rc = stream_emit(stream, type, &stream->text, stream->emitted,
					 stream->candidate - stream->emitted);
			if (rc != 0)
				return rc;
			stream->in_final = 1;
			stream->skip_separator = 1;
			stream->emitted = marker_end;
			return stream_drain(stream, finish);
		}
		if (!finish && stream->candidate_possible)
			available = stream->candidate - stream->emitted;
	}
	available = nb_utf8_clamp_bytes(base + stream->emitted, available);
	rc = stream_emit(stream, type, &stream->text, stream->emitted, available);
	if (rc != 0)
		return rc;
	stream->emitted += available;
	if (finish && stream->emitted != stream->text.len)
		return NB_ERR_PROTOCOL;
	return 0;
}

int react_stream_finish(struct react_stream *stream)
{
	int rc = stream_status(stream);

	if (rc != 0)
		return rc;
	stream->error = stream_drain_reasoning(stream);
	if (stream->error == 0)
		stream->error = stream_drain(stream, 1);
	if (stream->error == 0 && !nb_buf_empty(&stream->reasoning))
		stream->error = NB_ERR_PROTOCOL;
	return stream->error;
}

int react_stream_cb(const char *token, void *user_data)
{
	struct react_stream *stream = user_data;
	int rc = stream_status(stream);

	if (rc != 0)
		return rc;
	if (!token)
		return NB_ERR_INVAL;
	rc = (nb_buf_append_cstr(&stream->text, token) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = stream_drain(stream, 0);
	stream->error = rc;
	return rc;
}

int react_typed_stream_cb(enum llm_stream_kind kind, const char *token, void *user_data)
{
	struct react_stream *stream = user_data;
	int rc = stream_status(stream);

	if (rc != 0)
		return rc;
	if (!token)
		return NB_ERR_INVAL;
	if (kind == LLM_STREAM_REASONING) {
		nb_buf *buffer = &stream->reasoning;

		if (!buffer->ptr)
			rc = (nb_buf_init(buffer), nb_buf_reserve(buffer, 64) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(buffer, token) ? 0 : NB_ERR_NOMEM);
		if (rc != 0) {
			stream->error = rc;
			return rc;
		}
		stream->error = stream_drain_reasoning(stream);
		return stream->error;
	}
	if (kind == LLM_STREAM_CONTENT)
		stream->typed_content = 1;
	return react_stream_cb(token, stream);
}
