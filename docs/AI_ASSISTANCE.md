# AI-assisted development disclosure

This Diddy Kong Racing recomp prototype has been developed with substantial assistance from OpenAI Codex.

## Division of work

Human direction and verification have included:

- choosing the project goals and acceptable legal boundary;
- supplying a privately owned ROM locally;
- repeated gameplay testing on macOS and Steam Deck;
- capturing visual artifacts, crash logs, performance changes, and controller behavior;
- deciding which regressions were acceptable and which required further investigation;
- approving distribution and sharing actions.

Codex assistance has included:

- source and assembly analysis;
- implementation across the application runtime and dependency patches;
- build-system and packaging work;
- crash-symbol and rendering diagnosis;
- test-harness construction;
- technical documentation.

The project should therefore be described as **substantially AI-assisted**, not merely spell-checked or lightly edited by AI.

## Quality policy

AI involvement neither proves nor disproves code quality. Changes are accepted based on evidence:

- a root-cause explanation tied to concrete code or runtime state;
- reviewable scope and comments for non-obvious behavior;
- clean builds from isolated directories;
- package inspection and private-ROM exclusion;
- deterministic smoke tests where possible;
- real gameplay verification on the affected platform and scene;
- removal of failed experiments and diagnostic scaffolding from release defaults.

No contributor should claim that an AI-generated change was manually authored. Equally, no change should be dismissed or accepted solely because AI was involved.

## Pull requests and upstream communication

Any PR materially produced with AI assistance should disclose that fact in its description and identify:

- what the tool contributed;
- what the submitter reviewed and understood;
- which tests were actually run;
- any uncertainty or untested platform remaining.

The submitter remains the responsible author for review purposes. Maintainers should never be asked to review a raw transcript-driven dump, speculative fix, or monolithic patch assembled without understanding its dependency boundaries.

Suggested disclosure:

> This change was developed with substantial assistance from OpenAI Codex. I directed the work and reviewed the resulting diff. Validation performed: [list exact builds/tests]. Remaining uncertainty: [list anything not exercised].

## Publication standard

Before this repository is presented as a maintained public project, the prototype worktree will be split into coherent commits, stale inherited documentation and CI will be replaced, dependency patches will be separated by upstream owner, and the documented test matrix will be rerun from a clean clone.
