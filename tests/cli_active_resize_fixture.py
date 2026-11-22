"""Deterministic model peer for resizing a production frontend during active work."""
import http.server
import json
import time


class ActiveResizeModel(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.put(body)
        messages = body.get('messages', [])
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()

        def delta(value):
            event = {'choices': [{'index': 0, 'delta': value}]}
            self.wfile.write(('data: ' + json.dumps(event) + '\n\n').encode())
            self.wfile.flush()

        def complete(reason='stop'):
            event = {'choices': [{'index': 0, 'delta': {}, 'finish_reason': reason}]}
            self.wfile.write(('data: ' + json.dumps(event) + '\n\ndata: [DONE]\n\n').encode())
            self.wfile.flush()

        try:
            if any(message.get('role') == 'system' and
                   message.get('content', '').startswith('Summarize the user')
                   for message in messages):
                delta({'content': '{"title":"Active resize"}'})
                complete()
                return

            last_user = max(index for index, message in enumerate(messages)
                            if message.get('role') == 'user')
            prompt = messages[last_user].get('content', '')
            finished_tool = any(message.get('role') == 'tool'
                                for message in messages[last_user + 1:])
            if prompt == 'RESIZE_STREAM':
                delta({'reasoning_content': 'Inspect the retained output while resizing.\n'})
                delta({'content': 'RESIZE_STREAM_START\n\n```c\n'})
                for index in range(80):
                    delta({'content': f'int resize_row_{index:03d} = {index}; /* 中文🙂 */\n'})
                    time.sleep(.05)
                delta({'content': '```\n\nRESIZE_STREAM_DONE'})
                complete()
            elif prompt in ('RESIZE_TOOL', 'RESIZE_APPROVAL') and not finished_tool:
                if prompt == 'RESIZE_APPROVAL':
                    time.sleep(.8)
                command = (
                    "printf 'RESIZE_TOOL_START\\n'; "
                    'resize_until=$((SECONDS + 3)); '
                    'while [ "$SECONDS" -lt "$resize_until" ]; do :; done; '
                    "printf 'RESIZE_TOOL_FINISH\\n'"
                    if prompt == 'RESIZE_TOOL' else
                    'printf RESIZE_APPROVED > resize-approval.txt')
                arguments = {'command': command, 'host': True, 'yield_time_ms': 10000,
                             'description': prompt}
                delta({'tool_calls': [{'index': 0, 'id': prompt.lower(), 'type': 'function',
                       'function': {'name': 'exec', 'arguments': json.dumps(arguments)}}]})
                complete('tool_calls')
            else:
                delta({'content': prompt + '_DONE'})
                complete()
        except (BrokenPipeError, ConnectionResetError):
            pass
