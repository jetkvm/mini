#!/usr/bin/env python3
"""Lock the board's SDMMC sampling policy and its pre-init application."""

from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "jetkvm_virtual_media.c").read_text(
    encoding="utf-8"
)

policy_start = SOURCE.index("static const char *configure_tf_input_delay")
policy_end = SOURCE.index("\n}\n\n#if JETKVM_TF_RAW_BOOT_SELFTEST", policy_start) + 2
policy = SOURCE[policy_start:policy_end]

assert "JETKVM_TF_RAW_DELAY_PHASE >= 0" in policy
assert "SDMMC_DELAY_PHASE_0 +" in policy
assert "JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_DDR50" in policy
assert "host->input_delay_phase = SDMMC_DELAY_PHASE_1;" in policy
assert "JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_SDR50" in policy
assert "JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_SDR104" in policy
assert policy.count("host->input_delay_phase = SDMMC_DELAY_PHASE_0;") == 2

configure_at = SOURCE.index("configure_tf_input_delay(&host)")
card_init_at = SOURCE.index("sdmmc_card_init(&host, card)", configure_at)
assert configure_at < card_init_at
assert "card->host.set_input_delay" not in SOURCE

print("TF SDMMC input-delay policy contract: PASS")
