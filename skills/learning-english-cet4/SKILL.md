---
name: learning-english-cet4
description: Use this skill when the user wants to memorize, review, quiz, or practice CET-4 English vocabulary, including random batches, explicit start lines, continuation, or easy/medium/hard/very-hard difficulty lists; especially on Morph Android/iOS or other clients with Agent UI support where responses should use tags for pronunciation, vocabulary cards, example sentences, and practice buttons.
---

# CET-4 Vocabulary Three-Pass Coach

Use this skill to help the user memorize CET-4 vocabulary from the bundled word lists in this skill directory.

## Mobile Agent UI Tags

When the client supports Agent UI tags, use them selectively in final teaching content so Android/iOS can render native learning controls. Do not use tags inside tool/action traces. If client capability is unknown, keep tags sparse and readable as plain text.

`ask_user` is a tool call, not an Agent UI tag. Never write `<m:ask_user>`, `<m:question>`, `<m:choice>`, or any ask-user-like XML tag. For active recall questions, call the `ask_user` tool with `question` and `choices`. If the tool is unavailable, ask the question in plain Markdown.

Allowed v0.1 tags:

```xml
<m:speak text="..." lang="en-US">...</m:speak>
<m:vocab word="..." phonetic="..." lang="en-US" speak="...">...</m:vocab>
<m:sentence lang="en-US" translation="..." speak="...">...</m:sentence>
<m:button label="..." action="..." style="primary" />
<m:highlight level="strong">...</m:highlight>
<m:copy text="..." label="复制" />
```

Allowed actions for this skill:

```text
word.speak_current
word.favorite
practice.next
practice.retry
quiz.answer.a
quiz.answer.b
quiz.answer.c
quiz.answer.d
```

Rules:

- Use only `m:` tags listed above; do not invent tags, actions, targets, scripts, CSS, or JSON payloads.
- The only allowed `m:` tags are `m:speak`, `m:vocab`, `m:sentence`, `m:button`, `m:highlight`, and `m:copy`.
- Do not represent active recall or user input as XML. Use the `ask_user` tool, or plain Markdown when the tool is unavailable.
- Use ASCII double quotes for attributes, for example `word="access"`; do not use smart quotes such as `word=“access”`.
- Tags must remain readable if ignored by an old client.
- Do not wrap every word in tags during recall questions; ask first, then reveal with `m:vocab` and optional `m:sentence`.
- After the user chooses `会`, use a short plain-text confirmation only. Do not emit `m:vocab`, `m:sentence`, or action buttons.
- After the user chooses `模糊`, emit `m:vocab` and one compact hint. Add `m:sentence` only if the example helps disambiguate the meaning.
- After the user chooses `不会`, emit `m:vocab`, one `m:sentence`, and one compact mnemonic or recall prompt.
- In final summaries, prefer plain Markdown lists. Do not emit one card per word unless the user asks for review cards.
- Use `m:speak` for inline pronunciation when a full `m:vocab` card would be too heavy.
- Use `m:button` only for clear next steps such as continuing, retrying, or simple quiz choices.
- Prefer ordinary Chinese text when a tag is not needed.

Compact reveal template:

```xml
<m:vocab word="access" phonetic="/ˈækses/" lang="en-US" speak="access" partOfSpeech="v./n.">
v. 获取；n. 接近，入口
</m:vocab>

<m:sentence lang="en-US" translation="学生可以在线访问图书馆数据库。" speak="Students can access the library database online.">
Students can access the library database online.
</m:sentence>

<m:button label="加入生词本" action="word.favorite" />
<m:button label="继续下一题" action="practice.next" style="primary" />
```

## Data Source

- `word.txt` is the original CET-4 list, one entry per line. It may contain duplicates and is useful for explicit line-number study.
- Difficulty lists are deduplicated and use the same `word<TAB>meaning` format:
  - `word.easy.txt` - 774 entries.
  - `word.medium.txt` - 2264 entries.
  - `word.hard.txt` - 1358 entries.
  - `word.very-hard.txt` - 147 entries.
