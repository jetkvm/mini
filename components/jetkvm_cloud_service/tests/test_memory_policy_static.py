#!/usr/bin/env python3

from pathlib import Path
import re


source = (Path(__file__).resolve().parents[1] / "jetkvm_cloud_service.c").read_text()
task_start = source.index("static void cloud_service_task")
task_end = source.index("esp_err_t jetkvm_cloud_service_start", task_start)
task = source[task_start:task_end]

assert "jetkvm_memory_policy_control_alloc(sizeof(*config))" in task
assert "jetkvm_memory_policy_control_free(config)" in task
assert "memset(config, 0, sizeof(*config))" in task
assert not re.search(r"\b(?:malloc|calloc|realloc|free)\s*\(", task)

print("cloud service config control-arena lifetime contract: PASS")
