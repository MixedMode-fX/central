---
name: explain-diff-html
description: Use when the user asks for a rich explanation of a code change, diff, branch, or PR. Produces a self-contained interactive HTML page, publishes it on the pr-diff-explained branch, and posts the permalink as a comment on the pull request.
---

<!-- Geoffrey Litt's `explain-diff-html` skill (his wording throughout):
     https://gist.github.com/geoffreylitt/a29df1b5f9865506e8952488eac3d524
     The argument behind it: "Understanding is the new bottleneck",
     https://www.geoffreylitt.com/2026/07/02/understanding-is-the-new-bottleneck

     Changed here, for a repository driven from a phone: the page is not left
     in /tmp on somebody's laptop. It is committed to the long-lived
     `pr-diff-explained` branch and linked from a comment on the pull request,
     so the reader gets a URL they can open where they are actually reading. -->

# Explain Diff

Please make me a rich, interactive explanation of the specified code change.

## Which change

If you already have the change in context — you just wrote it, or the user
pointed at a branch, a diff or a PR — explain that, and skip to the next
section.

**If you were invoked cold, with no change in context, do not guess.** Ask
which pull request to explain, and offer the recent ones as the choices rather
than asking an open question:

```bash
gh pr list --limit 10 --state all   # or: mcp__github__list_pull_requests
```

Then read that PR properly before writing a word about it: its title and body,
its full diff, and the files it touches at their current state on the branch.
An explanation written from a diff alone gets the *what* right and the *why*
wrong.

## The screenshots

A change you look at is explained twice as fast with a picture of it, and this
workflow has already taken them: a visual change here is photographed at phone
and desktop width before its pull request is opened. So before writing, go and
find them.

- `.dev/screenshots/` — the shots from the session that made the change, if
  you are still in that session. This is the common case: you wrote the
  change, you already sent the pictures to the chat.
- The branch itself, for a change that commits its own images:
  `git diff --name-only origin/main...HEAD | grep -Ei '\.(png|jpe?g|gif|webp|svg)$'`
- Neither, the change is visual, and the branch is checked out — take them
  now, with the `app-screenshots` skill.

None of the three, or a change that does not show on a screen? Skip this
entirely. An explanation of a shell script does not need a picture of the
page.

**Two or three of them, the ones that carry the argument** — the before and
the after of the thing that moved — not the contents of the directory. Copy
those next to the draft, into a directory named after it, and rename each for
what it shows:

```
/tmp/2026-01-12-pr-42-scrollspy.html
/tmp/2026-01-12-pr-42-scrollspy/
    before-phone.png
    after-phone.png
```

Reference them by that relative path and no other:

```html
<figure>
  <img src="2026-01-12-pr-42-scrollspy/after-phone.png" alt="The contents rail with Checks lit"
       style="width: 100%; max-width: 390px; height: auto;">
  <figcaption>After: the section you are actually in is the one lit in the rail.</figcaption>
</figure>
```

`publish.sh` puts that directory in the same commit as the page, so the
relative path resolves under the rendered link, the raw one and the permalink
alike — and the shots survive the review branch being deleted at merge, which
a link into it would not. Two things to hold to:

- **A phone shot is 390 pixels wide and stays that way.** `max-width` at its
  natural size; a screenshot stretched across a desktop window is a blurry
  claim about a layout nobody has.
- **A screenshot is evidence, not an explanation.** It shows that the change
  lands; the diagram beside it is what says why. Caption every one with what
  to look at in it, and write the `alt` text for the reader who gets no image
  at all.

## The explanation

It should have these sections:

- Background: Explain the existing system relevant to this change. (You should broadly explore surrounding code for this.) We don't know how much the reader already knows, so include a deep background for beginners (note that it can be skipped if the reader is already familiar), and then a more narrow background directly relevant to the change.
- Intuition: Explain the core intuition for the code change. The focus here is to explain the essence, not the full details. Use concrete examples with toy data. Use figures and diagrams liberally.
- Code: Do a high-level walkthrough of the changes to the code. Group/order the changes in an understandable way.
- Quiz: Come up with five questions that test the reader's knowledge of this PR. This should be medium difficulty, difficult enough that you actually need to understand the substance of the PR to answer them, but not gotchas. The goal is to help the reader make sure that they've actually understood. These should be presented as interactive multiple-choice questions, and when the user clicks, it tells them whether they were correct and gives feedback.

