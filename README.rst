correct
=======

``correct`` provides Python bindings for the convolutional and Reed-Solomon
forward error correction algorithms from `libcorrect`_.  Convolutional codes
use bitarrays for encoded data, while Reed-Solomon codes operate on bytes.

The package requires Python 3.9 or later and bitarray 3.4 or later.

Example
-------

Create a rate 1/2, order 7 convolutional code, encode a message, and decode it
again::

    >>> import correct
    >>> conv = correct.Convolutional(2, 7, (0o161, 0o127))
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

Installation
------------

Install the package from PyPI::

    python -m pip install libcorrect

Run the self-tests::

    python -m test_correct

The Python binding and the vendored libcorrect sources are distributed under
BSD licenses.  See ``LICENSE`` and ``libcorrect/LICENSE`` for details.

.. _libcorrect: https://github.com/quiet/libcorrect
