import unittest

from ane_cold_start import idle_ok, parse_libane, parse_strace, parse_worker

# Worker stderr from a real sealed session (m1-test-host, MLX_OMARCHY_OPEN_TIMING=1).
WORKER = """\
[omarchy-ane] sealed island-pv/program-0.anec bytes=182400 sha256=3ae3 seals=0xf
[omarchy-ane] timing bundle island-pv load 4.2 ms
[omarchy-ane] timing bundle parakeet-encoder-whole load 434.8 ms
[omarchy-ane] timing libane seal 0.1 ms
[omarchy-ane] child device factory 0.2 ms
[omarchy-ane] child load program 0 (7) 4.1 ms
[omarchy-ane] child load program 1 (14) 352.0 ms
[omarchy-ane] child loaded 2 programs
[omarchy-ane] parent open handshake 368.0 ms (fork to loaded)
"""

LIBANE = """\
LIBANE: TIMING stage=device_open ms=0.010 bytes=0
LIBANE: TIMING stage=model_read ms=0.100 bytes=180000
LIBANE: TIMING stage=bo_init ms=0.200 bytes=16384
LIBANE: TIMING stage=bo_init ms=0.300 bytes=16384
LIBANE: TIMING stage=init_total ms=1.000 bytes=180000
LIBANE: TIMING stage=device_open ms=0.020 bytes=0
LIBANE: TIMING stage=bo_init ms=150.000 bytes=458014720
LIBANE: TIMING stage=init_total ms=350.000 bytes=458014720
LIBANE: TIMING stage=exec ms=440.000 bytes=0
"""

STRACE = """\
1700000000.000001 openat(AT_FDCWD, "/dev/accel/accel0", O_RDWR|O_CLOEXEC) = 5 <0.000020>
1700000000.000100 ioctl(0x5, 0xc0406400, 0xfffff000) = 0 <0.000004>
1700000000.000200 ioctl(0x5, 0xc0186441, 0xfffff100) = 0 <0.120000>
1700000000.200000 mmap(NULL, 458014720, PROT_READ|PROT_WRITE, MAP_SHARED, 5, 0x100000) = 0xffff0000 <0.010000>
1700000000.300000 mmap(NULL, 458018816, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0xfffe0000 <0.000010>
1700000000.400000 ioctl(0x5, 0xc0186441, 0xfffff100) = 0 <0.001000>
1700000000.500000 mmap(NULL, 16384, PROT_READ|PROT_WRITE, MAP_SHARED, 5, 0x200000) = 0xfffd0000 <0.000005>
"""


class ParseTests(unittest.TestCase):
    def test_worker_stages_keyed_by_bundle_and_program(self):
        got = parse_worker(WORKER)
        self.assertEqual(got["seal:parakeet-encoder-whole"], 434.8)
        self.assertEqual(got["seal:island-pv"], 4.2)
        self.assertEqual(got["seal:libane"], 0.1)
        self.assertEqual(got["child_dlopen"], 0.2)
        self.assertEqual(got["ane_init:1"], 352.0)
        self.assertEqual(got["fork_to_loaded"], 368.0)
        self.assertNotIn("sealed", " ".join(got))

    def test_libane_lines_split_per_program_and_skip_exec(self):
        small, whole = parse_libane(LIBANE)
        self.assertAlmostEqual(small["bo_init"], 0.5)
        self.assertEqual(small["bytes"], 180000)
        self.assertEqual(whole["bo_init"], 150.0)
        self.assertEqual(whole["bytes"], 458014720)
        self.assertNotIn("exec", whole)

    def test_strace_classifies_drm_ioctls_and_large_shared_maps(self):
        got = parse_strace(STRACE)
        self.assertEqual(got["ioctl:drm_version"][0], 1)
        n, total, peak = got["ioctl:bo_init"]
        self.assertEqual(n, 2)
        self.assertAlmostEqual(total, 121.0)
        self.assertAlmostEqual(peak, 120.0)
        self.assertEqual(got["mmap>=1MiB:shared_fd"][0], 1)
        self.assertEqual(got["mmap>=1MiB:other"][0], 1)


class IdleGateTests(unittest.TestCase):
    PSI = "some avg10={} avg60=0.00 avg300=0.00 total=1\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"

    def test_both_conditions_must_hold(self):
        self.assertEqual(idle_ok("0.49 0.3 0.2 1/100 5", self.PSI.format("0.00")),
                         (True, 0.49, 0.0))
        self.assertFalse(idle_ok("0.50 0.3 0.2 1/100 5", self.PSI.format("0.00"))[0])
        self.assertFalse(idle_ok("0.10 0.3 0.2 1/100 5", self.PSI.format("0.01"))[0])

    def test_unreadable_inputs_fail_the_gate_without_raising(self):
        missing = "unavailable: [Errno 2] No such file or directory"
        self.assertEqual(idle_ok("0.10 0.1 0.1 1/100 5", missing), (False, None, None))
        self.assertEqual(idle_ok(missing, self.PSI.format("0.00")), (False, None, None))


if __name__ == "__main__":
    unittest.main()
