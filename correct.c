#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>

#include "correct.h"


static PyObject *BitarrayType;


typedef struct {
    PyObject_HEAD
    correct_convolutional *ctx;
    size_t inv_rate;
    size_t order;
} ConvObject;


typedef struct {
    PyObject_HEAD
    correct_reed_solomon *ctx;
    size_t num_roots;
} RSObject;


static int
get_buffer(PyObject *obj, Py_buffer *view)
{
    if (PyObject_GetBuffer(obj, view, PyBUF_CONTIG_RO) < 0)
        return -1;
    if (view->itemsize != 1) {
        PyBuffer_Release(view);
        PyErr_SetString(PyExc_TypeError,
                        "expected a bytes-like object with itemsize 1");
        return -1;
    }
    return 0;
}


static PyObject *
bitarray_from_bytes(PyObject *data, Py_ssize_t nbits)
{
    PyObject *res;
    Py_ssize_t total;

    res = PyObject_CallFunction(BitarrayType, "Os", data, "big");
    if (res == NULL)
        return NULL;

    total = PyObject_Length(res);
    if (total < 0)
        goto error;
    assert(total > 0 && total % 8 == 0);
    if (nbits < total && PySequence_DelSlice(res, nbits, total) < 0)
        goto error;

    return res;

 error:
    Py_DECREF(res);
    return NULL;
}


static int
get_codeword(PyObject *obj, Py_buffer *view, Py_ssize_t *nbits)
{
    PyObject *endian;
    int is_bitarray, is_big;

    is_bitarray = PyObject_IsInstance(obj, BitarrayType);
    if (is_bitarray < 0)
        return -1;
    if (is_bitarray == 0) {
        PyErr_SetString(PyExc_TypeError, "expected a bitarray");
        return -1;
    }

    endian = PyObject_GetAttrString(obj, "endian");
    if (endian == NULL)
        return -1;
    is_big = (PyUnicode_Check(endian) &&
              PyUnicode_CompareWithASCIIString(endian, "big") == 0);
    Py_DECREF(endian);
    if (!is_big) {
        PyErr_SetString(PyExc_ValueError,
                        "convolutional codeword must be big-endian");
        return -1;
    }

    *nbits = PyObject_Length(obj);
    if (*nbits < 0)
        return -1;
    return get_buffer(obj, view);
}


static void
conv_dealloc(ConvObject *self)
{
    if (self->ctx != NULL)
        correct_convolutional_destroy(self->ctx);
    Py_TYPE(self)->tp_free((PyObject *) self);
}


static int
conv_init(ConvObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"inv_rate", "order", "polynomials", NULL};
    Py_ssize_t inv_rate, order, i;
    PyObject *obj, *seq = NULL;
    correct_convolutional_polynomial_t *poly = NULL;
    correct_convolutional *ctx = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "nnO:Convolutional",
                                     kwlist, &inv_rate, &order, &obj))
        return -1;
    if (inv_rate <= 0) {
        PyErr_SetString(PyExc_ValueError, "inv_rate must be positive");
        return -1;
    }
    if (order <= 0 || order > 16) {
        PyErr_SetString(PyExc_ValueError, "order must be between 1 and 16");
        return -1;
    }

    seq = PySequence_Fast(obj, "polynomials must be a sequence");
    if (seq == NULL)
        return -1;
    if (PySequence_Fast_GET_SIZE(seq) != inv_rate) {
        PyErr_Format(PyExc_ValueError,
                     "expected %zd polynomials, got %zd", inv_rate,
                     PySequence_Fast_GET_SIZE(seq));
        goto error;
    }
    if ((size_t) inv_rate > SIZE_MAX / sizeof(*poly)) {
        PyErr_NoMemory();
        goto error;
    }
    poly = PyMem_Malloc((size_t) inv_rate * sizeof(*poly));
    if (poly == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    for (i = 0; i < inv_rate; i++) {
        unsigned long value = PyLong_AsUnsignedLong(
            PySequence_Fast_GET_ITEM(seq, i));
        if (value == -1 && PyErr_Occurred())
            goto error;
        if (value > UINT16_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "convolutional polynomial does not fit in 16 bits");
            goto error;
        }
        poly[i] = (correct_convolutional_polynomial_t) value;
    }

    ctx = correct_convolutional_create((size_t) inv_rate, (size_t) order,
                                       poly);
    if (ctx == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    if (self->ctx != NULL)
        correct_convolutional_destroy(self->ctx);
    self->ctx = ctx;
    self->inv_rate = (size_t) inv_rate;
    self->order = (size_t) order;
    PyMem_Free(poly);
    Py_DECREF(seq);
    return 0;

error:
    PyMem_Free(poly);
    Py_XDECREF(seq);
    return -1;
}


