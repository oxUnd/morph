# User questions and automatic continuation

`ask_user` supports structured recommendations and bounded waiting for optional
preferences in the CLI, the CLI JSON protocol, and FastCGI. Preselection is not a
submitted answer. Permission grants remain in the dedicated approval flow.

```json
{
  "question": "Which storage backend should this use?",
  "choices": ["JSON files", "SQLite"],
  "recommended_indices": [1],
  "response_policy": "optional",
  "timeout_seconds": 60
}
```

## Policy

- `recommended_indices` contains distinct zero-based choice indices. Single
  selection permits one recommendation. A multi-selection recommendation must
  satisfy the normalized `min_choices` and `max_choices`. Invalid recommendations
  fail before opening a prompt; an empty recommendation list is allowed.
- `response_policy: "optional"` is intended for preferences that can be resolved
  by a reasonable assumption. Untouched prompts expire after `timeout_seconds`
  (default 60, range 1–86400). The tool adopts the recommended combination, or
  returns no input when no recommendation exists.
- `response_policy: "required"` is intended for critical missing information.
  This is the backward-compatible default. There is no automatic choice;
  `timeout_seconds` does not establish a deadline for required questions.
- Once the user starts navigating, changing a selection, or entering an answer,
  automatic continuation is disabled for that prompt. It remains disabled even
  if the user clears their input. Cancel and EOF are separate from idle expiry.
- The question callback owns its deadline. The generic ReAct tool execution
  timeout does not race the question's deadline or terminate a user mid-answer.

The CLI marks recommendations and preselects them. Enter confirms the current
selection. In the plain prompt fallback, an empty submitted line confirms the
recommended combination if one exists. Esc/Ctrl-C cancels the turn.

## Results

The existing `kind`, `question`, `choices`, `selection_mode`, `min_choices`,
`max_choices`, `answers`, and `no_input` fields remain. The result also includes
recommendation/policy metadata and a `resolution` field:

| Resolution | Meaning |
| --- | --- |
| `user` | The user explicitly submitted the answers. |
| `no_input` | The host returned an empty answer. |
| `timeout_recommended` | An untouched optional question expired and adopted recommendations. |
| `timeout_no_input` | An untouched optional question expired without recommendations. |

`no_input` continues to indicate an empty `answers` array. `resolution` identifies
whether a nonempty answer was submitted by the user or chosen automatically.
Cancellation returns an error and never adopts recommendations. A required
question returning no input does not supply its missing information.

The agent must state that an automatic recommendation is an assumption rather
than claiming that the user chose it. An optional no-input result should not
cause the same question to be asked again.

## CLI JSON protocol

`interaction.request` includes the policy fields inside `data.request`.
Responses continue to use the pending `request_id` and `data.answers`:

```json
{"type":"interaction.response","request_id":"interaction-1","data":{"answers":["SQLite"]}}
```

Hosts should send editing activity as soon as the user begins interacting:

```json
{"type":"interaction.progress","request_id":"interaction-1"}
```

Receiving a partial answer also pauses automatic continuation while that answer
is being entered. A complete stale response does not disable the current
question's deadline. Idle expiry emits `interaction.resolved` with a timeout
reason before the tool returns its result. EOF and explicit cancellation emit
`interaction.cancelled`. Late interaction frames never become a new chat turn.

## FastCGI

The `ask_user` event includes a per-question `data.request_id`, all selection
limits, recommendations, policy, and timeout. Answer an optional question with:

```json
{
  "type": "answer",
  "payload": {
    "request_id": "turn-id-ask-1",
    "answers": ["SQLite"]
  }
}
```

Send `type: "progress"` with the same payload `request_id` when editing starts.
Closing a question emits `interaction.resolved` containing its request ID and
resolution. A matching cancel action closes the question without a default;
an unscoped cancel still cancels the active turn. Stale scoped replies are
consumed without answering a new question or becoming model steering text.
Legacy required questions continue accepting unscoped answers; optional
questions require scoped answers so their late replies can be isolated.

## Native hosts

`ask_user_callback_fn` receives a borrowed `struct ask_user_options` before its
answer outputs. Hosts must honor the policy, idle timeout, and cancellation token,
returning `-ETIMEDOUT` only for an untouched optional prompt. Return cancellation
as `-ECANCELED`/`-EINTR`. The shared tool computes fallback answers and result
provenance, so hosts must not manufacture a user answer on timeout. Mobile
presentation adapters must forward these options before exposing the timed
question interaction in their own UI.
