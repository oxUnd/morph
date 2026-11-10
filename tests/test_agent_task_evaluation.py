"""Acceptance and attribution regressions for the real multi-agent evaluator."""
import argparse
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import eval_multi_agents as evaluation
import eval_multi_agents_acceptance as acceptance


class EvaluationTests(unittest.TestCase):
    def test_ultra_requires_observed_delegation_and_integration(self):
        accepted = {'tests_passed': True, 'integrity_passed': True}
        metrics = {'unconsumed_results': 0, 'collaboration': {}}
        result = evaluation.assess_trial(metrics, 'ultra', False, True, accepted)
        self.assertTrue(result['task_passed'])
        self.assertFalse(result['passed'])
        self.assertEqual(result['orchestration_status'], 'not_observed')
        self.assertTrue(evaluation.assess_trial(metrics, 'normal', False, True, accepted)['passed'])
        metrics['collaboration'].update(delegation_observed=True, integration_observed=True)
        self.assertTrue(evaluation.assess_trial(metrics, 'ultra', False, True, accepted)['passed'])
        self.assertFalse(evaluation.assess_trial(metrics, 'ultra', True, True, accepted)['passed'])
        metrics['collaboration']['dependency_contract_before_child_implementation'] = True
        self.assertTrue(evaluation.assess_trial(metrics, 'ultra', True, True, accepted)['passed'])

    def test_live_profile_output_and_thinking_settings_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            profile = {'model': 'configured-model', 'context_limit': 1048576,
                       'max_tokens': 65536, 'timeout_seconds': 300,
                       'extra_body_json': '{"thinking":{"type":"enabled"}}'}
            effective = evaluation.make_config(directory / 'config.toml', profile,
                                               'ultra', directory, 500000)
            config = evaluation.tomllib.loads((directory / 'config.toml').read_text())
            self.assertEqual(config['model']['text']['max_tokens'], 65536)
            self.assertEqual(config['model']['text']['extra_body_json'], profile['extra_body_json'])
            self.assertEqual(effective['context_limit'], 1048576)
            budget = config['features']['rollout_budget']
            self.assertTrue(budget['enabled'])
            self.assertEqual(budget['limit_tokens'], 500000)
            self.assertEqual(budget['reminder_at_remaining_tokens'], [150000, 50000])

    def test_trusted_verifier_rejects_ndebug_false_success(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            evaluation.create_task(directory, True)
            baseline = acceptance.capture_integrity(directory, dependency=True)
            (directory / 'include/task.h').write_text(evaluation.HEADER + '#define NDEBUG\n')
            result = acceptance.verify_task(directory, self.runner, baseline, dependency=True)
            self.assertFalse(result['tests_passed'])

    def test_valid_candidate_passes_and_protected_file_deletion_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            evaluation.create_task(directory, False)
            baseline = acceptance.capture_integrity(directory)
            for name, source in evaluation.SOURCES.items():
                (directory / f'src/{name}.c').write_text(source)
            result = acceptance.verify_task(directory, self.runner, baseline)
            self.assertTrue(result['tests_passed'], result)
            self.assertTrue(result['integrity_passed'], result)
            (directory / 'verify.c').unlink()
            result = acceptance.verify_task(directory, self.runner, baseline)
            self.assertFalse(result['tests_passed'])
            self.assertFalse(result['integrity_passed'])
            json.dumps(result)

    def test_acceptance_timeout_is_a_result(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            evaluation.create_task(directory, False)
            baseline = acceptance.capture_integrity(directory)
            with mock.patch.object(acceptance.subprocess, 'Popen',
                                   side_effect=subprocess.TimeoutExpired('runner', 1)):
                result = acceptance.verify_task(directory, self.runner, baseline)
            self.assertFalse(result['tests_passed'])
            self.assertTrue(result['timed_out'])
            json.dumps(result)

    def test_real_scripted_run_and_inherited_history_attribution(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            result = evaluation.run_trial(self.driver, self.runner, {}, 'ultra', True,
                                           directory, True, 60, 120000)
            self.assertTrue(result['passed'], result)
            database = directory / 'dependency_contract-ultra/home/.morph/data.db'
            before = evaluation.read_session(database)
            with sqlite3.connect(database) as db:
                child = db.execute('SELECT session_id FROM agent_threads LIMIT 1').fetchone()[0]
                root = before['session_id']
                columns = [row[1] for row in db.execute('PRAGMA table_info(model_history_items)')
                           if row[1] != 'id']
                db.row_factory = sqlite3.Row
                original = db.execute("SELECT * FROM model_history_items WHERE session_id=? "
                                      "AND kind='assistant_tool_calls' LIMIT 1", (root,)).fetchone()
                values = dict(original)
                values['session_id'] = child
                values['sequence_no'] = db.execute(
                    'SELECT MAX(sequence_no)+1 FROM model_history_items WHERE session_id=?',
                    (child,)).fetchone()[0]
                db.execute('INSERT INTO model_history_items (' + ','.join(columns) + ') VALUES (' +
                           ','.join('?' for _ in columns) + ')', [values[name] for name in columns])
            after = evaluation.read_session(database)
            self.assertEqual(before['tool_calls'], after['tool_calls'])
            self.assertEqual(after['history_attribution']['excluded_inherited_records'],
                             before['history_attribution']['excluded_inherited_records'] + 1)
            with sqlite3.connect(database) as db:
                db.row_factory = sqlite3.Row
                receipt = db.execute("SELECT * FROM model_history_items WHERE session_id=? "
                                     "AND kind='tool_result' AND tool_name='exec' LIMIT 1",
                                     (root,)).fetchone()
                metadata = json.loads(receipt['payload_json'])
                reply = json.loads(metadata.get('original_content', receipt['content']))
                reply.get('data', reply)['command'] = 'cc -Iinclude verify.c src/*.c -o /tmp/verify_eval && /tmp/verify_eval'
                metadata['original_content'] = json.dumps(reply)
                db.execute('UPDATE model_history_items SET payload_json=? WHERE id=?',
                           (json.dumps(metadata), receipt['id']))
            after = evaluation.read_session(database)
            self.assertEqual(after['collaboration']['primary_successful_verification_records'], 1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    EvaluationTests.driver = args.driver.resolve()
    EvaluationTests.runner = args.runner.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
