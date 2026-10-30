"""Exercise optional questions against the real FastCGI server and a local model."""
import http.server
import json
import os
from pathlib import Path
import queue
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time


class Model(http.server.BaseHTTPRequestHandler):
    requests = queue.Queue()

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        release = threading.Event()
        if any(m.get('role') == 'system' and
               m.get('content', '').startswith("Summarize the user's underlying task")
               for m in body.get('messages', [])):
            release.delta = {'content': '{"title":"Question regression"}'}
            release.set()
        else:
            self.requests.put((body, release))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        if not release.wait(12):
            return
        delta = {'choices': [{'index': 0, 'delta': release.delta}]}
        try:
            self.wfile.write(('data: ' + json.dumps(delta) + '\n\n'
                             'data: [DONE]\n\n').encode())
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def record(kind, data=b''):
    return struct.pack('!BBHHBB', 1, kind, 1, len(data), 0, 0) + data


def read_exact(stream, size):
    data = b''
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        assert chunk, 'FastCGI server closed the connection'
        data += chunk
    return data


def request(path, route, payload, user=''):
    body = json.dumps(payload).encode()
    params = {'REQUEST_METHOD': 'POST', 'REQUEST_URI': route, 'PATH_INFO': route,
              'SCRIPT_NAME': '', 'QUERY_STRING': '', 'CONTENT_TYPE': 'application/json',
              'CONTENT_LENGTH': str(len(body)), 'HTTP_X_REMOTE_USER': user}
    encoded = b''
    for key, value in params.items():
        key, value = key.encode(), value.encode()
        def length(n):
            return bytes([n]) if n < 128 else struct.pack('!I', n | 0x80000000)
        encoded += length(len(key)) + length(len(value)) + key + value
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(str(path))
        client.sendall(record(1, struct.pack('!HB5x', 1, 0)) + record(4, encoded) +
                       record(4) + record(5, body) + record(5))
        output = b''
        while True:
            _, kind, _, size, padding, _ = struct.unpack('!BBHHBB', read_exact(client, 8))
            data = read_exact(client, size + padding)[:size]
            if kind == 6:
                output += data
            if kind == 3:
                break
    head, body = output.decode().split('\r\n\r\n', 1)
    assert any(f'Status: {code}' in head for code in (200, 201, 202)), output
    return json.loads(body)


def wait_event(database, session, kind, after=0):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        with sqlite3.connect(database) as db:
            row = db.execute('SELECT id,payload FROM fcgi_events '
                             'WHERE session_id=? AND type=? AND id>? ORDER BY id LIMIT 1',
                             (session, kind, after)).fetchone()
        if row:
            return row[0], json.loads(row[1])
        time.sleep(.03)
    raise AssertionError(f'No {kind} event after {after}')


def main():
    binary = Path(sys.argv[1]).resolve()
    model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    model.daemon_threads = True
    threading.Thread(target=model.serve_forever, daemon=True).start()
    releases = []
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-fcgi-question-') as temp:
            root = Path(temp)
            database, endpoint = root / 'test.db', root / 'server.sock'
            config = root / 'config.toml'
            config.write_text(f'''[general]
output_dir = "{root}/output"
[memory]
enabled = false
[features]
hooks = false
[model.text]
provider = "openai"
model = "question-test"
api_base = "http://127.0.0.1:{model.server_port}/v1"
api_key_env = "MORPH_QUESTION_TEST_KEY"
retry_count = 0
''')
            env = dict(os.environ, MORPH_FCGI_CONFIG=str(config), MORPH_FCGI_DB=str(database),
                       MORPH_FCGI_LISTEN='unix:' + str(endpoint), MORPH_FCGI_WORKERS='2',
                       MORPH_FCGI_TRUST_HDR='X-Remote-User', MORPH_FCGI_RUNTIME_MIN_WORKERS='1',
                       MORPH_FCGI_RUNTIME_MAX_WORKERS='1', MORPH_FCGI_LOG_FILE=str(root / 'server.log'),
                       MORPH_FCGI_OUTPUT_DIR=str(root / 'output'), MORPH_QUESTION_TEST_KEY='local-test',
                       HOME=str(root))
            with (root / 'stderr.log').open('w') as log:
                process = subprocess.Popen([str(binary)], env=env, stdout=log, stderr=log)
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if endpoint.exists():
                        try:
                            admin = request(endpoint, '/api/install',
                                            {'username': 'questiontest', 'password': 'test-password-123'})
                            break
                        except (OSError, AssertionError):
                            pass
                    assert process.poll() is None, (root / 'stderr.log').read_text()
                    time.sleep(.05)
                else:
                    raise AssertionError('FastCGI server did not start')
                user = admin['user_id']
                session = request(endpoint, '/api/sessions', {'name': 'Questions'}, user)['id']
                request(endpoint, f'/api/sessions/{session}/turns', {'prompt': 'Choose settings'}, user)

                def ask(arguments):
                    _, release = Model.requests.get(timeout=8)
                    releases.append(release)
                    release.delta = {'tool_calls': [{'index': 0, 'id': 'ask-' + arguments['question'],
                        'type': 'function', 'function': {'name': 'ask_user',
                        'arguments': json.dumps(arguments)}}]}
                    release.set()

                def result(expected, source):
                    body, release = Model.requests.get(timeout=8)
                    releases.append(release)
                    data = [json.loads(m['content']) for m in body['messages']
                            if m['role'] == 'tool' and 'ask_user_response' in m['content']][-1]
                    data = data.get('data', data)
                    assert data['answers'] == expected and data['resolution'] == source, data
                    return release

                args = {'question': 'first', 'choices': ['A', 'B'], 'recommended_indices': [1],
                        'response_policy': 'optional', 'timeout_seconds': 1}
                ask(args)
                first_event, first = wait_event(database, session, 'ask_user')
                release = result(['B'], 'timeout_recommended')
                release.delta = {'tool_calls': [{'index': 0, 'id': 'ask-second', 'type': 'function',
                    'function': {'name': 'ask_user', 'arguments': json.dumps(dict(args, question='second'))}}]}
                release.set()
                second_event, second = wait_event(database, session, 'ask_user', first_event)
                assert first['data']['request_id'] != second['data']['request_id']
                request(endpoint, f'/api/sessions/{session}/actions', {'type': 'answer',
                        'payload': {'request_id': first['data']['request_id'], 'answers': ['A']}}, user)
                release = result(['B'], 'timeout_recommended')
                release.delta = {'tool_calls': [{'index': 0, 'id': 'ask-third', 'type': 'function',
                    'function': {'name': 'ask_user', 'arguments': json.dumps(dict(args, question='third'))}}]}
                release.set()
                _, third = wait_event(database, session, 'ask_user', second_event)
                request_id = third['data']['request_id']
                assert third['data']['selection_mode'] == 'single'
                request(endpoint, f'/api/sessions/{session}/actions', {'type': 'progress',
                        'payload': {'request_id': request_id}}, user)
                time.sleep(1.3)
                assert Model.requests.empty(), 'Editing activity did not pause auto-continue'
                request(endpoint, f'/api/sessions/{session}/actions', {'type': 'answer',
                        'payload': {'request_id': request_id, 'answers': ['custom']}}, user)
                release = result(['custom'], 'user')
                release.delta = {'content': 'Done.'}
                release.set()
                wait_event(database, session, 'turn_end')
                print('PASS: FastCGI timeout fallback, question IDs, stale reply isolation, and editing pause')
    finally:
        for release in releases:
            if not release.is_set():
                release.delta = {'content': 'Done.'}
                release.set()
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        model.shutdown()


if __name__ == '__main__':
    main()