Format:

- Output a single self-contained HTML file which includes CSS and JavaScript — the screenshot directory beside it is the one thing that lives outside the file. Make the whole thing one long page with section headers and a table of contents. Don't use tabs for the top-level structure. Basic responsive styling so you can view it on a phone is nice too.
- **Write the draft outside the repository** — the scratchpad directory, or
  `/tmp` — so the working tree you are mid-change in stays clean. The
  published copy is put on its own branch in the next section, never
  committed to the branch under review. Name it with today's date first, so
  the collection stays time-sorted:
  `YYYY-MM-DD-pr-<number>-<slug>.html`, e.g.
  `/tmp/2026-01-12-pr-42-scrollspy.html`. For a change with no PR behind it,
  `YYYY-MM-DD-explanation-<slug>.html`.
- Please write with the clarity and flow of Martin Kleppmann, making it engaging and written in classic style. Transitions between sections should be smooth.
- Some tips on diagrams. Ideally, you should pick a small number of diagram families that can be reused throughout the explanation to explain various cases. Some useful kinds of diagrams:
  - A very simplified version of the UI that the user sees in the app, to explain UI changes.
  - A system diagram showing data flow or communication between components. Make sure to include example data here!
- Don't use ASCII diagrams. Always use simple HTML designs for your diagrams, HTML lists for lists of things, etc.
  - For code blocks, always use `<pre>` tags. If you use a custom styled div instead, it **must** have
    `white-space: pre-wrap` in its CSS, or the browser will collapse all newlines into a single line.
    Before saving the file, scan each code block in the HTML source and confirm its CSS includes
    `white-space: pre` or `pre-wrap`.

## Publish it

```bash
bash .claude/skills/explain-diff-html/scripts/publish.sh <file.html> [pr-number]
```

One commit is appended to `pr-diff-explained`, a branch that holds nothing but
these pages and their screenshots. The script creates it as a true orphan — no
history, no `src/`, no `README` — the first time it runs, and pushes it. Your
working tree, index and current branch are untouched, so this is safe to run
in the middle of the change you are explaining.

The directory named after the page rides along in the same commit; anything in
it that is not an image is skipped, and the script says which.

**The branch is never merged.** It is a collection, not a proposal: it
accumulates one page per pull request and stays open. `git log
origin/pr-diff-explained` is its index — there is no list to keep up to date,
and therefore no list to let rot.

The script prints, as `key=url` lines:

| Key | What it is |
| --- | --- |
| `permalink` | the file pinned to the commit SHA — the canonical link |
| `preview` | the same file rendered, via rawcdn.githack.com |
| `raw` | the raw file, if you want to fetch it back |

It also prints `assets=<n>`, the number of screenshots published with the
page. A zero there on a visual change means the directory was named something
other than the page, and the reader is about to meet three broken images.

## Comment it on the PR

Post the links to the pull request, so the reader finds the explanation where
the review already is:

```bash
gh pr comment <number> --body "$(cat <<'EOF'
📖 **Explanation of this PR** — background, intuition, a code walkthrough and a five-question quiz.

- [Read it rendered](<preview>)
- [Permalink](<permalink>) (pinned to `<sha>`, on the `pr-diff-explained` branch)

---
_Generated by [Claude Code](https://claude.ai/code)_
EOF
)"
```

In a web or mobile session there is no `gh`: use the GitHub MCP tool for a
pull request comment instead (`mcp__github__add_issue_comment` — a PR comment
*is* an issue comment), with the same body.

Lead with the rendered link. `permalink` shows HTML source on a phone, which
is not what anybody clicked for; `preview` is the one that reads.

Use the URLs the script printed, verbatim, and do not substitute a viewer of
your own. A rendered link addressed as `?https://github.com/...` carries a
whole URL inside its own, and a comment pipeline that reads that shape as an
open redirect wraps it in backticks: posted, and tappable by nobody. Then read
the comment back once and check the link came out a link.
