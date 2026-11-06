#!/usr/bin/env python3
"""Pure offline helpers for individually decoded J700/H17 FIFO rules.

This deliberately does not simulate a complete producer/consumer exchange:
peer initialization and acknowledgment ownership remain unresolved. Source
contract: firmware-load-path receipt §13.2, host kernelcache SHA-256
7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe.
"""

U32_MASK = 0xFFFFFFFF
U64_MASK = 0xFFFFFFFFFFFFFFFF


def advance_index(index: int, entries: int) -> int:
    """Host primitive's wrap-at-entries-minus-one rule."""
    if entries <= 0 or not 0 <= index < entries:
        raise ValueError("invalid FIFO index/entry count")
    return 0 if index == entries - 1 else index + 1


def producer_word_after_send(new_producer: int, consumer: int) -> int:
    """Decoded host store: plain zero if equal, otherwise ~new_producer."""
    if not 0 <= new_producer <= U32_MASK or not 0 <= consumer <= U32_MASK:
        raise ValueError("producer/consumer must be u32")
    return 0 if consumer == new_producer else (~new_producer) & U32_MASK


def acknowledged_producer_index(word: int, entries: int) -> int:
    """Linux safety guard; accepts only a plain peer-acknowledged index.

    This is deliberately stricter than Apple's low-level send primitive. It
    refuses inverted announcement values rather than attempting a send from
    an uncertain control-block state.
    """
    if entries <= 0 or not 0 <= word < entries:
        raise ValueError("producer is not a plain acknowledged FIFO index")
    return word


def send_phase_word(message_id: int, phase_word: int) -> int:
    """Encode the host send slot word, retaining ID/flags except phase bit."""
    if not 0 <= message_id <= U64_MASK:
        raise ValueError("message ID must be u64")
    phase = phase_word & U32_MASK
    signed_phase = phase if phase < 0x80000000 else phase - 0x100000000
    return (message_id | ((signed_phase ^ 1) & U64_MASK)) & U64_MASK


def receive_slot_matches(slot_word: int, phase_word: int) -> bool:
    """Decoded host receive availability test: compare only phase bit 0."""
    return (slot_word & 1) == (phase_word & 1)


def release_slot_word(phase_word: int) -> int:
    """Decoded receive release value: sxtw(phase) XOR 1, stored as u64."""
    phase = phase_word & U32_MASK
    signed_phase = phase if phase < 0x80000000 else phase - 0x100000000
    return (signed_phase ^ 1) & U64_MASK


def received_message_id(slot_word: int) -> int:
    """The host receive primitive clears both low bits from the stored word."""
    return slot_word & U64_MASK & ~0x3
