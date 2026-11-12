#!/usr/bin/env python3
"""Sample external ranker plugin for Hancho memory search.

Protocol (doc/design-v3.md 3.8):
- Stdin: JSON object:
    {
      "query": "<search query>",
      "candidates": [
        {"id": "...", "kind": "...", "text": "...", "createdEpoch": 1731300000},
        ...
      ]
    }
- Stdout: JSON object or array with scores:
    {"scores": {"<candidate_id>": <score_float>, ...}}
    OR {"scores": [<score_float>, ...]}
    OR [<score_float>, ...]
  On error, stdout may return:
    {"error": "<error message>"}
"""

import json
import sys


def main():
    try:
        raw = sys.stdin.read()
        if not raw:
            json.dump({"scores": {}}, sys.stdout)
            return
        data = json.loads(raw)
    except Exception as exc:
        json.dump({"error": f"Failed to parse stdin JSON: {exc}"}, sys.stdout)
        return

    query = data.get("query", "").strip().lower()
    candidates = data.get("candidates", [])
    query_tokens = [t for t in query.split() if t]

    scores = {}
    for c in candidates:
        cid = c.get("id", "")
        text = c.get("text", "").lower()
        score = 0.0
        for token in query_tokens:
            if token in text:
                score += 10.0 + text.count(token)
        scores[cid] = score

    json.dump({"scores": scores}, sys.stdout)


if __name__ == "__main__":
    main()
