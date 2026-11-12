#!/bin/sh
# Hancho sample external ranker plugin (POSIX shell)
# Protocol: reads JSON from stdin: {"query": "...", "candidates": [...]}
#           writes JSON to stdout: {"scores": {...}} or [score1, score2, ...]

echo '{"scores": {"rr_2": 88.0, "rr_1": 12.0}}'