static PyObject *
conv_encode(ConvObject *self, PyObject *obj)
{
    Py_buffer msg;
    PyObject *data = NULL, *res;
    size_t nbits, actual, nbytes;

    if (get_buffer(obj, &msg) < 0)
        return NULL;
    nbits = correct_convolutional_encode_len(self->ctx, (size_t) msg.len);
    if (nbits > SIZE_MAX - 7) {
        PyBuffer_Release(&msg);
        return PyErr_NoMemory();
    }
    nbytes = (nbits + 7) / 8;
    if (nbytes > (size_t) PY_SSIZE_T_MAX) {
        PyBuffer_Release(&msg);
        return PyErr_NoMemory();
    }

    data = PyBytes_FromStringAndSize(NULL, (Py_ssize_t) nbytes);
    if (data == NULL) {
        PyBuffer_Release(&msg);
        return NULL;
    }
    actual = correct_convolutional_encode(
        self->ctx, (const uint8_t *) msg.buf, (size_t) msg.len,
        (uint8_t *) PyBytes_AS_STRING(data));
    PyBuffer_Release(&msg);
    if (actual > nbytes * 8) {
        Py_DECREF(data);
        PyErr_SetString(PyExc_RuntimeError,
                        "convolutional encoder returned an invalid length");
        return NULL;
    }

    res = bitarray_from_bytes(data, (Py_ssize_t) actual);
    Py_DECREF(data);
    return res;
}


static PyObject *
conv_decode(ConvObject *self, PyObject *obj)
{
    Py_buffer encoded;
    PyObject *res;
    Py_ssize_t nbits, capacity, expected;
    size_t decoded_bits;
    ssize_t length;

    if (get_codeword(obj, &encoded, &nbits) < 0)
        return NULL;
    if ((size_t) nbits % self->inv_rate) {
        PyBuffer_Release(&encoded);
        PyErr_Format(PyExc_ValueError,
                     "codeword length must be divisible by %zu",
                     self->inv_rate);
        return NULL;
    }
    decoded_bits = (size_t) nbits / self->inv_rate;
    if (decoded_bits < self->order + 1 ||
        (decoded_bits - self->order - 1) % 8) {
        PyBuffer_Release(&encoded);
        PyErr_SetString(PyExc_ValueError,
                        "invalid convolutional codeword length");
        return NULL;
    }
    expected = (Py_ssize_t) ((decoded_bits - self->order - 1) / 8);
    capacity = (Py_ssize_t) ((decoded_bits + 7) / 8);
    res = PyBytes_FromStringAndSize(NULL, capacity);
    if (res == NULL) {
        PyBuffer_Release(&encoded);
        return NULL;
    }
    length = correct_convolutional_decode(
        self->ctx, (const uint8_t *) encoded.buf, (size_t) nbits,
        (uint8_t *) PyBytes_AS_STRING(res));
    PyBuffer_Release(&encoded);
    if (length < 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_ValueError,
                        "could not decode convolutional codeword");
        return NULL;
    }
    if (length < expected || length > capacity) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_RuntimeError,
                        "convolutional decoder returned an invalid length");
        return NULL;
    }
    if (_PyBytes_Resize(&res, expected) < 0)
        return NULL;
    return res;
}


static PyObject *
conv_decode_soft(ConvObject *self, PyObject *obj)
{
    Py_buffer encoded;
    PyObject *res;
    Py_ssize_t capacity;
    ssize_t length;

    if (get_buffer(obj, &encoded) < 0)
        return NULL;
    if ((size_t) encoded.len % self->inv_rate) {
        PyBuffer_Release(&encoded);
        PyErr_Format(PyExc_ValueError,
                     "number of soft symbols must be divisible by %zu",
                     self->inv_rate);
        return NULL;
    }
    capacity = (Py_ssize_t) (((size_t) encoded.len / self->inv_rate + 7) / 8);
    res = PyBytes_FromStringAndSize(NULL, capacity);
    if (res == NULL) {
        PyBuffer_Release(&encoded);
        return NULL;
    }

    length = correct_convolutional_decode_soft(
        self->ctx, (const correct_convolutional_soft_t *) encoded.buf,
        (size_t) encoded.len, (uint8_t *) PyBytes_AS_STRING(res));
    PyBuffer_Release(&encoded);
    if (length < 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_ValueError,
                        "could not decode convolutional soft symbols");
        return NULL;
    }
    if (length > capacity) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_RuntimeError,
                        "convolutional decoder returned an invalid length");
        return NULL;
    }
    if (_PyBytes_Resize(&res, (Py_ssize_t) length) < 0)
        return NULL;

    return res;
}


