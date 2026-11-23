#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "correct.h"


#define CORRECTION_VERSION  "0.4.0"


static PyObject *BitarrayType;

#ifdef Py_GIL_DISABLED
static PyMutex conv_create_mutex;
#define LOCK_CODEC(obj)    PyMutex_Lock(&(obj)->mutex)
#define UNLOCK_CODEC(obj)  PyMutex_Unlock(&(obj)->mutex)
#else
#define LOCK_CODEC(obj)
#define UNLOCK_CODEC(obj)
#endif


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
    assert(0 <= nbits && nbits <= total);
    if (nbits < total && PySequence_DelSlice(res, nbits, total) < 0)
        goto error;

    return res;

 error:
    Py_DECREF(res);
    return NULL;
}

/* -------------------------  Convolutional codes  ----------------------- */

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


typedef struct {
    PyObject_HEAD
    correct_convolutional *ctx;
    size_t inv_rate;
    size_t order;
#ifdef Py_GIL_DISABLED
    PyMutex mutex;
#endif
} ConvObject;


static void
conv_dealloc(ConvObject *self)
{
    if (self->ctx)
        correct_convolutional_destroy(self->ctx);
    Py_TYPE(self)->tp_free((PyObject *) self);
}


static correct_convolutional_polynomial_t *
alloc_default_polynomials(Py_ssize_t inv_rate, Py_ssize_t order)
{
    static const correct_convolutional_polynomial_t *const defaults[2][4] = {
        {correct_conv_r12_6_polynomial, correct_conv_r12_7_polynomial,
         correct_conv_r12_8_polynomial, correct_conv_r12_9_polynomial},
        {correct_conv_r13_6_polynomial, correct_conv_r13_7_polynomial,
         correct_conv_r13_8_polynomial, correct_conv_r13_9_polynomial},
    };
    correct_convolutional_polynomial_t *poly;
    size_t nbytes;

    if (inv_rate < 2 || inv_rate > 3 || order < 6 || order > 9) {
        PyErr_Format(PyExc_ValueError, "polynomials must be specified "
                     "for inv_rate %zd and order %zd", inv_rate, order);
        return NULL;
    }

    nbytes = (size_t) inv_rate * sizeof(*poly);
    poly = PyMem_Malloc(nbytes);
    if (poly == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    memcpy(poly, defaults[inv_rate - 2][order - 6], nbytes);
    return poly;
}


static correct_convolutional_polynomial_t *
alloc_custom_polynomials(Py_ssize_t inv_rate, PyObject *obj)
{
    correct_convolutional_polynomial_t *poly = NULL;
    PyObject *seq;
    Py_ssize_t i;

    seq = PySequence_Fast(obj, "polynomials must be a sequence");
    if (seq == NULL)
        return NULL;

    if (PySequence_Fast_GET_SIZE(seq) != inv_rate) {
        PyErr_Format(PyExc_ValueError, "expected %zd polynomials, got %zd",
                     inv_rate, PySequence_Fast_GET_SIZE(seq));
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
        if (value == (unsigned long) -1 && PyErr_Occurred())
            goto error;
        if (value > UINT16_MAX) {
            PyErr_SetString(PyExc_OverflowError, "convolutional "
                            "polynomial does not fit in 16 bits");
            goto error;
        }
        poly[i] = (correct_convolutional_polynomial_t) value;
    }
    Py_DECREF(seq);
    return poly;

 error:
    PyMem_Free(poly);
    Py_DECREF(seq);
    return NULL;
}


static correct_convolutional *
create_conv_context(size_t inv_rate, size_t order,
                    correct_convolutional_polynomial_t *poly)
{
    correct_convolutional *ctx;

#ifdef Py_GIL_DISABLED
    PyMutex_Lock(&conv_create_mutex);
#endif
    ctx = correct_convolutional_create(inv_rate, order, poly);
#ifdef Py_GIL_DISABLED
    PyMutex_Unlock(&conv_create_mutex);
#endif
    return ctx;
}


static int
conv_init(ConvObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"inv_rate", "order", "polynomials", NULL};
    Py_ssize_t inv_rate, order;
    PyObject *obj = Py_None;
    correct_convolutional_polynomial_t *poly;
    correct_convolutional *ctx = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "nn|O:Convolutional",
                                     kwlist, &inv_rate, &order, &obj))
        return -1;
    if (inv_rate < 2) {
        PyErr_SetString(PyExc_ValueError, "inv_rate must be at least 2");
        return -1;
    }
    if (order <= 0 || order > 16) {
        PyErr_SetString(PyExc_ValueError, "order must be between 1 and 16");
        return -1;
    }

    if (obj == Py_None) {
        poly = alloc_default_polynomials(inv_rate, order);
    }
    else {
        poly = alloc_custom_polynomials(inv_rate, obj);
    }
    if (poly == NULL)
        return -1;

    ctx = create_conv_context((size_t) inv_rate, (size_t) order, poly);
    PyMem_Free(poly);
    if (ctx == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    if (self->ctx)
        correct_convolutional_destroy(self->ctx);
    self->ctx = ctx;
    self->inv_rate = (size_t) inv_rate;
    self->order = (size_t) order;
    return 0;
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

    LOCK_CODEC(self);
    actual = correct_convolutional_encode(
        self->ctx, (const uint8_t *) msg.buf, (size_t) msg.len,
        (uint8_t *) PyBytes_AS_STRING(data));
    UNLOCK_CODEC(self);

    PyBuffer_Release(&msg);
    assert(actual == nbits);
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
conv_decode_impl(ConvObject *self, const Py_buffer *encoded,
                 Py_ssize_t nsymbols, int soft)
{
    PyObject *res;
    Py_ssize_t capacity, expected;
    size_t decoded_bits;
    ssize_t length;

    if ((size_t) nsymbols % self->inv_rate) {
        PyErr_Format(PyExc_ValueError,
                     "%s must be divisible by %zu",
                     soft ? "number of soft symbols" : "codeword length",
                     self->inv_rate);
        return NULL;
    }
    decoded_bits = (size_t) nsymbols / self->inv_rate;
    if (decoded_bits < self->order + 1 ||
        (decoded_bits - self->order - 1) % 8) {
        PyErr_Format(PyExc_ValueError,
                     "invalid convolutional %s length",
                     soft ? "soft-symbol" : "codeword");
        return NULL;
    }
    expected = (Py_ssize_t) ((decoded_bits - self->order - 1) / 8);
    capacity = (Py_ssize_t) ((decoded_bits + 7) / 8);
    res = PyBytes_FromStringAndSize(NULL, capacity);
    if (res == NULL)
        return NULL;

    LOCK_CODEC(self);
    if (soft) {
        length = correct_convolutional_decode_soft(
            self->ctx,
            (const correct_convolutional_soft_t *) encoded->buf,
            (size_t) nsymbols, (uint8_t *) PyBytes_AS_STRING(res));
    }
    else {
        length = correct_convolutional_decode(
            self->ctx, (const uint8_t *) encoded->buf,
            (size_t) nsymbols, (uint8_t *) PyBytes_AS_STRING(res));
    }
    UNLOCK_CODEC(self);

    if (length < 0) {
        Py_DECREF(res);
        PyErr_Format(PyExc_ValueError, "could not decode convolutional %s",
                     soft ? "soft symbols" : "codeword");
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
conv_decode(ConvObject *self, PyObject *obj)
{
    Py_buffer encoded;
    PyObject *res;
    Py_ssize_t nbits;

    if (get_codeword(obj, &encoded, &nbits) < 0)
        return NULL;
    res = conv_decode_impl(self, &encoded, nbits, 0);
    PyBuffer_Release(&encoded);
    return res;
}


static PyObject *
conv_decode_soft(ConvObject *self, PyObject *obj)
{
    Py_buffer encoded;
    PyObject *res;

    if (get_buffer(obj, &encoded) < 0)
        return NULL;
    res = conv_decode_impl(self, &encoded, encoded.len, 1);
    PyBuffer_Release(&encoded);
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
    .tp_name = "correction.Convolutional",
    .tp_basicsize = sizeof(ConvObject),
    .tp_dealloc = (destructor) conv_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Convolutional(inv_rate, order, polynomials=None)",
    .tp_methods = conv_methods,
    .tp_init = (initproc) conv_init,
    .tp_new = PyType_GenericNew,
};

/* --------------------------  Reed-Solomon codes  ----------------------- */

typedef struct {
    PyObject_HEAD
    correct_reed_solomon *ctx;
    size_t num_roots;
#ifdef Py_GIL_DISABLED
    PyMutex mutex;
#endif
} RSObject;


static void
rs_dealloc(RSObject *self)
{
    if (self->ctx)
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
    if (self->ctx)
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

    LOCK_CODEC(self);
    status = correct_reed_solomon_encode(
        self->ctx, (const uint8_t *) msg.buf, (size_t) msg.len,
        (uint8_t *) PyBytes_AS_STRING(res));
    UNLOCK_CODEC(self);

    PyBuffer_Release(&msg);
    if (status < 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_ValueError,
                        "could not encode Reed-Solomon block");
        return NULL;
    }
    return res;
}


static uint8_t *
alloc_erasure_locations(size_t num_roots, Py_ssize_t len,
                        PyObject *erasures, Py_ssize_t *nerasures)
{
    uint8_t *locations = NULL;
    PyObject *seq;
    Py_ssize_t n;               /* number of erasures */
    Py_ssize_t i;

    seq = PySequence_Fast(erasures, "erasures must be a sequence");
    if (seq == NULL)
        return NULL;

    n = PySequence_Fast_GET_SIZE(seq);
    if ((size_t) n > num_roots) {
        PyErr_SetString(PyExc_ValueError, "too many Reed-Solomon erasures");
        goto error;
    }
    /* allocate at least one byte so NULL unambiguously indicates failure */
    locations = PyMem_Malloc((size_t) Py_MAX(n, 1));
    if (locations == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    for (i = 0; i < n; i++) {
        Py_ssize_t pos = PyLong_AsSsize_t(PySequence_Fast_GET_ITEM(seq, i));

        if (pos == -1 && PyErr_Occurred())
            goto error;

        if (pos < 0 || pos >= len) {
            PyErr_SetString(PyExc_ValueError,
                            "erasure position is out of range");
            goto error;
        }
        locations[i] = (uint8_t) pos;
    }
    Py_DECREF(seq);
    *nerasures = n;
    return locations;

 error:
    PyMem_Free(locations);
    Py_DECREF(seq);
    return NULL;
}


static PyObject *
rs_decode(RSObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"encoded", "erasures", NULL};
    PyObject *obj, *erasures = Py_None, *res = NULL;
    Py_buffer encoded;
    uint8_t *locations = NULL;
    Py_ssize_t nerasures = 0, capacity;
    ssize_t length;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:decode", kwlist,
                                     &obj, &erasures))
        return NULL;
    if (get_buffer(obj, &encoded) < 0)
        return NULL;
    if ((size_t) encoded.len < self->num_roots || encoded.len > 255) {
        PyErr_Format(PyExc_ValueError, "encoded block length must be "
                     "between %zu and 255", self->num_roots);
        goto error;
    }

    if (erasures != Py_None) {
        locations = alloc_erasure_locations(self->num_roots, encoded.len,
                                            erasures, &nerasures);
        if (locations == NULL)
            goto error;
    }

    capacity = encoded.len - (Py_ssize_t) self->num_roots;
    res = PyBytes_FromStringAndSize(NULL, capacity);
    if (res == NULL)
        goto error;

    LOCK_CODEC(self);
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
    UNLOCK_CODEC(self);

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
    return res;

error:
    PyBuffer_Release(&encoded);
    PyMem_Free(locations);
    Py_XDECREF(res);
    return NULL;
}


