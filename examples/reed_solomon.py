"""
Transmit a large message using Reed-Solomon blocks and a CRC checksum.
"""
import random
import zlib

import correction


NUM_ROOTS = 32
PAYLOAD_SIZE = 255 - NUM_ROOTS
ERRORS_PER_BLOCK = 8

rs = correction.ReedSolomon(NUM_ROOTS)


def crc32(data):
    """Return the CRC-32 checksum as four bytes in network byte order."""
    return zlib.crc32(data).to_bytes(4, "big")


def transmit(message, rng):
    """Encode a message and simulate transmission errors."""
    data = message + crc32(message)
    blocks = []

    for start in range(0, len(data), PAYLOAD_SIZE):
        block = bytearray(rs.encode(data[start:start + PAYLOAD_SIZE]))

        # Corrupt distinct byte positions.  With 32 parity bytes, each block
        # can correct up to 16 errors at unknown positions.
        for pos in rng.sample(range(len(block)), ERRORS_PER_BLOCK):
            block[pos] ^= rng.randrange(1, 256)
        blocks.append(bytes(block))

    return blocks


def receive(blocks):
    """Decode transmitted blocks and verify the complete message."""
    data = b"".join(rs.decode(block) for block in blocks)
    if len(data) < 4:
        raise ValueError("message is missing its CRC checksum")

    message, expected_crc = data[:-4], data[-4:]
    if crc32(message) != expected_crc:
        raise ValueError("CRC checksum does not match")
    return message


if __name__ == "__main__":
    # The channel corruption is random but reproducible for this example.
    rng = random.Random(2026)
    text = b"Reed-Solomon protects data from transmission errors.\n"
    length = 10 * 1024
    message = (text * (length // len(text) + 1))[:length]

    blocks = transmit(message, rng)
    recovered = receive(blocks)

    print("message:    %d bytes" % len(message))
    print("blocks:     %d" % len(blocks))
    print("introduced: %d byte errors" % (len(blocks) * ERRORS_PER_BLOCK))
    print("CRC-32:     %08x" % zlib.crc32(recovered))
    assert recovered == message