static PyMethodDef conv_methods[] = {
    {"encode", (PyCFunction) conv_encode, METH_O,
     "encode(data) -> bitarray\n\nEncode a bytes-like object."},
    {"decode", (PyCFunction) conv_decode, METH_O,
     "decode(codeword) -> bytes\n\nDecode a big-endian bitarray."},
    {"decode_soft", (PyCFunction) conv_decode_soft, METH_O,
     "decode_soft(values) -> bytes\n\nDecode 8-bit soft-decision values."},
    {NULL}
};


static PyTypeObject Conv_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "correct.Convolutional",
    .tp_basicsize = sizeof(ConvObject),
    .tp_dealloc = (destructor) conv_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Convolutional(inv_rate, order, polynomials)",
    .tp_methods = conv_methods,
    .tp_init = (initproc) conv_init,
    .tp_new = PyType_GenericNew,
};


static void
rs_dealloc(RSObject *self)
{
    if (self->ctx != NULL)
        correct_reed_solomon_destroy(self->ctx);
    Py_TYPE(self)->tp_free((PyObject *) self);
}


static int
rs_init(RSObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {
        "num_roots", "primitive_polynomial", "first_root", "root_gap", NULL
    };
    Py_ssize_t num_roots;
    unsigned int primitive = 0x11d, first_root = 1, root_gap = 1;
    correct_reed_solomon *ctx;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "n|III:ReedSolomon",
                                     kwlist, &num_roots, &primitive,
                                     &first_root, &root_gap))
        return -1;
    if (num_roots <= 0 || num_roots >= 255) {
        PyErr_SetString(PyExc_ValueError,
                        "num_roots must be between 1 and 254");
        return -1;
    }
    if (primitive > UINT16_MAX || first_root > UINT8_MAX ||
        root_gap > UINT8_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "Reed-Solomon parameter is out of range");
        return -1;
    }

    ctx = correct_reed_solomon_create((uint16_t) primitive,
                                      (uint8_t) first_root,
                                      (uint8_t) root_gap,
                                      (size_t) num_roots);
    if (ctx == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    if (self->ctx != NULL)
        correct_reed_solomon_destroy(self->ctx);
    self->ctx = ctx;
    self->num_roots = (size_t) num_roots;
    return 0;
}


static PyObject *
rs_encode(RSObject *self, PyObject *obj)
{
    Py_buffer msg;
    PyObject *res;
    Py_ssize_t length;
    ssize_t status;

    if (get_buffer(obj, &msg) < 0)
        return NULL;
    if ((size_t) msg.len > 255 - self->num_roots) {
        PyBuffer_Release(&msg);
        PyErr_Format(PyExc_ValueError,
                     "message cannot exceed %zu bytes",
                     255 - self->num_roots);
        return NULL;
    }
    length = msg.len + (Py_ssize_t) self->num_roots;
    res = PyBytes_FromStringAndSize(NULL, length);
    if (res == NULL) {
        PyBuffer_Release(&msg);
        return NULL;
    }
    status = correct_reed_solomon_encode(
        self->ctx, (const uint8_t *) msg.buf, (size_t) msg.len,
        (uint8_t *) PyBytes_AS_STRING(res));
    PyBuffer_Release(&msg);
    if (status < 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_ValueError,
                        "could not encode Reed-Solomon block");
        return NULL;
    }
    return res;
}