static PyMethodDef rs_methods[] = {
    {"encode", (PyCFunction) rs_encode, METH_O,
     "encode(data) -> bytes\n\nEncode one Reed-Solomon block."},
    {"decode", (PyCFunction) rs_decode, METH_VARARGS | METH_KEYWORDS,
     "decode(encoded, erasures=None) -> bytes\n\nDecode one Reed-Solomon "
     "block."},
    {NULL}
};


static PyTypeObject RS_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "correction.ReedSolomon",
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
    .m_name = "correction",
    .m_doc = "Forward error correction using libcorrect.",
    .m_size = -1,
};


static int
add_type(PyObject *module, const char *name, PyTypeObject *type)
{
    Py_INCREF(type);
    if (PyModule_AddObject(module, name, (PyObject *) type) < 0) {
        Py_DECREF(type);
        return -1;
    }
    return 0;
}


PyMODINIT_FUNC
PyInit_correction(void)
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

    if (PyModule_AddStringConstant(module, "__version__",
                                   CORRECTION_VERSION) < 0)
        goto error;

#ifdef Py_GIL_DISABLED
    if (PyUnstable_Module_SetGIL(module, Py_MOD_GIL_NOT_USED) < 0)
        goto error;
#endif

#ifndef NDEBUG
    if (PyModule_AddIntConstant(module, "DEBUG", 1) < 0)
        goto error;
#endif

    if (add_type(module, "Convolutional", &Conv_Type) < 0 ||
        add_type(module, "ReedSolomon", &RS_Type) < 0)
        goto error;
    return module;

 error:
    Py_DECREF(module);
    return NULL;
}
