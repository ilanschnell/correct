import sys
import unittest
from array import array
from concurrent.futures import ThreadPoolExecutor
from sysconfig import get_config_var
from threading import Barrier

from bitarray import bitarray

import correction


GIL_DISABLED = get_config_var("Py_GIL_DISABLED") in (1, "1")

# -------------------------  Convolutional codes  ------------------------

CONV_CODES = [
    (2, 6, (0o73, 0o61)),
    (2, 7, (0o161, 0o127)),
    (2, 8, (0o225, 0o373)),
    (2, 9, (0o767, 0o545)),
    (3, 6, (0o53, 0o75, 0o47)),
    (3, 7, (0o137, 0o153, 0o121)),
    (3, 8, (0o333, 0o257, 0o351)),
    (3, 9, (0o417, 0o627, 0o675)),
]

class ConvolutionalTests(unittest.TestCase):

    def test_repr(self):
        mv, ms = 0o177777, "0o177777"
        self.assertEqual(mv, 65535)
        for args, s in [
            ((2, 7, None),
             "Convolutional(2, 7, (0o161, 0o127))"),
            ((2, 5, (0o35, 0o23)),
             "Convolutional(2, 5, (0o35, 0o23))"),
            ((2, 4, (0o0, 0o123456)),
             "Convolutional(2, 4, (0o0, 0o123456))"),
            ((25, 10, 25 * (mv,)),
             "Convolutional(25, 10, (%s))" % ", ".join(25 * [ms])),
            ((26, 10, 26 * (mv,)),
             "Convolutional(26, 10, (%s, ...))" % ", ".join(25 * [ms])),
        ]:
            s = "correction." + s
            conv = correction.Convolutional(*args)
            self.assertEqual(repr(conv), s)
            if "..." in s:
                with self.assertRaises(TypeError):
                    eval(s)
            else:
                self.assertEqual(repr(eval(s)), s)

    def test_roundtrip(self):
        for inv_rate, order, polynomials in CONV_CODES:
            conv = correction.Convolutional(inv_rate, order, polynomials)
            for msg in (b'', b'a', b'Hello world!', bytes(range(256))):
                encoded = conv.encode(msg)
                self.assertIs(type(encoded), bitarray)
                self.assertEqual(encoded.endian, 'big')
                self.assertEqual(len(encoded) % inv_rate, 0)
                self.assertEqual(conv.decode(encoded), msg)

    def test_default_polynomials(self):
        msg = b'default convolutional code'
        for inv_rate, order, polynomials in CONV_CODES:
            explicit = correction.Convolutional(inv_rate, order, polynomials)
            expected = explicit.encode(msg)
            for default in [correction.Convolutional(inv_rate, order),
                            correction.Convolutional(inv_rate, order, None)]:
                encoded = default.encode(msg)
                self.assertEqual(encoded, expected)
                self.assertEqual(default.decode(encoded), msg)

    def test_correct_error(self):
        conv = correction.Convolutional(2, 7, (0o161, 0o127))
        msg = b'convolutional code'
        encoded = conv.encode(msg)
        for i in (0, 1, len(encoded) // 2, len(encoded) - 1):
            damaged = encoded.copy()
            damaged.invert(i)
            self.assertEqual(conv.decode(damaged), msg)

    def test_soft_decoding(self):
        for inv_rate, order, polynomials in CONV_CODES:
            conv = correction.Convolutional(inv_rate, order, polynomials)
            for msg in (b'', b'a', b'soft decisions'):
                encoded = conv.encode(msg)
                soft = bytearray(255 if bit else 0 for bit in encoded)
                self.assertEqual(conv.decode_soft(soft), msg)

        conv = correction.Convolutional(2, 7, (0o161, 0o127))
        encoded = conv.encode(b'soft decisions')
        soft = bytearray(255 if bit else 0 for bit in encoded)
        soft[len(soft) // 2] = 128
        self.assertEqual(conv.decode_soft(soft), b'soft decisions')

    def test_buffers(self):
        conv = correction.Convolutional(2, 7, (0o161, 0o127))
        self.assertEqual(conv.decode(conv.encode(bytearray(b'abc'))), b'abc')
        self.assertEqual(conv.decode(conv.encode(memoryview(b'abc'))), b'abc')

        with self.assertRaises(TypeError):
            conv.encode(array('H', [1, 2]))

    def test_invalid_codeword(self):
        conv = correction.Convolutional(2, 7, (0o161, 0o127))
        encoded = conv.encode(b'abc')

        with self.assertRaises(TypeError):
            conv.decode(encoded.tobytes())

        little = bitarray(encoded, endian='little')
        with self.assertRaises(ValueError):
            conv.decode(little)

        del encoded[-1]
        with self.assertRaises(ValueError):
            conv.decode(encoded)
        with self.assertRaises(ValueError):
            conv.decode_soft(bytearray(len(encoded)))

    def test_invalid_parameters(self):
        for inv_rate, order in [(2, 5), (2, 10), (3, 5), (3, 10), (4, 7)]:
            with self.assertRaisesRegex(ValueError,
                                        "polynomials must be specified"):
                correction.Convolutional(inv_rate, order)
        for inv_rate in (0, 1):
            with self.assertRaises(ValueError):
                correction.Convolutional(inv_rate, 7, ())
        for order in (0, 17):
            with self.assertRaises(ValueError):
                correction.Convolutional(2, order, (0o161, 0o127))
        with self.assertRaises(ValueError):
            correction.Convolutional(2, 7, (0o161,))
        for polynomial in (-1, 1 << 16):
            with self.assertRaises(OverflowError):
                correction.Convolutional(2, 7, (polynomial, 0o127))
        with self.assertRaises(TypeError):
            correction.Convolutional(2, 7, ('x', 0o127))

# --------------------------  Reed-Solomon codes  ------------------------

class ReedSolomonTests(unittest.TestCase):

    def setUp(self):
        self.rs = correction.ReedSolomon(32)

    def test_repr(self):
        self.assertEqual(repr(self.rs),
                         "correction.ReedSolomon(32, field_poly=0x11d, "
                         "first_root=1, root_gap=1)")

        rs = correction.ReedSolomon(16, 0x187, 2, 3)
        self.assertEqual(repr(rs),
                         "correction.ReedSolomon(16, field_poly=0x187, "
                         "first_root=2, root_gap=3)")

    def test_roundtrip(self):
        for msg in (b'', b'a', b'Hello world!', bytes(range(223))):
            encoded = self.rs.encode(msg)
            self.assertIs(type(encoded), bytes)
            self.assertEqual(len(encoded), len(msg) + 32)
            self.assertEqual(encoded[:len(msg)], msg)
            self.assertEqual(self.rs.decode(encoded), msg)

    def test_correct_errors(self):
        msg = bytes(range(223))
        encoded = self.rs.encode(msg)
        for count in (1, 8, 16):
            damaged = bytearray(encoded)
            for i in range(count):
                damaged[7 * i] ^= 0xff
            self.assertEqual(self.rs.decode(damaged), msg)

    def test_erasures(self):
        msg = bytes(range(100))
        damaged = bytearray(self.rs.encode(msg))
        erasures = list(range(20))
        for i in erasures:
            damaged[i] ^= 0xff
        self.assertEqual(self.rs.decode(damaged, erasures), msg)
        self.assertEqual(self.rs.decode(damaged, erasures=erasures), msg)

    def test_buffers(self):
        encoded = self.rs.encode(bytearray(b'abc'))
        self.assertEqual(self.rs.decode(encoded, []), b'abc')
        self.assertEqual(self.rs.decode(memoryview(encoded)), b'abc')
        with self.assertRaises(TypeError):
            self.rs.encode(array('H', [1, 2]))

    def test_invalid_blocks(self):
        with self.assertRaises(ValueError):
            self.rs.encode(bytes(224))
        with self.assertRaises(ValueError):
            self.rs.decode(bytes(31))
        with self.assertRaises(ValueError):
            self.rs.decode(bytes(256))

        encoded = self.rs.encode(b'abc')
        with self.assertRaises(ValueError):
            self.rs.decode(encoded, range(33))
        for pos in (-1, len(encoded)):
            with self.assertRaises(ValueError):
                self.rs.decode(encoded, [pos])
        with self.assertRaises(TypeError):
            self.rs.decode(encoded, ['x'])

    def test_invalid_parameters(self):
        for num_roots in (0, 255):
            with self.assertRaises(ValueError):
                correction.ReedSolomon(num_roots)
        with self.assertRaises(OverflowError):
            correction.ReedSolomon(32, field_poly=1 << 16)
        with self.assertRaises(OverflowError):
            correction.ReedSolomon(32, first_root=256)
        with self.assertRaises(OverflowError):
            correction.ReedSolomon(32, root_gap=256)

# ----------------------------  Free-threading  --------------------------

NTHREADS = 4

def run_concurrently(func):
    """Call func simultaneously in several threads and return the results."""
    barrier = Barrier(NTHREADS)

    def worker(index):
        barrier.wait()
        return func(index)

    with ThreadPoolExecutor(max_workers=NTHREADS) as executor:
        futures = [executor.submit(worker, i) for i in range(NTHREADS)]
        return [future.result() for future in futures]

@unittest.skipUnless(GIL_DISABLED, "requires free-threaded Python")
class FreeThreadingTests(unittest.TestCase):

    def test_gil_disabled(self):
        self.assertFalse(sys._is_gil_enabled())

    def test_convolutional(self):
        conv = correction.Convolutional(2, 7, (0o161, 0o127))

        def work(index):
            for count in range(10):
                msg = bytes((index + count + i) % 256 for i in range(64))
                encoded = conv.encode(msg)
                if conv.decode(encoded) != msg:
                    return False
                soft = bytearray(255 if bit else 0 for bit in encoded)
                if conv.decode_soft(soft) != msg:
                    return False
            return True

        self.assertEqual(run_concurrently(work), NTHREADS * [True])

    def test_reed_solomon(self):
        rs = correction.ReedSolomon(32)

        def work(index):
            for count in range(10):
                msg = bytes((index + count + i) % 256 for i in range(100))
                encoded = rs.encode(msg)
                damaged = bytearray(encoded)
                damaged[(index + count) % len(damaged)] ^= 0xff
                if rs.decode(damaged) != msg:
                    return False
            return True

        self.assertEqual(run_concurrently(work), NTHREADS * [True])


if __name__ == '__main__':
    print("correction version:", correction.__version__)
    print("GIL_DISABLED:", GIL_DISABLED)
    print("DEBUG: %s" % getattr(correction, "DEBUG", 0))
    unittest.main()
