---
name: grilling
description: Grill the user relentlessly about a plan, decision, or idea, one question at a time, and capture the decisions as a self-contained GitHub issue. Use when the user wants to stress-test their thinking, or uses any 'grill' trigger phrases.
---

<!-- Matt Pocock's `grilling` skill (MIT, https://github.com/mattpocock/skills),
     his wording throughout. Changed here: one question at a time rather than a
     round of them, and a GitHub issue at the end. -->

Interview the user relentlessly until you reach a shared understanding. Map this as a **design tree**: every decision branches into the decisions that hang off it.

Work the tree **one question at a time**. The **frontier** is every decision whose prerequisites are already settled: the questions you can ask _now_ without guessing at answers you haven't heard yet. Ask the single most load-bearing question on the frontier — the one whose answer reshapes the most of the tree — and give your recommended answer. Then wait for the user's answer before the next question. Never stack a second question underneath.

Format a question like so:

```
❓ **<question title>**: <question body, might be multiple paragraphs, including multiple choices>

➡️ <your recommended answer>
```

Each answer the user gives reshapes the tree: settled decisions push the frontier outward and unblock questions that depended on them. Recompute the frontier and ask the next single most load-bearing question. A question whose answer depends on one you haven't asked yet belongs _later_, not now.

Finding _facts_ is your job, never the user's. When a frontier question needs a fact from the environment (filesystem, tools, etc.), dispatch a sub-agent to find it; don't ask the user for anything you could look up yourself. Don't block on it: a running exploration is an unsettled prerequisite, so only the questions downstream of it wait for the sub-agent to report; ask another question on the frontier now. The _decisions_ are the user's: put each to them and wait.

The session is done when the frontier is empty: every branch of the design tree visited, nothing left silently assumed. Do not act on it until the user confirms you have reached a shared understanding.

Then offer to capture it as a **self-contained GitHub issue** — it will be handed to an agent in a fresh container that never saw this conversation. Everything the interview settled goes in the issue text: the evidence, the decisions and the options you rejected, the change, and what done looks like from the outside. Follow `.github/ISSUE_TEMPLATE/task.md`. If the answers cover more than one session's work, offer one issue per session's work plus an epic (`.github/ISSUE_TEMPLATE/epic.md`), linked with `Part of #NNN`.
