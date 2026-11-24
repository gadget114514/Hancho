# Documents under doc/

This folder holds the plans, task cards, and reference documents for the current development line (0.10 series). Older documents that are no longer maintained are in [obsoleted/](obsoleted/), and the index for those is in the top-level [README.md](../README.md#documentation).

Rule: plans and task cards are written in English, and the English version is the source. When a Japanese version is needed, it is kept next to it as `-ja.md`. Japanese documents that have not been translated yet are marked below and will be translated the next time they are edited.

## Documents

| Document | Language | Contents |
| :--- | :--- | :--- |
| [ci.md](ci.md) | English | Using Hancho in CI (Jenkins, GitHub Actions, and similar): opt-in flags and environment variables |
| [ci-ja.md](ci-ja.md) | Japanese | The same as `ci.md`, in Japanese |
| [ci-cards.md](ci-cards.md) | Japanese | Task cards 105-1 to 105-6 for CI support |
| [demo.md](demo.md) | Japanese | Demo mode: a build that runs with fixed time and data for screenshots (not shipped) |
| [direction-cards.md](direction-cards.md) | Japanese | Direction records and the 0.10.0 name changes: task cards 103-1 to 103-8 |
| [fix-failure-engine-switch-plan.md](fix-failure-engine-switch-plan.md) | Japanese | Plan: switching the model after a failed fix (implemented) |
| [fix-engine-switch-cards.md](fix-engine-switch-cards.md) | Japanese | Task cards 101-1 to 101-6 for the model switch after a failed fix |
| [i18n-cards.md](i18n-cards.md) | Japanese | Store data in English and translate it when displayed: task cards 106-1 to 106-6 |
| [task-card-store-cards.md](task-card-store-cards.md) | Japanese | Saving task cards in Hancho: task cards 104-1 to 104-6 |
| [task-lineage-plan.md](task-lineage-plan.md) | Japanese | Plan: task and lineage (implemented) |
| [task-lineage-cards.md](task-lineage-cards.md) | Japanese | Task cards 100-2 to 100-10 for task and lineage |
| [task-lineage-100-1.md](task-lineage-100-1.md) | Japanese | Task card 100-1: the data layer for task lineage |
| [task-list-redesign-cards.md](task-list-redesign-cards.md) | Japanese | Redesign of the task list: task cards 102-1 to 102-10 |
| [terminology.md](terminology.md) | English | Glossary: task, direction, job, attempt, exec / agent, and the name changes |
| [terminology-ja.md](terminology-ja.md) | Japanese | The same glossary, in Japanese |

## Other folders

| Path | Contents |
| :--- | :--- |
| [images/](images/) | Screenshots and diagrams used in the README |
| [schema/](schema/) | JSON schema for the `hancho.event/1` event stream ([README.md](schema/README.md) in English, [README-ja.md](schema/README-ja.md) in Japanese) |
| [obsoleted/](obsoleted/) | Documents that are no longer maintained (kept for reference) |

## English status

English: `ci.md`, `terminology.md`, and `schema/README.md`. All other documents in this folder are Japanese for now.
