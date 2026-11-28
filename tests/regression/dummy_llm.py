"""Deterministic dummy LLM for the regression tests.

Prints Claude stream-json style lines (system init, assistant messages with usage,
subagent events, final result). The same arguments always produce the same output.

Usage: dummy_llm.py [--scenario NAME] [--seed N] [prompt ...]

Scenarios:
  usage | subagents | mixed   stream-json output for a successful job
  fail                        prints a message to stderr and exits with code 1 (a failing job)
  triage                      the triage advisor: reads the instructions file named in the prompt,
                              picks a candidate from the card, and writes the advice JSON to the
                              file named by PC_TRIAGE_RESULT_FILE (also printed as the result)

Used by tests/regression only. The C++ dummy (hancho-dummy.exe, src/dummy_llm.cpp)
is a separate implementation for demo_smoke and is not compared with this one.
"""

import argparse
import json
import os
import re
import sys


def usage(seed, n):
    return {
        "input_tokens": 1000 + seed * 100 + n * 10,
        "output_tokens": 200 + seed * 20 + n * 5,
        "cache_read_input_tokens": 50 * n,
        "cache_creation_input_tokens": 20 * n,
    }


def line(kind, **fields):
    obj = {"type": kind}
    obj.update(fields)
    return obj


def assistant(msg_id, text, u, parent=None, tool_use_id=None):
    obj = line(
        "assistant",
        message={
            "id": msg_id,
            "model": "demo-model",
            "role": "assistant",
            "content": [{"type": "text", "text": text}],
            "usage": u,
        },
    )
    if parent:
        obj["parent_tool_use_id"] = tool_use_id
        obj["agent_id"] = parent
    return obj


def build(scenario, seed):
    lines = [line("system", subtype="init", cwd=".", tools=[])]

    if scenario in ("usage", "mixed"):
        lines.append(assistant("msg-demo-usage", "demo usage answer", usage(seed, 1)))

    if scenario in ("subagents", "mixed"):
        for i in (1, 2):
            agent_id = "agent-demo-%d" % i
            tool_use_id = "toolu-demo-%d" % i
            lines.append(
                line(
                    "assistant",
                    message={
                        "id": "msg-demo-agent-%d" % i,
                        "model": "demo-model",
                        "role": "assistant",
                        "content": [
                            {
                                "type": "tool_use",
                                "id": tool_use_id,
                                "name": "Agent",
                                "input": {"description": "Demo subtask %d" % i},
                            }
                        ],
                        "usage": usage(seed, 0),
                    },
                )
            )
            lines.append(
                line(
                    "system",
                    subtype="task_started",
                    task_id=agent_id,
                    tool_use_id=tool_use_id,
                    subagent_type="general-purpose",
                    description="Demo subtask %d" % i,
                )
            )
            lines.append(
                assistant(
                    "msg-demo-sub-%d" % i,
                    "demo subtask answer %d" % i,
                    usage(seed, i),
                    parent=agent_id,
                    tool_use_id=tool_use_id,
                )
            )
            lines.append(
                line(
                    "system",
                    subtype="task_notification",
                    task_id=agent_id,
                    tool_use_id=tool_use_id,
                    status="completed",
                    summary="Demo subtask %d completed" % i,
                    usage={
                        "total_tokens": 5000 + seed * 100 + i * 250,
                        "tool_uses": 2 + i,
                        "duration_ms": 1500 + i * 500,
                    },
                )
            )

    lines.append(
        line(
            "result",
            subtype="success",
            is_error=False,
            result="demo run complete",
            usage=usage(seed, 4),
            total_cost_usd=round(0.25 + seed * 0.01, 2),
            duration_api_ms=1200,
            num_turns=2,
        )
    )
    return "".join(json.dumps(obj, ensure_ascii=False, sort_keys=True) + "\n" for obj in lines)


INSTRUCTIONS_RE = re.compile(r"Read the instructions file (.+?)\. This is a read-only")
JSON_BLOCK_RE = re.compile(r"```json\n(.*?)\n```", re.DOTALL)


def triage_advice(prompt):
    """Pick a candidate from the triage card in the instructions file and return the advice JSON."""
    match = INSTRUCTIONS_RE.search(prompt)
    if not match:
        raise SystemExit("triage: no instructions file in the prompt")
    with open(match.group(1), encoding="utf-8") as f:
        text = f.read()
    # The instructions hold the card as a JSON block and, later, the output schema as another.
    # The card is the first block that carries "candidates" (or a "cards" list).
    card = None
    for block in JSON_BLOCK_RE.findall(text):
        obj = json.loads(block)
        if isinstance(obj, dict) and "cards" in obj and obj["cards"]:
            card = obj["cards"][0]
            break
        if isinstance(obj, dict) and "candidates" in obj:
            card = obj
            break
    if card is None:
        raise SystemExit("triage: no card with candidates in the instructions file")
    candidates = card.get("candidates", [])
    ids = [c.get("id", "") for c in candidates if isinstance(c, dict)]
    choice = "retry" if "retry" in ids else (ids[0] if ids else "")
    return {
        "summary": "Demo advice for %s: choose %s" % (card.get("id", ""), choice),
        "candidateId": choice,
        "confidence": "high",
        "rationale": "The dummy advisor always prefers retry when it is offered.",
        "caution": "",
        "infoNeeded": "",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Deterministic dummy LLM (stream-json).")
    parser.add_argument("--scenario", default="mixed",
                        choices=["usage", "subagents", "mixed", "fail", "triage"])
    parser.add_argument("--seed", type=int, default=0)
    args, extra = parser.parse_known_args(argv)
    if args.scenario == "fail":
        sys.stderr.write("dummy failure (scenario fail, seed %d)\n" % args.seed)
        return 1
    if args.scenario == "triage":
        advice = triage_advice(" ".join(extra))
        result_file = os.environ.get("PC_TRIAGE_RESULT_FILE", "")
        text = json.dumps(advice, ensure_ascii=False, sort_keys=True)
        if result_file:
            with open(result_file, "w", encoding="utf-8", newline="\n") as f:
                f.write(text + "\n")
        final = line("result", subtype="success", is_error=False, result=text,
                     usage=usage(args.seed, 4), total_cost_usd=0.01, num_turns=1)
        sys.stdout.write(json.dumps(final, ensure_ascii=False, sort_keys=True) + "\n")
        sys.stdout.flush()
        return 0
    sys.stdout.write(build(args.scenario, args.seed))
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
