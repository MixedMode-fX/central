---
name: Task
about: One session's work — a bug, or one phase-step of an epic
title: ''
labels: ''
---

<!--
This issue is the whole brief. It will be handed to an agent in a fresh
container that has never seen this conversation, has no Teensy attached, and
is read back on a phone. Anything you leave implicit gets guessed.
-->

## The evidence

<!-- What is wrong, where. Quote file:line, paste the failing assertion,
     attach the screenshot, name the algorithm and the parameter. If it only
     shows on hardware, say so — that decides whether this is finishable in a
     session at all. -->

## The change

<!-- What to do, in numbered steps if the order matters. Name the files.
     Say what NOT to do if there is an obvious wrong turn nearby — renumbering
     an algorithm id, hand-editing app/src/protocol.js, reaching for a pin
     from src/algorithm/. Scope creep is easier to prevent here than to
     review out later. -->

## Done when

<!-- Checkable from the outside, by someone who did not write it:
     - <the observable behaviour>
     - covered by a test under test/<suite>
     - screenshotted at 390px and 1440px, if it is visible in the app
     - `bash scripts/checks.sh` passes
-->

<!-- If this belongs to an epic, link it WITHOUT a closing keyword, so the
     epic survives this issue's merge:
Part of #NNN
-->