static PyObject *
rs_decode(RSObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"encoded", "erasures", NULL};
    PyObject *obj, *erasures = Py_None, *seq = NULL, *res = NULL;
    Py_buffer encoded;
    uint8_t *locations = NULL;
    Py_ssize_t i, nerasures = 0, capacity;
    ssize_t length;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:decode", kwlist,
                                     &obj, &erasures))
        return NULL;
    if (get_buffer(obj, &encoded) < 0)
        return NULL;
    if ((size_t) encoded.len < self->num_roots || encoded.len > 255) {
        PyErr_Format(PyExc_ValueError,
                     "encoded block length must be between %zu and 255",
                     self->num_roots);
        goto error;
    }

    if (erasures != Py_None) {
        seq = PySequence_Fast(erasures, "erasures must be a sequence");
        if (seq == NULL)
            goto error;
        nerasures = PySequence_Fast_GET_SIZE(seq);
        if ((size_t) nerasures > self->num_roots) {
            PyErr_SetString(PyExc_ValueError,
                            "too many Reed-Solomon erasures");
            goto error;
        }
        locations = PyMem_Malloc((size_t) nerasures);
        if (locations == NULL && nerasures) {
            PyErr_NoMemory();
            goto error;
        }
        for (i = 0; i < nerasures; i++) {
            Py_ssize_t pos = PyLong_AsSsize_t(
                PySequence_Fast_GET_ITEM(seq, i));
            if (pos == -1 && PyErr_Occurred())
                goto error;
            if (pos < 0 || pos >= encoded.len) {
                PyErr_SetString(PyExc_ValueError,
                                "erasure position is out of range");
                goto error;
            }
            locations[i] = (uint8_t) pos;
        }
    }

    capacity = encoded.len - (Py_ssize_t) self->num_roots;
    res = PyBytes_FromStringAndSize(NULL, capacity);
    if (res == NULL)
        goto error;
    if (nerasures) {
        length = correct_reed_solomon_decode_with_erasures(
            self->ctx, (const uint8_t *) encoded.buf,
            (size_t) encoded.len, locations, (size_t) nerasures,
            (uint8_t *) PyBytes_AS_STRING(res));
    }
    else {
        length = correct_reed_solomon_decode(
            self->ctx, (const uint8_t *) encoded.buf,
            (size_t) encoded.len, (uint8_t *) PyBytes_AS_STRING(res));
    }
    if (length < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "could not decode Reed-Solomon block");
        goto error;
    }
    if (length > capacity) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Reed-Solomon decoder returned an invalid length");
        goto error;
    }
    if (_PyBytes_Resize(&res, (Py_ssize_t) length) < 0)
        goto error;

    PyBuffer_Release(&encoded);
    PyMem_Free(locations);
    Py_XDECREF(seq);
    return res;

error:
    PyBuffer_Release(&encoded);
    PyMem_Free(locations);
    Py_XDECREF(seq);
    Py_XDECREF(res);
    return NULL;
}


static PyMethodDef rs_methods[] = {
    {"encode", (PyCFunction) rs_encode, METH_O,
     "encode(data) -> bytes\n\nEncode one Reed-Solomon block."},
    {"decode", (PyCFunction) rs_decode, METH_VARARGS | METH_KEYWORDS,
     "decode(encoded, erasures=None) -> bytes\n\nDecode one Reed-Solomon block."},
    {NULL}
};


static PyTypeObject RS_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "correct.ReedSolomon",
    .tp_basicsize = sizeof(RSObject),
    .tp_dealloc = (destructor) rs_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "ReedSolomon(num_roots, primitive_polynomial=0x11d, "
              "first_root=1, root_gap=1)",
    .tp_methods = rs_methods,
    .tp_init = (initproc) rs_init,
    .tp_new = PyType_GenericNew,
};


static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name = "correct",
    .m_doc = "Forward error correction using libcorrect.",
    .m_size = -1,
};


PyMODINIT_FUNC
PyInit_correct(void)
{
    PyObject *module, *bitarray;

    if (PyType_Ready(&Conv_Type) < 0 || PyType_Ready(&RS_Type) < 0)
        return NULL;

    bitarray = PyImport_ImportModule("bitarray");
    if (bitarray == NULL)
        return NULL;
    BitarrayType = PyObject_GetAttrString(bitarray, "bitarray");
    Py_DECREF(bitarray);
    if (BitarrayType == NULL)
        return NULL;

    module = PyModule_Create(&moduledef);
    if (module == NULL)
        return NULL;

    Py_INCREF(&Conv_Type);
    if (PyModule_AddObject(module, "Convolutional",
                           (PyObject *) &Conv_Type) < 0) {
        Py_DECREF(&Conv_Type);
        goto error;
    }
    Py_INCREF(&RS_Type);
    if (PyModule_AddObject(module, "ReedSolomon",
                           (PyObject *) &RS_Type) < 0) {
        Py_DECREF(&RS_Type);
        goto error;
    }
    return module;

 error:
    Py_DECREF(module);
    return NULL;
}
