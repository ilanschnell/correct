import unittest
from array import array

from bitarray import bitarray

import correct


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

    def test_roundtrip(self):
        for inv_rate, order, polynomials in CONV_CODES:
            conv = correct.Convolutional(inv_rate, order, polynomials)
            for msg in (b'', b'a', b'Hello world!', bytes(range(256))):
                encoded = conv.encode(msg)
                self.assertIs(type(encoded), bitarray)
                self.assertEqual(encoded.endian, 'big')
                self.assertEqual(len(encoded) % inv_rate, 0)
                self.assertEqual(conv.decode(encoded), msg)

    def test_correct_error(self):
        conv = correct.Convolutional(2, 7, (0o161, 0o127))
        msg = b'convolutional code'
        encoded = conv.encode(msg)
        for i in (0, 1, len(encoded) // 2, len(encoded) - 1):
            damaged = encoded.copy()
            damaged.invert(i)
            self.assertEqual(conv.decode(damaged), msg)

    def test_soft_decoding(self):
        for inv_rate, order, polynomials in CONV_CODES:
            conv = correct.Convolutional(inv_rate, order, polynomials)
            for msg in (b'', b'a', b'soft decisions'):
                encoded = conv.encode(msg)
                soft = bytearray(255 if bit else 0 for bit in encoded)
                self.assertEqual(conv.decode_soft(soft), msg)

        conv = correct.Convolutional(2, 7, (0o161, 0o127))
        encoded = conv.encode(b'soft decisions')
        soft = bytearray(255 if bit else 0 for bit in encoded)
        soft[len(soft) // 2] = 128
        self.assertEqual(conv.decode_soft(soft), b'soft decisions')

    def test_buffers(self):
        conv = correct.Convolutional(2, 7, (0o161, 0o127))
        self.assertEqual(conv.decode(conv.encode(bytearray(b'abc'))), b'abc')
        self.assertEqual(conv.decode(conv.encode(memoryview(b'abc'))), b'abc')

        with self.assertRaises(TypeError):
            conv.encode(array('H', [1, 2]))

    def test_invalid_codeword(self):
        conv = correct.Convolutional(2, 7, (0o161, 0o127))
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
        for inv_rate in (0, 1):
            with self.assertRaises(ValueError):
                correct.Convolutional(inv_rate, 7, ())
        for order in (0, 17):
            with self.assertRaises(ValueError):
                correct.Convolutional(2, order, (0o161, 0o127))
        with self.assertRaises(ValueError):
            correct.Convolutional(2, 7, (0o161,))
        for polynomial in (-1, 1 << 16):
            with self.assertRaises(OverflowError):
                correct.Convolutional(2, 7, (polynomial, 0o127))
        with self.assertRaises(TypeError):
            correct.Convolutional(2, 7, ('x', 0o127))


class ReedSolomonTests(unittest.TestCase):

    def setUp(self):
        self.rs = correct.ReedSolomon(32)

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
                correct.ReedSolomon(num_roots)
        with self.assertRaises(OverflowError):
            correct.ReedSolomon(32, primitive_polynomial=1 << 16)
        with self.assertRaises(OverflowError):
            correct.ReedSolomon(32, first_root=256)
        with self.assertRaises(OverflowError):
            correct.ReedSolomon(32, root_gap=256)


if __name__ == '__main__':
    unittest.main()
