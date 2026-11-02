"""Verify session pagination/history, JSON escaping, request ids, and limits."""
import http.server
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

import test_fastcgi_ask_user as harness

FCGI_MAX_BODY_BYTES = 4 * 1024 * 1024


def call(endpoint, method, route, payload=None, user='', query='',
         content_length=None, extra=None):
    body = b'' if payload is None else json.dumps(payload).encode()
    params = {
        'REQUEST_METHOD': method,
        'REQUEST_URI': route,
        'PATH_INFO': route.split('?', 1)[0],
        'SCRIPT_NAME': '',
        'QUERY_STRING': query,
        'CONTENT_TYPE': 'application/json',
        'CONTENT_LENGTH': str(len(body) if content_length is None
                              else content_length),
        'HTTP_X_REMOTE_USER': user,
    }
    if extra:
        params.update(extra)

    encoded = b''
    for key, value in params.items():
        key, value = key.encode(), value.encode()

        def length(n):
            return bytes([n]) if n < 128 else struct.pack('!I', n | 0x80000000)

        encoded += length(len(key)) + length(len(value)) + key + value
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(str(endpoint))
        client.sendall(harness.record(1, struct.pack('!HB5x', 1, 0)) +
                       harness.record(4, encoded) + harness.record(4) +
                       harness.record(5, body) + harness.record(5))
        output = b''
        while True:
            _, kind, _, size, padding, _ = struct.unpack(
                '!BBHHBB', harness.read_exact(client, 8))
            data = harness.read_exact(client, size + padding)[:size]
            if kind == 6:
                output += data
            if kind == 3:
                break
    head, raw = output.decode().split('\r\n\r\n', 1)
    lines = head.split('\r\n')
    status = next(int(line.split()[1]) for line in lines
                  if line.startswith('Status:'))
    headers = {}
    for line in lines[1:]:
        if ':' in line:
            key, value = line.split(':', 1)
            headers[key.strip().lower()] = value.strip()
    return status, headers, raw


def wait_for_install(endpoint, process, stderr):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if endpoint.exists():
            try:
                return harness.request(endpoint, '/api/install',
                                       {'username': 'apitest',
                                        'password': 'test-password-123'})
            except (OSError, AssertionError, json.JSONDecodeError):
                pass
        assert process.poll() is None, stderr.read_text()
        time.sleep(.05)
    raise AssertionError('FastCGI server did not start')


def main():
    binary = Path(sys.argv[1]).resolve()
    model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), harness.Model)
    model.daemon_threads = True
    threading.Thread(target=model.serve_forever, daemon=True).start()
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-fcgi-api-') as temp:
            root = Path(temp)
            database = root / 'test.db'
            endpoint = root / 'server.sock'
            config = root / 'config.toml'
            config.write_text(f'''[general]
output_dir = "{root}/output"
[memory]
enabled = false
[features]
hooks = false
[model.text]
provider = "openai"
model = "api-test"
api_base = "http://127.0.0.1:{model.server_port}/v1"
api_key_env = "MORPH_API_TEST_KEY"
retry_count = 0
''')
            env = dict(os.environ,
                       MORPH_FCGI_CONFIG=str(config),
                       MORPH_FCGI_DB=str(database),
                       MORPH_FCGI_LISTEN='unix:' + str(endpoint),
                       MORPH_FCGI_WORKERS='2',
                       MORPH_FCGI_TRUST_HDR='X-Remote-User',
                       MORPH_FCGI_RUNTIME_MIN_WORKERS='1',
                       MORPH_FCGI_RUNTIME_MAX_WORKERS='1',
                       MORPH_FCGI_RUNTIME_QUEUE_MAX='2',
                       MORPH_FCGI_LOG_FILE=str(root / 'server.log'),
                       MORPH_FCGI_OUTPUT_DIR=str(root / 'output'),
                       MORPH_API_TEST_KEY='local-test',
                       HOME=str(root))
            with (root / 'stderr.log').open('w') as log:
                process = subprocess.Popen([str(binary)], env=env,
                                           stdout=log, stderr=log)
                admin = wait_for_install(endpoint, process, root / 'stderr.log')
                user = admin['user_id']
                name = 'quote " and \\ slash\nnewline'
                status, _, raw = call(
                    endpoint, 'POST', '/api/sessions',
                    {'name': name, 'model': 'demo'}, user)
                assert status == 201, (status, raw)
                session = json.loads(raw)['id']
                status, _, raw = call(
                    endpoint, 'POST', '/api/sessions',
                    {'name': 'second'}, user)
                assert status == 201, (status, raw)

                status, _, raw = call(endpoint, 'GET', '/api/sessions',
                                      user=user)
                data = json.loads(raw)
                match = [item for item in data['items']
                         if item['id'] == session]
                assert match and match[0]['name'] == name, data

                status, _, raw = call(endpoint, 'GET', '/api/sessions',
                                      query='limit=1', user=user)
                data = json.loads(raw)
                assert data['limit'] == 1 and data['has_more'] is True, data
                assert len(data['items']) == 1, data
                status, _, raw = call(endpoint, 'GET', '/api/sessions',
                                      query='limit=1&offset=1', user=user)
                data = json.loads(raw)
                assert data['offset'] == 1 and data['has_more'] is False, data
                assert len(data['items']) == 1, data

                renamed = 'renamed "quoted"'
                status, _, raw = call(
                    endpoint, 'PATCH', f'/api/sessions/{session}',
                    {'name': renamed}, user)
                assert status == 200, (status, raw)
                status, _, raw = call(endpoint, 'GET', '/api/sessions',
                                      user=user)
                data = json.loads(raw)
                match = [item for item in data['items']
                         if item['id'] == session]
                assert match and match[0]['name'] == renamed, data

                status, _, raw = call(
                    endpoint, 'GET', f'/api/sessions/{session}/messages',
                    user=user)
                data = json.loads(raw)
                assert status == 200 and data['items'] == [], (status, data)
                assert data['has_more'] is False, data

                status, headers, raw = call(
                    endpoint, 'GET', '/api/health',
                    extra={'HTTP_X_REQUEST_ID': 'test-req-1'}, user=user)
                assert status == 200, raw
                assert headers.get('x-request-id') == 'test-req-1', headers
                health = json.loads(raw)
                assert health['metrics']['requests_total'] >= 1, health

                status, _, raw = call(
                    endpoint, 'POST', '/api/sessions',
                    content_length=FCGI_MAX_BODY_BYTES + 1, user=user)
                assert status == 413, (status, raw)
                status, _, raw = call(
                    endpoint, 'POST', '/api/sessions',
                    content_length='abc', user=user)
                assert status == 413, (status, raw)
                print('PASS: pagination, history, update, escaping, limits')
    finally:
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
