"""
Transmit a large message using a convolutional code and a CRC checksum.
"""
import random
import zlib

import correction


NUM_ERRORS = 64

conv = correction.Convolutional(2, 7)


def crc32(data):
    """Return the CRC-32 checksum as four bytes in network byte order."""
    return zlib.crc32(data).to_bytes(4, "big")


def transmit(message, rng):
    """Encode a message and simulate bit errors during transmission."""
    encoded = conv.encode(message + crc32(message))

    # Flip distinct bits at random positions in the encoded bit stream.
    for pos in rng.sample(range(len(encoded)), NUM_ERRORS):
        encoded.invert(pos)
    return encoded


def receive(encoded):
    """Decode a transmission and verify the recovered message."""
    data = conv.decode(encoded)
    if len(data) < 4:
        raise ValueError("message is missing its CRC checksum")

    message, expected_crc = data[:-4], data[-4:]
    if crc32(message) != expected_crc:
        raise ValueError("CRC checksum does not match")
    return message


if __name__ == "__main__":
    # The channel corruption is random but reproducible for this example.
    rng = random.Random(2026)
    text = b"Convolutional coding protects a stream from bit errors.\n"
    length = 10 * 1024
    message = (text * (length // len(text) + 1))[:length]

    encoded = transmit(message, rng)
    recovered = receive(encoded)

    print("message:    %d bytes" % len(message))
    print("encoded:    %d bits" % len(encoded))
    print("introduced: %d bit errors" % NUM_ERRORS)
    print("CRC-32:     %08x" % zlib.crc32(recovered))
    assert recovered == message