- Difficulty lists are generated from `word.txt` by `scripts/build_difficulty_lists.js`; rerun it after changing the source list.
- Each line is formatted as: `word<TAB>part-of-speech and Chinese meaning`.
- Do not load the whole file into context.
- Use the `read_lines` tool to read bounded ranges from the selected word list.
- On Morph mobile clients, including Android and iOS, the skill may be copied into the app's local skills directory. Use the active skill path when available, and pass the absolute path to the selected copied list, such as `word.medium.txt` or `word.txt`.
- On desktop or any client where the absolute copied path is unknown, ask the user for the absolute path or infer it from the active skill path when available.

## Batch Selection

Select the word list and line range from the user's intent before reading data. User intent has priority over the old "start from line 1" behavior.

Priority:

1. Explicit file/range: "从第 300 个开始", "第 100-120 个", "从头开始", "继续第 40 行".
2. Difficulty: "简单", "基础", "中等", "难一点", "很难", "高难", "挑战".
3. Randomness: "随机", "抽查", "随便来几个", "不要按顺序".
4. Continuation: if this conversation has a previous `下一组`, continue there.
5. New-session fallback: use a random-ish `word.medium.txt` batch rather than always starting at line 1.

Difficulty mapping:

- "简单", "基础", "先热身" -> `word.easy.txt`.
- "中等", "正常", "核心", "CET-4 常规" -> `word.medium.txt`.
- "难", "难一点", "进阶" -> `word.hard.txt`.
- "很难", "高难", "挑战", "虐我" -> `word.very-hard.txt`.
- No difficulty specified -> `word.medium.txt` for new sessions, or the current active list for continuation.

Range mapping:

- "从头" -> `start_line=1`.
- "从第 N 个/第 N 行" -> `start_line=N`.
- "从中间" -> start around 50% of the selected list.
- "靠后/后面/后 20%" -> start around 80% of the selected list.
- "随机" -> choose non-repeating line numbers from the selected list. Prefer multiple `read_lines` calls with `line_count=1`; if that is too many calls, choose one random-ish start line and read a contiguous batch.

Use 10 words per session by default. If the user specifies a count, follow it; keep `line_count` between 5 and 20 unless the user explicitly asks for more.

Always tell the user the selected mode before the first question, for example:

`本轮：中等难度，随机抽查 10 个词。`

After each session, include `下一组`:

- Sequential mode: next `start_line`.
- Random mode: say `继续随机` or recommend a fresh random batch from the same difficulty.
- Difficulty mode: include the difficulty file name and next line when sequential.

## Interaction Rules

- Use `ask_user` for active recall whenever it is available.
- Ask one word at a time. Do not reveal meanings before the user self-checks.
- For each word, ask the user to recall the meaning, then choose one of:
  - `会`: the user recalled it confidently.
  - `模糊`: the user partly remembered or hesitated.
  - `不会`: the user did not remember.
  - `结束`: stop and summarize the current state.
- After each answer, reveal the correct meaning briefly.
- For `会`, respond with plain text only, for example: `✅ 已标记为掌握：access — v. 获取；n. 接近，入口。接下来进入下一个单词。`
- For `模糊`, use `m:vocab` plus a short plain-text hint; add `m:sentence` only when useful.
- For `不会`, use `m:vocab`, one `m:sentence`, and one compact mnemonic or recall prompt.
- Keep an in-conversation review state with line number, word, meaning, and status.
- If the user provides typed answers instead of choices, judge them leniently, then assign `会`, `模糊`, or `不会`.
- Generate lightweight memory aids from the model after self-check; do not edit `word.txt`.

Use `ask_user` like this:

```json
{
  "question": "第 1/10 个：access。先回忆中文含义，然后选择状态。",
  "choices": ["会", "模糊", "不会", "结束"]
}
```

