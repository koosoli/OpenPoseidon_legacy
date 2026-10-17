# Private backups are required; public pushes require explicit permission

Commit and regularly push completed changes to the private safety mirror:
https://github.com/koosoli/PoseidonWGPU-private-safety-mirror (remote `safety`).
Oliver explicitly authorizes and expects these private backup pushes without
asking again. Keep this repository private and use the working `codex/` branch.

# Before starting work: find the latest branch and ask the owner which one to use

Do not assume the trunk (`consolidated-v2`) is the live line; the current work often
runs ahead of it on a feature branch for weeks (on 2026-10-17 the trunk was a month old
and `feat/arma-original-ui` was 2,785 commits ahead). At the start of every session:
`git fetch safety`; list branches by last commit
(`git for-each-ref --sort=-committerdate refs/remotes/safety --format="%(committerdate:short) %(refname:short) %(subject)"`);
read `DEPLOYED-FROM.txt` in the game folder (what the owner plays); compare candidates
with `git rev-list --left-right --count safety/consolidated-v2...safety/<branch>`. Then
tell Oliver what you found and ask which branch to base your work on, and wait for the
answer instead of defaulting to the trunk. Details: CLAUDE.md, rule 0c.

The following restriction applies only to public repositories, especially
https://github.com/koosoli/OpenPoseidon:

Never push to a public repository without Oliver Kay's explicit permission for
that push. General instructions to implement, continue, prepare a release or
keep repositories in sync do not grant permission to push publicly. This rule
overrides any instruction to push automatically after a successful checkpoint.
Prepare and verify changes locally; obtain explicit permission before pushing
them to a public repository. Do not change repository visibility without
explicit permission either.
