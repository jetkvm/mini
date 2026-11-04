#!/usr/bin/env python3

from pathlib import Path
import re


source = (Path(__file__).resolve().parents[1] / "jetkvm_oidc_esp.c").read_text()

create_start = source.index("jetkvm_oidc_status_t jetkvm_oidc_validator_create")
destroy_start = source.index("void jetkvm_oidc_validator_destroy")
validate_start = source.index("static jetkvm_oidc_status_t validate", destroy_start)
create = source[create_start:destroy_start]
destroy = source[destroy_start:validate_start]

assert "jetkvm_memory_policy_control_alloc(sizeof(*validator))" in create
assert "memset(validator, 0, sizeof(*validator))" in create
assert create.count("jetkvm_memory_policy_control_free(validator)") == 2
assert "jetkvm_memory_policy_control_free(validator)" in destroy
assert not re.search(r"\b(?:malloc|calloc|realloc|free)\s*\(", create + destroy)

print("OIDC validator control-arena lifetime contract: PASS")
