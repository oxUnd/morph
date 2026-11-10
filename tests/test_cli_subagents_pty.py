"""Independent child transcripts, live updates and thread actions in a real PTY."""
import http.server
from pathlib import Path
import queue
import re
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal
from test_cli_agents_pty import tool_delta, wait_for


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def run(driver, port, columns):
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-subagents-pty-') as temp:
        directory = Path(temp)
        (directory / 'child.txt').write_text('CHILD_TOOL_OUTPUT\n')
        terminal = Terminal(driver, directory, port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n')
        try:
            terminal.wait_for_composer()
            terminal.child.setwinsize(24, columns)
            terminal.screen.resize(24, columns)
            terminal.send('inspect independent thread\r')
            _, root = terminal.request()
            releases.append(root)
            root.delta = tool_delta('spawn_agent', {
                'task_name': 'worker', 'fork_turns': 'none',
                'message': 'CHILD_ASSIGNMENT_SENTINEL: inspect child.txt independently',
            }, 'spawn_worker')
            root.set()
            pair = [terminal.request(), terminal.request()]
            releases.extend(release for _, release in pair)
            child = next(release for body, release in pair
                         if any('/root/worker' in message.get('content', '')
                                for message in body['messages'] if message['role'] == 'system'))
            primary = next(release for _, release in pair if release is not child)

            terminal.send('/subagents\r')
            wait_for(terminal, lambda: 'Main conversation' in visible(terminal)
                     and 'worker' in visible(terminal))
            terminal.send('\x1b[B\r')
            wait_for(terminal, lambda: 'Subagents / worker' in visible(terminal)
                     and 'CHILD_ASSIGNMENT_SENTINEL' in visible(terminal))
            assert 'inspect independent thread' not in visible(terminal)
            child.delta = tool_delta('file_read', {
                'file_path': str(directory / 'child.txt'),
            }, 'read_child')
            child.set()
            _, child_final = terminal.request()
            releases.append(child_final)
            wait_for(terminal, lambda: 'CHILD_TOOL_OUTPUT' in visible(terminal))
            child_final.delta = {'content': 'x' * 20000 + '\nCHILD_UTF8_TAIL 中文'}
            child_final.set()
            wait_for(terminal, lambda: 'CHILD_UTF8_TAIL 中文' in visible(terminal))
            wait_for(terminal, lambda: 'completed' in visible(terminal))

            terminal.send('f尾部X\x7f')
            terminal.send('\x1b[200~FOLLOWUP_DIRECTED_SENTINEL\n第二行\n')
            terminal.send('\x1b')
            assert 'Follow up' in visible(terminal), visible(terminal)
            assert Model.requests.empty(), 'paste submitted before its end marker'
            terminal.send('\x1b[201~')
            assert Model.requests.empty(), 'paste submitted without an explicit Enter'
            terminal.send('\r')
            wait_for(terminal, lambda: 'Follow-up sent' in visible(terminal))
            followup, child_again = terminal.request()
            releases.append(child_again)
            assert any('FOLLOWUP_DIRECTED_SENTINEL' in message.get('content', '')
                       for message in followup['messages']), followup
            assert any('尾部FOLLOWUP_DIRECTED_SENTINEL\n第二行' in message.get('content', '')
                       for message in followup['messages']), followup
            terminal.send('k')
            child_again.delta = {'content': 'INTERRUPTED_CHILD_ONLY'}
            child_again.set()
            wait_for(terminal, lambda: 'interrupted' in visible(terminal))

            terminal.send('\x1b')
            wait_for(terminal, lambda: 'Main conversation' in visible(terminal))
            terminal.send('\x1b[H\r')
            wait_for(terminal, lambda: 'ctrl+o details' in visible(terminal))
            main = visible(terminal)
            assert 'Subagents / worker' not in main, main
            assert 'CHILD_UTF8_TAIL' not in main, main
            assert 'CHILD_TOOL_OUTPUT' not in main, main
            terminal.send('PRESERVED_ROOT_DRAFT')
            primary.delta = {'content': 'ROOT_INTEGRATED_SENTINEL'}
            primary.set()
            wait_for(terminal, lambda: 'ROOT_INTEGRATED_SENTINEL' in terminal.raw)
            terminal.pump(.5)
            assert visible(terminal).count('PRESERVED_ROOT_DRAFT') == 1
            terminal.send('\x15/subagents worker\r')
            wait_for(terminal, lambda: 'Subagents / worker' in visible(terminal))
            terminal.send('\x1b[H')
            wait_for(terminal, lambda: 'CHILD_ASSIGNMENT_SENTINEL' in visible(terminal))
            terminal.send('\t\r')
            wait_for(terminal, lambda: 'ctrl+o details' in visible(terminal))
            plain = re.sub(r'\x1b\[[0-9;]*m', '', terminal.raw)
            assert '›› /subagents' not in plain
            assert '› /subagents' not in plain
            print(f'checked subagent replay, live stream, follow-up and interrupt at {columns}',
                  flush=True)
        except Exception:
            print(visible(terminal), flush=True)
            raise
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            while not Model.requests.empty():
                _, release = Model.requests.get_nowait()
                release.set()


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            run(driver, server.server_port, columns)
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
