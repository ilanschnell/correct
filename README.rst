correction
==========

``correction`` provides Python bindings for the convolutional and Reed-Solomon
forward error correction algorithms from `libcorrect`_.  Convolutional codes
use bitarrays for encoded data, while Reed-Solomon codes operate on bytes.

The package requires Python 3.9 or later and bitarray 3.4 or later.

Installation
------------

Install the package from PyPI::

    python -m pip install correction

Run the self-tests::

    python -m test_correction


Convolutional codes
-------------------

A convolutional encoder turns each input bit into several output bits.  The
``inv_rate`` argument is the number of output bits per input bit: ``2`` gives
a rate 1/2 code, and ``3`` gives a rate 1/3 code.  A lower rate adds more
redundancy.

The ``order`` is the width, in bits, of the encoder's shift register.  It
controls how much preceding input affects each output.  Larger orders can
provide stronger codes, but decoding cost and memory use increase rapidly.

The optional generator ``polynomials`` are bit masks which select the
shift-register bits XORed to produce each output bit.  When supplied, there
must be exactly ``inv_rate`` polynomials.  They are conventionally written in
octal.  For example::

    conv = correction.Convolutional(2, 7)

This creates a rate 1/2, order 7 code: each input bit produces two output bits,
using the recommended generator polynomials ``161`` and ``127`` (octal).  It
is equivalent to::

    conv = correction.Convolutional(2, 7, (0o161, 0o127))

When ``polynomials`` is omitted, the following configurations are selected
automatically.  They are recommended by libcorrect and exercised by the test
suite:

======  =====  ==========================
Rate    Order  Polynomials (octal)
======  =====  ==========================
1/2     6      ``(0o73, 0o61)``
1/2     7      ``(0o161, 0o127)``
1/2     8      ``(0o225, 0o373)``
1/2     9      ``(0o767, 0o545)``
1/3     6      ``(0o53, 0o75, 0o47)``
1/3     7      ``(0o137, 0o153, 0o121)``
1/3     8      ``(0o333, 0o257, 0o351)``
1/3     9      ``(0o417, 0o627, 0o675)``
======  =====  ==========================

Other rates, orders, and generator polynomials are also possible.  The
implementation builds its lookup tables from the supplied polynomials and is
not limited to the configurations above.  However, not every combination
produces a useful code; poorly chosen polynomials can provide weak error
correction or even produce a catastrophic code.

``Convolutional.encode(data)``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Encode a bytes-like object and return a big-endian ``bitarray.bitarray``.  The
encoder appends ``order + 1`` zero steps to terminate the code.  The exact
encoded length is therefore::

    inv_rate * (8 * len(data) + order + 1)

The result may contain a number of bits which is not divisible by eight;
bitarray preserves that length without exposing byte-padding details.

The following example encodes a message, damages three bits, and recovers the
original data::

    >>> import correction
    >>> conv = correction.Convolutional(2, 7, (0o161, 0o127))
    >>> encoded = conv.encode(b"Hello world!")
    >>> encoded.endian
    'big'
    >>> len(encoded)
    208
    >>> for i in 97, 123, 198:
    ...   encoded.invert(i)
    >>> conv.decode(encoded)
    b'Hello world!'

``encoded`` is a big-endian ``bitarray.bitarray`` and can be stored,
transmitted, or modified using the normal bitarray operations.

``Convolutional.decode(codeword)``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Decode a big-endian bitarray produced by a compatible convolutional code and
return ``bytes``.  A little-endian bitarray is rejected because its bit order
would give the codeword a different meaning.

Convolutional decoding cannot reliably detect when corruption exceeds the
code's ability to recover the original message.  Protect the decoded data with
a checksum or CRC when silent corruption matters.

``Convolutional.decode_soft(values)``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Soft decoding accepts one unsigned byte for every encoded bit.  Each value
expresses confidence in the received bit:

* ``0`` means a definite zero;
* ``255`` means a definite one;
* ``128`` means an unknown or erased bit;
* intermediate values express intermediate confidence.

The argument may be any contiguous bytes-like object with one-byte elements.
For example, convert a hard codeword to soft symbols and erase one symbol::

    >>> encoded = conv.encode(b"soft decisions")
    >>> soft = bytearray(255 if bit else 0 for bit in encoded)
    >>> soft[len(soft) // 2] = 128
    >>> conv.decode_soft(soft)
    b'soft decisions'


Reed-Solomon codes
------------------

``correction.ReedSolomon(num_roots, primitive_polynomial=0x11d,
first_root=1, root_gap=1)`` creates a Reed-Solomon code over 8-bit symbols.  A
full encoded block contains at most 255 bytes.

``num_roots`` is the number of parity bytes added to every message and must be
between 1 and 254.  A block can therefore contain at most
``255 - num_roots`` message bytes.  With no erasure information, up to
``num_roots // 2`` corrupted bytes can be corrected.

The other constructor arguments define the finite field and generator roots.
The defaults use the primitive polynomial
``x^8 + x^4 + x^3 + x^2 + 1``.  Change these advanced parameters only when
interoperating with a protocol or implementation that specifies different
values; not every combination produces a useful code.

``ReedSolomon.encode(data)``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Encode a bytes-like object and return ``bytes`` containing the original data
followed by ``num_roots`` parity bytes.  Short messages use virtual leading
padding, which is not included in the result::

    >>> rs = correction.ReedSolomon(32)
    >>> encoded = rs.encode(b"Reed-Solomon")
    >>> len(encoded)
    44
    >>> encoded[:12]
    b'Reed-Solomon'
    >>> rs.decode(encoded)
    b'Reed-Solomon'

``ReedSolomon.decode(encoded, erasures=None)``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Decode an encoded bytes-like object and return the original message as
``bytes``.  The optional ``erasures`` argument is a sequence of byte positions
known to be unreliable.  A known erasure consumes one parity byte of the
correction capacity, while an error at an unknown position consumes two.  In
general, recovery is possible when::

    2 * unknown_errors + erasures <= num_roots

For example, mark several corrupted bytes as erasures::

    >>> encoded = bytearray(rs.encode(b"known erasures"))
    >>> erasures = [0, 4, 8]
    >>> for i in erasures:
    ...   encoded[i] ^= 0xff
    >>> rs.decode(encoded, erasures=erasures)
    b'known erasures'

As with convolutional decoding, sufficiently severe corruption is not always
detectable.  Use a checksum or CRC when the decoded message must be verified.


The Python binding and the vendored libcorrect sources are distributed under
BSD licenses.  See ``LICENSE`` and ``libcorrect/LICENSE`` for details.

.. _libcorrect: https://github.com/quiet/libcorrect
