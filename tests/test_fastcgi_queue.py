"""Verify per-session turn queueing, ordering, and queue-full rejection."""
import http.server
from loopback_http import LoopbackHTTPServer
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time

import test_fastcgi_ask_user as harness


def request_status(path, route, payload, user='', expected=(200, 201, 202)):
    body = json.dumps(payload).encode()
    params = {'REQUEST_METHOD': 'POST', 'REQUEST_URI': route, 'PATH_INFO': route,
              'SCRIPT_NAME': '', 'QUERY_STRING': '',
              'CONTENT_TYPE': 'application/json',
              'CONTENT_LENGTH': str(len(body)),
              'HTTP_X_REMOTE_USER': user}
    encoded = b''
    for key, value in params.items():
        key, value = key.encode(), value.encode()

        def length(n):
            return bytes([n]) if n < 128 else struct.pack('!I', n | 0x80000000)

        encoded += length(len(key)) + length(len(value)) + key + value
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(str(path))
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
    head, body = output.decode().split('\r\n\r\n', 1)
    status = next(int(line.split()[1]) for line in head.split('\r\n')
                  if line.startswith('Status:'))
    assert status in expected, (status, output)
    return status, json.loads(body)


def wait_turn_event(database, session, kind, turn_id):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        with sqlite3.connect(database) as db:
            rows = db.execute(
                'SELECT id,payload FROM fcgi_events '
                'WHERE session_id=? AND type=? ORDER BY id', (session, kind))
            for row_id, payload in rows:
                data = json.loads(payload)
                if data.get('turn_id') == turn_id:
                    return row_id, data
        time.sleep(.03)
    raise AssertionError(f'No {kind} event for {turn_id}')


def turn_events(database, session, kind):
    with sqlite3.connect(database) as db:
        rows = db.execute(
            'SELECT payload FROM fcgi_events '
            'WHERE session_id=? AND type=? ORDER BY id', (session, kind))
        return [json.loads(payload) for (payload,) in rows]


def main():
    binary = Path(sys.argv[1]).resolve()
    model = LoopbackHTTPServer(('127.0.0.1', 0), harness.Model)
    model.daemon_threads = True
    threading.Thread(target=model.serve_forever, daemon=True).start()
    releases = []
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-fcgi-queue-') as temp:
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
model = "queue-test"
api_base = "http://127.0.0.1:{model.server_port}/v1"
api_key_env = "MORPH_QUEUE_TEST_KEY"
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
                       MORPH_FCGI_RUNTIME_QUEUE_MAX='1',
                       MORPH_FCGI_LOG_FILE=str(root / 'server.log'),
                       MORPH_FCGI_OUTPUT_DIR=str(root / 'output'),
                       MORPH_QUEUE_TEST_KEY='local-test',
                       HOME=str(root))
            with (root / 'stderr.log').open('w') as log:
                process = subprocess.Popen([str(binary)], env=env,
                                           stdout=log, stderr=log)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if endpoint.exists():
                        try:
                            admin = harness.request(
                                endpoint, '/api/install',
                                {'username': 'queuetest',
                                 'password': 'test-password-123'})
                            break
                        except (OSError, AssertionError):
                            pass
                    assert process.poll() is None, (root / 'stderr.log').read_text()
                    time.sleep(.05)
                else:
                    raise AssertionError('FastCGI server did not start')
                user = admin['user_id']
                session = harness.request(
                    endpoint, '/api/sessions', {'name': 'Queue'}, user)['id']

                first = harness.request(
                    endpoint, f'/api/sessions/{session}/turns',
                    {'prompt': 'first'}, user)
                assert first['queue_position'] == 1, first
                body, release = harness.Model.requests.get(timeout=10)
                releases.append(release)

                second = harness.request(
                    endpoint, f'/api/sessions/{session}/turns',
                    {'prompt': 'second'}, user)
                assert second['queue_position'] == 1, second
                status, body = request_status(
                    endpoint, f'/api/sessions/{session}/turns',
                    {'prompt': 'third'}, user, expected=(429,))
                assert status == 429 and body['error'] == 'queue_full', body

                wait_turn_event(database, session, 'turn_queued',
                                second['turn_id'])
                release.delta = {'content': 'first done'}
                release.set()
                wait_turn_event(database, session, 'turn_end',
                                first['turn_id'])
                body, release = harness.Model.requests.get(timeout=10)
                releases.append(release)
                release.delta = {'content': 'second done'}
                release.set()
                wait_turn_event(database, session, 'turn_end',
                                second['turn_id'])

                starts = turn_events(database, session, 'turn_start')
                order = [event['turn_id'] for event in starts]
                assert order == [first['turn_id'], second['turn_id']], order
                queued = turn_events(database, session, 'turn_queued')
                assert [event['turn_id'] for event in queued] == order, queued
                print('PASS: per-session FIFO queue, queue position, queue_full')
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
