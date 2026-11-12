@echo off
rem Hancho sample external ranker plugin (Windows batch)
rem Protocol: reads JSON from stdin: {"query": "...", "candidates": [...]}
rem           writes JSON to stdout: {"scores": {...}} or [score1, score2, ...]
rem
rem This sample assigns higher scores to matching items or provides sample scores.
echo {"scores": {"rr_2": 88.0, "rr_1": 12.0}}
