#!/usr/bin/env python3
"""Tests for decoded FIFO formulas only; not an end-to-end device model."""
import unittest

from rtqueue_spec import (
    acknowledged_producer_index,
    advance_index,
    producer_word_after_send,
    receive_slot_matches,
    received_message_id,
    release_slot_word,
    send_phase_word,
)


class QueueFormulaTests(unittest.TestCase):
    def test_index_wrap_and_validation(self):
        self.assertEqual([advance_index(i, 4) for i in range(4)], [1, 2, 3, 0])
        with self.assertRaises(ValueError):
            advance_index(4, 4)

    def test_post_send_announcement_formula(self):
        self.assertEqual(producer_word_after_send(2, 2), 0)
        self.assertEqual(producer_word_after_send(2, 1), 0xFFFFFFFD)
        self.assertEqual(producer_word_after_send(0, 1), 0xFFFFFFFF)

    def test_safety_guard_rejects_unacknowledged_words(self):
        self.assertEqual(acknowledged_producer_index(2, 4), 2)
        for word in (0xFFFFFFFF, 0xFFFFFFFD, 4):
            with self.subTest(word=hex(word)), self.assertRaises(ValueError):
                acknowledged_producer_index(word, 4)

    def test_phase_predicate_and_release_sign_extension(self):
        self.assertTrue(receive_slot_matches(0x101, 3))
        self.assertFalse(receive_slot_matches(0x100, 3))
        self.assertEqual(release_slot_word(0), 1)
        self.assertEqual(release_slot_word(1), 0)
        self.assertEqual(release_slot_word(0xFFFFFFFF), 0xFFFFFFFFFFFFFFFE)

    def test_send_word_and_received_id_mask(self):
        self.assertEqual(send_phase_word(0x104, 0), 0x105)
        self.assertEqual(send_phase_word(0x105, 1), 0x105)
        self.assertEqual(received_message_id(0x105), 0x104)
        self.assertEqual(received_message_id(0x107), 0x104)


if __name__ == "__main__":
    unittest.main(verbosity=2)