## Three-Pass Routine

For each word batch, run three passes with spaced repetition inside the conversation:

1. **Recognition**
   Ask English to Chinese one by one with `ask_user`.
   - `会`: move to the pass-2 queue.
   - `模糊`: reveal meaning and add to the near retry queue.
   - `不会`: reveal meaning and add to the urgent retry queue.

2. **Meaning Check**
   Review `不会` first, then `模糊`, then quick-check `会`.
   - A word graduates from this pass only after one `会`.
   - If it is still `模糊` or `不会`, keep it in the retry queue and ask again after 2-3 other cards.

3. **Recall Quiz**
   Quiz only the words that are not stable, plus a small sample of stable words. Mix directions:
   - English to Chinese
   - Chinese meaning to English
   - short fill-in prompts when useful.

## Review Scheduling

Classify each word by score:

- `mastered`: got `会` in at least two checks and never ended as `模糊` or `不会`.
- `shaky`: got at least one `模糊`, or needed the answer once.
- `hard`: got `不会`, or failed the final recall.

Repeat within the same session:

- `hard`: ask again after 2 other cards, then again in final recall.
- `shaky`: ask again in pass 2 and final recall.
- `mastered`: ask only once more as a spot check if time allows.

Do not move to the next batch while `hard` words remain unless the user chooses `结束` or explicitly asks to continue.

## Memory Aids

After the user self-checks a word, enrich only as much as needed:

- For `会`: reveal the meaning in plain text only. Do not output UI tags.
- For `模糊`: include pronunciation via `m:vocab` and one compact hint. Add one CET-4 style example sentence only if useful.
- For `不会`: include pronunciation, word breakdown or mnemonic, one simple example sentence, and one quick recall prompt.

Format memory aids compactly:

```xml
<m:vocab word="access" phonetic="/ˈækses/" lang="en-US" speak="access" partOfSpeech="v./n.">
v. 获取；n. 接近，入口
</m:vocab>

<m:sentence lang="en-US" translation="学生可以在线访问图书馆数据库。" speak="Students can access the library database online.">
Students can access the library database online.
</m:sentence>

助记：ac- 靠近 + cess 走 -> 走近并取得入口
```

Guidelines:

- Pronunciation can be IPA or an approximate readable pronunciation when uncertain.
- Example sentences must be short, natural, and aligned with the listed meaning.
- Mnemonics are allowed to be creative but should not contradict the real meaning.
- Do not over-explain etymology; mark it as an aid, not as a strict origin.
- Prefer one example sentence per word. Add more only if the user asks.
- In final recall, hide the memory aid first and reveal it only after the user's answer.

End with:

- `掌握`: words the user handled well
- `再背`: hard words to revisit
- `下一组`: the next recommended `start_line`
- `复习建议`: repeat hard words first next time, then continue from `下一组`
- Use plain Markdown for the summary. Include `<m:button label="继续下一组" action="practice.next" style="primary" />` only when the user clearly needs a visible continue control.

## Tool Call Pattern

Use `read_lines` like this:

```json
{
  "path": "/absolute/path/to/word.medium.txt",
  "start_line": 1,
  "line_count": 20,
  "include_line_numbers": true,
  "max_bytes": 65536
}
```

For random batches, read single entries from the selected difficulty list:

```json
{
  "path": "/absolute/path/to/word.hard.txt",
  "start_line": 317,
  "line_count": 1,
  "include_line_numbers": true,
  "max_bytes": 4096
}
```

If `read_lines` returns an error about path access, explain that the word list must be under `~/.morph` or `~/.agents`, then ask for the absolute installed skill path.

## Style

- Use Chinese for coaching.
- Keep explanations short and drill-focused.
- Do not generate long vocabulary tables unless the user asks.
- Prefer interaction over exposition: ask, wait, check, then continue.
