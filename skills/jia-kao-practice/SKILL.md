---
name: jia-kao-practice
description: Generate Chinese driving-test practice questions, quiz the user one by one with ask_user cards, grade each answer, and finish with a detailed summary.
metadata:
  short-description: Chinese driving-test quiz practice
---

# Jia Kao Practice

Use this skill when the user asks to generate, practice, answer, or review Chinese driving-test questions, including 科目一, 科目三, 驾考题, 驾照考试题, 交通法规题, or safety/civilized-driving practice.

## Core Behavior

- If the user asks for driving-test questions without specifying a count, generate exactly 10 questions.
- If the user specifies any count, use the user's requested count.
- Support 科目一 and 科目三. If the subject is not specified, ask a short clarification only when necessary; otherwise default to mixed 科目一 + 科目三 practice.
- Questions should be plausible Chinese driving-test style questions, written in Chinese.
- Prefer single-choice and true/false questions unless the user explicitly asks for other types.
- Do not reveal the correct answer before the user answers.

## Required Workflow

1. Prepare the full set of questions first.
   - Each question must have an id, subject, prompt, options, correct answer, and explanation.
   - Keep the prepared answer key hidden until after each user answer.
2. Ask the questions one by one using the `ask_user` tool.
   - Each `ask_user` card should contain one question.
   - Include clear answer choices.
   - For true/false questions, use choices like `正确` and `错误`.
   - `ask_user` is a tool call, not markup. Never write `<m:ask_user>`, `<m:question>`, `<m:choice>`, or any ask-user-like XML tag.
3. After each user answer:
   - Say whether the answer is correct.
   - Show the correct answer.
   - Give a concise explanation.
   - Then continue to the next question.
4. After all questions are answered, provide a final summary.

## Final Summary Requirements

The final summary is mandatory. It must include:

- Total score, for example `7/10`.
- A per-question vertical review list covering every question.
- Do not use a Markdown table for the final summary. Driving-test prompts and options are long, and tables are hard to read on mobile screens.
- For every question, include:
  - Question number.
  - Subject, such as 科目一 or 科目三.
  - The full original question prompt.
  - All original answer options.
  - User's selected answer.
  - Correct answer.
  - Whether the user was correct.
  - A short comment or explanation.
- A short overall点评 covering the user's weak points and what to review next.

The summary must be understandable without scrolling back through the conversation. Do not summarize a missed question as only "第 3 题错了"; repeat the original question and options so the user can see exactly what was being reviewed.

Use this layout for each final-summary item:

> 第 N 题目 - 科目一/科目三- N 道正确 / M 道错误

1. 题目主干
  - A. ...
  - B. ...
  - C. ...
  - D. ...

> 你的选择 (..)，正确答案 (...)，回答 **正确/错误**

```
点评：......
```

以上格式要求，正确答案应该用红色加粗输出

## Question Quality

- Make the questions practical and aligned with common Chinese driving-test themes:
  - Traffic signs and markings.
  - Right of way.
  - Speed limits.
  - Safe following distance.
  - Lane changes and overtaking.
  - Emergency handling.
  - Pedestrian and non-motor vehicle safety.
  - Adverse weather driving.
  - Civilized driving and hazard awareness.
- Avoid obscure legal article numbers unless the user asks for law-heavy questions.
- Keep explanations short, direct, and useful.

## Example Trigger

User: 生成驾考题

Behavior: Generate 10 questions by default, ask them one by one with `ask_user`, grade each answer, and finish with the required summary.

User: 给我出 20 道科目三安全文明驾驶题

Behavior: Generate 20 科目三 questions, ask them one by one, grade each answer, and finish with the required summary.
