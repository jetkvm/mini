# JetKVM HID input bridge

This component decouples WebRTC and control-protocol callbacks from `jetkvm_usb_hid`. Callers submit events with `jetkvm_hid_input_submit_event()` and `jetkvm_hid_input_submit_movement()`, which only copy the event and do a zero-tick queue send. A single worker pinned to CPU0 makes every USB call.

- Pure absolute-pointer moves cannot fill the last four queue slots. Once the queue reaches that level they are dropped, so motion never displaces a button-up, key-up or other state change.
- Every command carries a session generation (`jetkvm_hid_input_set_active_generation()`). On a generation change or `jetkvm_hid_input_release_all()`, the queue is flushed and a release command goes to the front, so a previous peer's keys and buttons never survive a reconnect. An internal epoch also rejects commands that raced with a release-all.
- Non-modifier `keypress` events release automatically after 100 ms unless a HIDRPC keepalive extends the deadline.
- Release-all retries once on a transient failure. While the host is suspended it stays pending and retries every 100 ms after resume, and no new input passes it. A detached host counts as released, because TinyUSB clears its state on detach.
- Keyboard macros are copied into a one-entry mailbox. The worker runs their steps between other commands without sleeping, so key-ups, cancellation and generation changes stay responsive. A new macro replaces any active or pending one. Replacement, cancellation and teardown all send an all-zero keyboard report first.
