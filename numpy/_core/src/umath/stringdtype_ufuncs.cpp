/* Ufunc implementations for the StringDType class */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE
#define _UMATHMODULE

#include "numpy/arrayobject.h"
#include "numpy/ndarraytypes.h"
#include "numpy/npy_math.h"
#include "numpy/ufuncobject.h"

#include "numpyos.h"
#include "gil_utils.h"
#include "dtypemeta.h"
#include "abstractdtypes.h"
#include "dispatching.h"
#include "string_ufuncs.h"
#include "stringdtype_ufuncs.h"
#include "string_buffer.h"
#include "string_fastsearch.h"
#include "templ_common.h" /* for npy_mul_size_with_overflow_size_t */

#include "stringdtype/static_string.h"
#include "stringdtype/dtype.h"
#include "stringdtype/utf8_utils.h"

#define LOAD_TWO_INPUT_STRINGS(CONTEXT)                                            \
        const npy_packed_static_string *ps1 = (npy_packed_static_string *)in1;     \
        npy_static_string s1 = {0, NULL};                                          \
        int s1_isnull = NpyString_load(s1allocator, ps1, &s1);                     \
        const npy_packed_static_string *ps2 = (npy_packed_static_string *)in2;     \
        npy_static_string s2 = {0, NULL};                                          \
        int s2_isnull = NpyString_load(s2allocator, ps2, &s2);                     \
        if (s1_isnull == -1 || s2_isnull == -1) {                                  \
            npy_gil_error(PyExc_MemoryError, "Failed to load string in %s",        \
                          CONTEXT);                                                \
            goto fail;                                                             \
        }                                                                          \


static NPY_CASTING
multiply_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *dtypes[], PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[], npy_intp *NPY_UNUSED(view_offset))
{
    PyArray_Descr *ldescr = given_descrs[0];
    PyArray_Descr *rdescr = given_descrs[1];
    PyArray_StringDTypeObject *odescr = NULL;
    PyArray_Descr *out_descr = NULL;

    if (dtypes[0] == &PyArray_StringDType) {
        odescr = (PyArray_StringDTypeObject *)ldescr;
    }
    else {
        odescr = (PyArray_StringDTypeObject *)rdescr;
    }

    if (given_descrs[2] == NULL) {
        out_descr = (PyArray_Descr *)new_stringdtype_instance(
                odescr->na_object, odescr->coerce, NULL);
        if (out_descr == NULL) {
            return (NPY_CASTING)-1;
        }
    }
    else {
        Py_INCREF(given_descrs[2]);
        out_descr = given_descrs[2];
    }

    Py_INCREF(ldescr);
    loop_descrs[0] = ldescr;
    Py_INCREF(rdescr);
    loop_descrs[1] = rdescr;
    loop_descrs[2] = out_descr;

    return NPY_NO_CASTING;
}

template <typename T>
static int multiply_loop_core(
        npy_intp N, char *sin, char *iin, char *out,
        npy_intp s_stride, npy_intp i_stride, npy_intp o_stride,
        PyArray_StringDTypeObject *idescr, PyArray_StringDTypeObject *odescr)
{
    PyArray_Descr *descrs[2] =
            {(PyArray_Descr *)idescr, (PyArray_Descr *)odescr};
    npy_string_allocator *allocators[2] = {};
    NpyString_acquire_allocators(2, descrs, allocators);
    npy_string_allocator *iallocator = allocators[0];
    npy_string_allocator *oallocator = allocators[1];
    int has_null = idescr->na_object != NULL;
    int has_nan_na = idescr->has_nan_na;
    int has_string_na = idescr->has_string_na;
    const npy_static_string *default_string = &idescr->default_string;

    while (N--) {
        const npy_packed_static_string *ips =
                (npy_packed_static_string *)sin;
        npy_static_string is = {0, NULL};
        npy_packed_static_string *ops = (npy_packed_static_string *)out;
        int is_isnull = NpyString_load(iallocator, ips, &is);
        if (is_isnull == -1) {
            npy_gil_error(PyExc_MemoryError,
                      "Failed to load string in multiply");
            goto fail;
        }
        else if (is_isnull) {
            if (has_nan_na) {
                if (NpyString_pack_null(oallocator, ops) < 0) {
                    npy_gil_error(PyExc_MemoryError,
                              "Failed to deallocate string in multiply");
                    goto fail;
                }

                sin += s_stride;
                iin += i_stride;
                out += o_stride;
                continue;
            }
            else if (has_string_na || !has_null) {
                is = *(npy_static_string *)default_string;
            }
            else {
                npy_gil_error(PyExc_TypeError,
                          "Cannot multiply null that is not a nan-like "
                          "value");
                goto fail;
            }
        }
        T factor = *(T *)iin;
        size_t cursize = is.size;
        size_t newsize;
        int overflowed = npy_mul_with_overflow_size_t(
                &newsize, cursize, factor);
        if (overflowed) {
            npy_gil_error(PyExc_MemoryError,
                      "Failed to allocate string in string multiply");
            goto fail;
        }

        char *buf = NULL;
        npy_static_string os = {0, NULL};
        // in-place
        if (descrs[0] == descrs[1]) {
            buf = (char *)PyMem_RawMalloc(newsize);
            if (buf == NULL) {
                npy_gil_error(PyExc_MemoryError,
                          "Failed to allocate string in multiply");
                goto fail;
            }
        }
        else {
            if (load_new_string(
                        ops, &os, newsize,
                        oallocator, "multiply") == -1) {
                goto fail;
            }
            /* explicitly discard const; initializing new buffer */
            buf = (char *)os.buf;
        }

        for (size_t i = 0; i < (size_t)factor; i++) {
            /* multiply can't overflow because cursize * factor */
            /* has already been checked and doesn't overflow */
            memcpy((char *)buf + i * cursize, is.buf, cursize);
        }

        // clean up temp buffer for in-place operations
        if (descrs[0] == descrs[1]) {
            if (NpyString_pack(oallocator, ops, buf, newsize) < 0) {
                npy_gil_error(PyExc_MemoryError,
                          "Failed to pack string in multiply");
                goto fail;
            }

            PyMem_RawFree(buf);
        }

        sin += s_stride;
        iin += i_stride;
        out += o_stride;
    }
    NpyString_release_allocators(2, allocators);
    return 0;

fail:
    NpyString_release_allocators(2, allocators);
    return -1;
}

template <typename T>
static int multiply_right_strided_loop(
        PyArrayMethod_Context *context, char *const data[],
        npy_intp const dimensions[], npy_intp const strides[],
        NpyAuxData *NPY_UNUSED(auxdata))
{
    PyArray_StringDTypeObject *idescr =
            (PyArray_StringDTypeObject *)context->descriptors[0];
    PyArray_StringDTypeObject *odescr =
            (PyArray_StringDTypeObject *)context->descriptors[2];
    npy_intp N = dimensions[0];
    char *sin = data[0];
    char *iin = data[1];
    char *out = data[2];
    npy_intp in1_stride = strides[0];
    npy_intp in2_stride = strides[1];
    npy_intp out_stride = strides[2];

    return multiply_loop_core<T>(
            N, sin, iin, out, in1_stride, in2_stride, out_stride,
            idescr, odescr);
}

template <typename T>
static int multiply_left_strided_loop(
        PyArrayMethod_Context *context, char *const data[],
        npy_intp const dimensions[], npy_intp const strides[],
        NpyAuxData *NPY_UNUSED(auxdata))
{
    PyArray_StringDTypeObject *idescr =
            (PyArray_StringDTypeObject *)context->descriptors[1];
    PyArray_StringDTypeObject *odescr =
            (PyArray_StringDTypeObject *)context->descriptors[2];
    npy_intp N = dimensions[0];
    char *iin = data[0];
    char *sin = data[1];
    char *out = data[2];
    npy_intp in1_stride = strides[0];
    npy_intp in2_stride = strides[1];
    npy_intp out_stride = strides[2];

    return multiply_loop_core<T>(
            N, sin, iin, out, in2_stride, in1_stride, out_stride,
            idescr, odescr);
}

static NPY_CASTING
binary_resolve_descriptors(struct PyArrayMethodObject_tag *NPY_UNUSED(method),
                           PyArray_DTypeMeta *NPY_UNUSED(dtypes[]),
                           PyArray_Descr *given_descrs[],
                           PyArray_Descr *loop_descrs[],
                           npy_intp *NPY_UNUSED(view_offset))
{
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)given_descrs[0];
    PyArray_StringDTypeObject *descr2 = (PyArray_StringDTypeObject *)given_descrs[1];

    // _eq_comparison has a short-circuit pointer comparison fast path,
    // so no need to check here
    int eq_res = _eq_comparison(descr1->coerce, descr2->coerce,
                                descr1->na_object, descr2->na_object);

    if (eq_res < 0) {
        return (NPY_CASTING)-1;
    }

    if (eq_res != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "Can only do binary operations with equal StringDType "
                        "instances.");
        return (NPY_CASTING)-1;
    }

    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    Py_INCREF(given_descrs[1]);
    loop_descrs[1] = given_descrs[1];

    PyArray_Descr *out_descr = NULL;

    if (given_descrs[2] == NULL) {
        out_descr = (PyArray_Descr *)new_stringdtype_instance(
                ((PyArray_StringDTypeObject *)given_descrs[1])->na_object,
                ((PyArray_StringDTypeObject *)given_descrs[1])->coerce,
                NULL);

        if (out_descr == NULL) {
            return (NPY_CASTING)-1;
        }
    }
    else {
        Py_INCREF(given_descrs[2]);
        out_descr = given_descrs[2];
    }

    loop_descrs[2] = out_descr;

    return NPY_NO_CASTING;
}

static int
add_strided_loop(PyArrayMethod_Context *context, char *const data[],
                 npy_intp const dimensions[], npy_intp const strides[],
                 NpyAuxData *NPY_UNUSED(auxdata))
{
    PyArray_StringDTypeObject *s1descr = (PyArray_StringDTypeObject *)context->descriptors[0];
    PyArray_StringDTypeObject *s2descr = (PyArray_StringDTypeObject *)context->descriptors[1];
    PyArray_StringDTypeObject *odescr = (PyArray_StringDTypeObject *)context->descriptors[2];
    int has_null = s1descr->na_object != NULL;
    int has_nan_na = s1descr->has_nan_na;
    int has_string_na = s1descr->has_string_na;
    const npy_static_string *default_string = &s1descr->default_string;
    npy_intp N = dimensions[0];
    char *in1 = data[0];
    char *in2 = data[1];
    char *out = data[2];
    npy_intp in1_stride = strides[0];
    npy_intp in2_stride = strides[1];
    npy_intp out_stride = strides[2];

    npy_string_allocator *allocators[3] = {};
    NpyString_acquire_allocators(3, context->descriptors, allocators);
    npy_string_allocator *s1allocator = allocators[0];
    npy_string_allocator *s2allocator = allocators[1];
    npy_string_allocator *oallocator = allocators[2];

    while (N--) {
        LOAD_TWO_INPUT_STRINGS("add")
        char *buf = NULL;
        npy_static_string os = {0, NULL};
        size_t newsize = 0;
        npy_packed_static_string *ops = (npy_packed_static_string *)out;
        if (NPY_UNLIKELY(s1_isnull || s2_isnull)) {
            if (has_nan_na) {
                if (NpyString_pack_null(oallocator, ops) < 0) {
                    npy_gil_error(PyExc_MemoryError,
                                  "Failed to deallocate string in add");
                    goto fail;
                }
                goto next_step;
            }
            else if (has_string_na || !has_null) {
                if (s1_isnull) {
                    s1 = *default_string;
                }
                if (s2_isnull) {
                    s2 = *default_string;
                }
            }
            else {
                npy_gil_error(PyExc_ValueError,
                              "Cannot add null that is not a nan-like value");
                goto fail;
            }
        }

        // check for overflow
        newsize = s1.size + s2.size;
        if (newsize < s1.size) {
            npy_gil_error(PyExc_MemoryError, "Failed to allocate string in add");
            goto fail;
        }

        // in-place
        if (s1descr == odescr || s2descr == odescr) {
            buf = (char *)PyMem_RawMalloc(newsize);

            if (buf == NULL) {
                npy_gil_error(PyExc_MemoryError,
                          "Failed to allocate string in add");
                goto fail;
            }
        }
        else {
            if (load_new_string(ops, &os, newsize, oallocator, "add") == -1) {
                goto fail;
            }
            // excplicitly discard const; initializing new buffer
            buf = (char *)os.buf;
        }

        memcpy(buf, s1.buf, s1.size);
        memcpy(buf + s1.size, s2.buf, s2.size);

        // clean up temporary in-place buffer
        if (s1descr == odescr || s2descr == odescr) {
            if (NpyString_pack(oallocator, ops, buf, newsize) < 0) {
                npy_gil_error(PyExc_MemoryError,
                          "Failed to pack output string in add");
                goto fail;
            }

            PyMem_RawFree(buf);
        }

    next_step:
        in1 += in1_stride;
        in2 += in2_stride;
        out += out_stride;
    }
    NpyString_release_allocators(3, allocators);
    return 0;

fail:
    NpyString_release_allocators(3, allocators);
    return -1;
}


static int
minimum_maximum_strided_loop(PyArrayMethod_Context *context, char *const data[],
                     npy_intp const dimensions[], npy_intp const strides[],
                     NpyAuxData *NPY_UNUSED(auxdata))
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    npy_bool invert = *(npy_bool *)context->method->static_data; // true for maximum
    PyArray_StringDTypeObject *in1_descr =
            ((PyArray_StringDTypeObject *)context->descriptors[0]);
    PyArray_StringDTypeObject *in2_descr =
            ((PyArray_StringDTypeObject *)context->descriptors[1]);
    npy_intp N = dimensions[0];
    char *in1 = data[0];
    char *in2 = data[1];
    char *out = data[2];
    npy_intp in1_stride = strides[0];
    npy_intp in2_stride = strides[1];
    npy_intp out_stride = strides[2];

    npy_string_allocator *allocators[3] = {};
    NpyString_acquire_allocators(3, context->descriptors, allocators);
    npy_string_allocator *in1_allocator = allocators[0];
    npy_string_allocator *in2_allocator = allocators[1];
    npy_string_allocator *out_allocator = allocators[2];

    while (N--) {
        const npy_packed_static_string *sin1 = (npy_packed_static_string *)in1;
        const npy_packed_static_string *sin2 = (npy_packed_static_string *)in2;
        npy_packed_static_string *sout = (npy_packed_static_string *)out;
        int cmp = _compare(in1, in2, in1_descr, in2_descr);
        if (cmp == 0 && (in1 == out || in2 == out)) {
            continue;
        }
        if ((cmp < 0) ^ invert) {
            // if in and out are the same address, do nothing to avoid a
            // use-after-free
            if (in1 != out) {
                if (free_and_copy(in1_allocator, out_allocator, sin1, sout,
                                  ufunc_name) == -1) {
                    goto fail;
                }
            }
        }
        else {
            if (in2 != out) {
                if (free_and_copy(in2_allocator, out_allocator, sin2, sout,
                                  ufunc_name) == -1) {
                    goto fail;
                }
            }
        }
        in1 += in1_stride;
        in2 += in2_stride;
        out += out_stride;
    }

    NpyString_release_allocators(3, allocators);
    return 0;

fail:
    NpyString_release_allocators(3, allocators);
    return -1;
}

static int
string_comparison_strided_loop(PyArrayMethod_Context *context, char *const data[],
                            npy_intp const dimensions[],
                            npy_intp const strides[],
                            NpyAuxData *NPY_UNUSED(auxdata))
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    npy_bool res_for_eq = ((npy_bool *)context->method->static_data)[0];
    npy_bool res_for_lt = ((npy_bool *)context->method->static_data)[1];
    npy_bool res_for_gt = ((npy_bool *)context->method->static_data)[2];
    npy_bool res_for_ne = !res_for_eq;
    npy_bool eq_or_ne = res_for_lt == res_for_gt;
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)context->descriptors[0];
    int has_null = descr1->na_object != NULL;
    int has_nan_na = descr1->has_nan_na;
    int has_string_na = descr1->has_string_na;
    const npy_static_string *default_string = &descr1->default_string;
    npy_intp N = dimensions[0];
    char *in1 = data[0];
    char *in2 = data[1];
    npy_bool *out = (npy_bool *)data[2];
    npy_intp in1_stride = strides[0];
    npy_intp in2_stride = strides[1];
    npy_intp out_stride = strides[2];

    npy_string_allocator *allocators[2] = {};
    NpyString_acquire_allocators(2, context->descriptors, allocators);
    npy_string_allocator *s1allocator = allocators[0];
    npy_string_allocator *s2allocator = allocators[1];

    while (N--) {
        int cmp;
        LOAD_TWO_INPUT_STRINGS(ufunc_name);
        if (NPY_UNLIKELY(s1_isnull || s2_isnull)) {
            if (has_nan_na) {
                // s1 or s2 is NA
                *out = NPY_FALSE;
                goto next_step;
            }
            else if (has_null && !has_string_na) {
                if (eq_or_ne) {
                    if (s1_isnull && s2_isnull) {
                        *out = res_for_eq;
                    }
                    else {
                        *out = res_for_ne;
                    }
                }
                else {
                    npy_gil_error(PyExc_ValueError,
                                  "'%s' not supported for null values that are not "
                                  "nan-like or strings.", ufunc_name);
                    goto fail;
                }
            }
            else {
                if (s1_isnull) {
                    s1 = *default_string;
                }
                if (s2_isnull) {
                    s2 = *default_string;
                }
            }
        }
        cmp = NpyString_cmp(&s1, &s2);
        if (cmp == 0) {
            *out = res_for_eq;
        }
        else if (cmp < 0) {
            *out = res_for_lt;
        }
        else {
            *out = res_for_gt;
        }

    next_step:
        in1 += in1_stride;
        in2 += in2_stride;
        out += out_stride;
    }

    NpyString_release_allocators(2, allocators);

    return 0;

fail:
    NpyString_release_allocators(2, allocators);

    return -1;
}

static NPY_CASTING
string_comparison_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]), PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[], npy_intp *NPY_UNUSED(view_offset))
{
    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    Py_INCREF(given_descrs[1]);
    loop_descrs[1] = given_descrs[1];
    loop_descrs[2] = PyArray_DescrFromType(NPY_BOOL);  // cannot fail

    return NPY_NO_CASTING;
}

static int
string_isnan_strided_loop(PyArrayMethod_Context *context, char *const data[],
                          npy_intp const dimensions[],
                          npy_intp const strides[],
                          NpyAuxData *NPY_UNUSED(auxdata))
{
    PyArray_StringDTypeObject *descr = (PyArray_StringDTypeObject *)context->descriptors[0];
    int has_nan_na = descr->has_nan_na;

    npy_intp N = dimensions[0];
    char *in = data[0];
    npy_bool *out = (npy_bool *)data[1];
    npy_intp in_stride = strides[0];
    npy_intp out_stride = strides[1];

    while (N--) {
        const npy_packed_static_string *s = (npy_packed_static_string *)in;
        if (has_nan_na && NpyString_isnull(s)) {
            *out = NPY_TRUE;
        }
        else {
            *out = NPY_FALSE;
        }

        in += in_stride;
        out += out_stride;
    }

    return 0;
}

static NPY_CASTING
string_bool_output_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]), PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[], npy_intp *NPY_UNUSED(view_offset))
{
    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    loop_descrs[1] = PyArray_DescrFromType(NPY_BOOL);  // cannot fail

    return NPY_NO_CASTING;
}

static NPY_CASTING
string_intp_output_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]), PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[], npy_intp *NPY_UNUSED(view_offset))
{
    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    loop_descrs[1] = PyArray_DescrFromType(NPY_INTP);  // cannot fail

    return NPY_NO_CASTING;
}

typedef bool (Buffer<ENCODING::UTF8>::*utf8_buffer_method)();

static int
string_bool_output_unary_strided_loop(
        PyArrayMethod_Context *context, char *const data[],
        npy_intp const dimensions[],
        npy_intp const strides[],
        NpyAuxData *NPY_UNUSED(auxdata))
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    utf8_buffer_method is_it = *(utf8_buffer_method *)(context->method->static_data);
    PyArray_StringDTypeObject *descr = (PyArray_StringDTypeObject *)context->descriptors[0];
    npy_string_allocator *allocator = NpyString_acquire_allocator(descr);
    int has_string_na = descr->has_string_na;
    int has_nan_na = descr->has_nan_na;
    const npy_static_string *default_string = &descr->default_string;
    npy_intp N = dimensions[0];
    char *in = data[0];
    char *out = data[1];
    npy_intp in_stride = strides[0];
    npy_intp out_stride = strides[1];

    while (N--) {
        const npy_packed_static_string *ps = (npy_packed_static_string *)in;

        npy_static_string s = {0, NULL};
        const char *buffer = NULL;
        size_t size = 0;
        Buffer<ENCODING::UTF8> buf;

        int is_null = NpyString_load(allocator, ps, &s);

        if (is_null == -1) {
            npy_gil_error(PyExc_MemoryError, "Failed to load string in %s", ufunc_name);
            goto fail;
        }

        if (is_null) {
            if (has_nan_na) {
                // NA values are always falsy
                *out = NPY_FALSE;
                goto next_step;
            }
            else if (!has_string_na) {
                npy_gil_error(PyExc_ValueError,
                              "Cannot use the %s function with a null that is "
                              "not a nan-like value", ufunc_name);
                goto fail;
            }
            buffer = default_string->buf;
            size = default_string->size;
        }
        else {
            buffer = s.buf;
            size = s.size;
        }
        buf = Buffer<ENCODING::UTF8>((char *)buffer, size);
        *(npy_bool *)out = (buf.*is_it)();

      next_step:
        in += in_stride;
        out += out_stride;
    }

    NpyString_release_allocator(allocator);

    return 0;
fail:
    NpyString_release_allocator(allocator);

    return -1;
}

static int
string_strlen_strided_loop(PyArrayMethod_Context *context, char *const data[],
                           npy_intp const dimensions[],
                           npy_intp const strides[],
                           NpyAuxData *auxdata)
{
    PyArray_StringDTypeObject *descr = (PyArray_StringDTypeObject *)context->descriptors[0];
    npy_string_allocator *allocator = NpyString_acquire_allocator(descr);
    int has_string_na = descr->has_string_na;
    const npy_static_string *default_string = &descr->default_string;

    npy_intp N = dimensions[0];
    char *in = data[0];
    char *out = data[1];
    npy_intp in_stride = strides[0];
    npy_intp out_stride = strides[1];

    while (N--) {
        const npy_packed_static_string *ps = (npy_packed_static_string *)in;

        npy_static_string s = {0, NULL};
        const char *buffer = NULL;
        size_t size = 0;
        Buffer<ENCODING::UTF8> buf;
        int is_null = NpyString_load(allocator, ps, &s);

        if (is_null == -1) {
            npy_gil_error(PyExc_MemoryError, "Failed to load string in str_len");
            goto fail;
        }

        if (is_null) {
            if (!has_string_na) {
                npy_gil_error(PyExc_ValueError,
                              "The length of a null string is undefined");
                goto next_step;
            }
            buffer = default_string->buf;
            size = default_string->size;
        }
        else {
            buffer = s.buf;
            size = s.size;
        }
        buf = Buffer<ENCODING::UTF8>((char *)buffer, size);
        *(npy_intp *)out = buf.num_codepoints();

      next_step:
        in += in_stride;
        out += out_stride;
    }

    NpyString_release_allocator(allocator);

    return 0;
fail:
    NpyString_release_allocator(allocator);

    return -1;
}

static int
string_find_rfind_count_promoter(PyObject *NPY_UNUSED(ufunc),
        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    new_op_dtypes[0] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[1] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[2] = NPY_DT_NewRef(&PyArray_Int64DType);
    new_op_dtypes[3] = NPY_DT_NewRef(&PyArray_Int64DType);
    new_op_dtypes[4] = PyArray_DTypeFromTypeNum(NPY_DEFAULT_INT);
    return 0;
}

static NPY_CASTING
string_find_rfind_count_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]),
        PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[],
        npy_intp *NPY_UNUSED(view_offset))
{
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)given_descrs[0];
    PyArray_StringDTypeObject *descr2 = (PyArray_StringDTypeObject *)given_descrs[1];

    // _eq_comparison has a short-circuit pointer comparison fast path,
    // so no need to check here
    int eq_res = _eq_comparison(descr1->coerce, descr2->coerce,
                                descr1->na_object, descr2->na_object);

    if (eq_res < 0) {
        return (NPY_CASTING)-1;
    }

    if (eq_res != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "Can only do binary operations with equal StringDType "
                        "instances.");
        return (NPY_CASTING)-1;
    }

    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    Py_INCREF(given_descrs[1]);
    loop_descrs[1] = given_descrs[1];
    Py_INCREF(given_descrs[2]);
    loop_descrs[2] = given_descrs[2];
    Py_INCREF(given_descrs[3]);
    loop_descrs[3] = given_descrs[3];
    if (given_descrs[4] == NULL) {
        loop_descrs[4] = PyArray_DescrFromType(NPY_DEFAULT_INT);
    }
    else {
        Py_INCREF(given_descrs[4]);
        loop_descrs[4] = given_descrs[4];
    }

    return NPY_NO_CASTING;
}

static int
string_startswith_endswith_promoter(
        PyObject *NPY_UNUSED(ufunc),
        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    new_op_dtypes[0] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[1] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[2] = NPY_DT_NewRef(&PyArray_Int64DType);
    new_op_dtypes[3] = NPY_DT_NewRef(&PyArray_Int64DType);
    new_op_dtypes[4] = PyArray_DTypeFromTypeNum(NPY_BOOL);
    return 0;
}

static NPY_CASTING
string_startswith_endswith_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]),
        PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[],
        npy_intp *NPY_UNUSED(view_offset))
{
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)given_descrs[0];
    PyArray_StringDTypeObject *descr2 = (PyArray_StringDTypeObject *)given_descrs[1];

    // _eq_comparison has a short-circuit pointer comparison fast path, so
    // no need to do it here
    int eq_res = _eq_comparison(descr1->coerce, descr2->coerce,
                                descr1->na_object, descr2->na_object);

    if (eq_res < 0) {
        return (NPY_CASTING)-1;
    }

    if (eq_res != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "Can only do binary operations with equal StringDType "
                        "instances.");
        return (NPY_CASTING)-1;
    }

    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    Py_INCREF(given_descrs[1]);
    loop_descrs[1] = given_descrs[1];
    Py_INCREF(given_descrs[2]);
    loop_descrs[2] = given_descrs[2];
    Py_INCREF(given_descrs[3]);
    loop_descrs[3] = given_descrs[3];
    if (given_descrs[4] == NULL) {
        loop_descrs[4] = PyArray_DescrFromType(NPY_BOOL);
    }
    else {
        Py_INCREF(given_descrs[4]);
        loop_descrs[4] = given_descrs[4];
    }

    return NPY_NO_CASTING;
}

typedef npy_intp find_like_function(Buffer<ENCODING::UTF8>, Buffer<ENCODING::UTF8>,
                                    npy_int64, npy_int64);

static int
string_find_rfind_count_strided_loop(PyArrayMethod_Context *context,
                         char *const data[],
                         npy_intp const dimensions[],
                         npy_intp const strides[],
                         NpyAuxData *auxdata)
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    find_like_function *function = *(find_like_function *)(context->method->static_data);
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)context->descriptors[0];

    int has_null = descr1->na_object != NULL;
    int has_string_na = descr1->has_string_na;
    const npy_static_string *default_string = &descr1->default_string;

    npy_string_allocator *allocators[2] = {};
    NpyString_acquire_allocators(2, context->descriptors, allocators);
    npy_string_allocator *s1allocator = allocators[0];
    npy_string_allocator *s2allocator = allocators[1];

    char *in1 = data[0];
    char *in2 = data[1];
    char *in3 = data[2];
    char *in4 = data[3];
    char *out = data[4];

    npy_intp N = dimensions[0];

    while (N--) {
        LOAD_TWO_INPUT_STRINGS(ufunc_name);
        if (NPY_UNLIKELY(s1_isnull || s2_isnull)) {
            if (has_null && !has_string_na) {
                npy_gil_error(PyExc_ValueError,
                              "'%s' not supported for null values that are not "
                              "strings.", ufunc_name);
                goto fail;
            }
            else {
                if (s1_isnull) {
                    s1 = *default_string;
                }
                if (s2_isnull) {
                    s2 = *default_string;
                }
            }
        }

        npy_int64 start = *(npy_int64 *)in3;
        npy_int64 end = *(npy_int64 *)in4;

        Buffer<ENCODING::UTF8> buf1((char *)s1.buf, s1.size);
        Buffer<ENCODING::UTF8> buf2((char *)s2.buf, s2.size);

        npy_intp pos = function(buf1, buf2, start, end);
        *(npy_intp *)out = pos;

        in1 += strides[0];
        in2 += strides[1];
        in3 += strides[2];
        in4 += strides[3];
        out += strides[4];
    }

    NpyString_release_allocators(2, allocators);

    return 0;

fail:
    NpyString_release_allocators(2, allocators);

    return -1;
}

static int
string_startswith_endswith_strided_loop(PyArrayMethod_Context *context,
                               char *const data[],
                               npy_intp const dimensions[],
                               npy_intp const strides[],
                               NpyAuxData *auxdata)
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    STARTPOSITION startposition = *(STARTPOSITION *)context->method->static_data;
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)context->descriptors[0];

    int has_null = descr1->na_object != NULL;
    int has_string_na = descr1->has_string_na;
    int has_nan_na = descr1->has_nan_na;
    const npy_static_string *default_string = &descr1->default_string;

    npy_string_allocator *allocators[2] = {};
    NpyString_acquire_allocators(2, context->descriptors, allocators);
    npy_string_allocator *s1allocator = allocators[0];
    npy_string_allocator *s2allocator = allocators[1];

    char *in1 = data[0];
    char *in2 = data[1];
    char *in3 = data[2];
    char *in4 = data[3];
    char *out = data[4];

    npy_intp N = dimensions[0];

    while (N--) {
        LOAD_TWO_INPUT_STRINGS(ufunc_name);
        if (NPY_UNLIKELY(s1_isnull || s2_isnull)) {
            if (has_null && !has_string_na) {
                if (has_nan_na) {
                    // nulls are always falsey for this operation.
                    *(npy_bool *)out = 0;
                    goto next_step;
                }
                else {
                    npy_gil_error(PyExc_ValueError,
                                  "'%s' not supported for null values that "
                                  "are not nan-like or strings.", ufunc_name);
                    goto fail;
                }
            }
            else {
                if (s1_isnull) {
                    s1 = *default_string;
                }
                if (s2_isnull) {
                    s2 = *default_string;
                }
            }
        }
        {
            npy_int64 start = *(npy_int64 *)in3;
            npy_int64 end = *(npy_int64 *)in4;

            Buffer<ENCODING::UTF8> buf1((char *)s1.buf, s1.size);
            Buffer<ENCODING::UTF8> buf2((char *)s2.buf, s2.size);

            npy_bool match = tailmatch<ENCODING::UTF8>(buf1, buf2, start, end,
                                                       startposition);
            *(npy_bool *)out = match;
        }

      next_step:

        in1 += strides[0];
        in2 += strides[1];
        in3 += strides[2];
        in4 += strides[3];
        out += strides[4];
    }

    NpyString_release_allocators(2, allocators);

    return 0;

fail:
    NpyString_release_allocators(2, allocators);

    return -1;
}

static int
strip_chars_promoter(PyObject *NPY_UNUSED(ufunc),
        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    new_op_dtypes[0] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[1] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[2] = NPY_DT_NewRef(&PyArray_StringDType);
    return 0;
}

static NPY_CASTING
strip_chars_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]),
        PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[],
        npy_intp *NPY_UNUSED(view_offset))
{
    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];

    // we don't actually care about the null behavior of the second argument,
    // so no need to check if the first two descrs are equal like in
    // binary_resolve_descriptors

    Py_INCREF(given_descrs[1]);
    loop_descrs[1] = given_descrs[1];

    PyArray_Descr *out_descr = NULL;

    if (given_descrs[2] == NULL) {
        out_descr = (PyArray_Descr *)new_stringdtype_instance(
                ((PyArray_StringDTypeObject *)given_descrs[0])->na_object,
                ((PyArray_StringDTypeObject *)given_descrs[0])->coerce,
                NULL);

        if (out_descr == NULL) {
            return (NPY_CASTING)-1;
        }
    }
    else {
        Py_INCREF(given_descrs[2]);
        out_descr = given_descrs[2];
    }

    loop_descrs[2] = out_descr;

    return NPY_NO_CASTING;
}


NPY_NO_EXPORT int
string_lrstrip_chars_strided_loop(
        PyArrayMethod_Context *context, char *const data[],
        npy_intp const dimensions[],
        npy_intp const strides[],
        NpyAuxData *auxdata)
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    STRIPTYPE striptype = *(STRIPTYPE *)context->method->static_data;
    PyArray_StringDTypeObject *s1descr = (PyArray_StringDTypeObject *)context->descriptors[0];
    int has_null = s1descr->na_object != NULL;
    int has_string_na = s1descr->has_string_na;

    const npy_static_string *default_string = &s1descr->default_string;
    npy_intp N = dimensions[0];
    char *in1 = data[0];
    char *in2 = data[1];
    char *out = data[2];

    npy_string_allocator *allocators[3] = {};
    NpyString_acquire_allocators(3, context->descriptors, allocators);
    npy_string_allocator *s1allocator = allocators[0];
    npy_string_allocator *s2allocator = allocators[1];
    npy_string_allocator *oallocator = allocators[2];

    while (N--) {
        LOAD_TWO_INPUT_STRINGS(ufunc_name);
        npy_packed_static_string *ops = (npy_packed_static_string *)out;

        if (NPY_UNLIKELY(s1_isnull || s2_isnull)) {
            if (has_string_na || !has_null) {
                if (s1_isnull) {
                    s1 = *default_string;
                }
                if (s2_isnull) {
                    s2 = *default_string;
                }
            }
            else {
                npy_gil_error(PyExc_ValueError,
                              "Cannot strip null values that are not strings");
                goto fail;
            }
        }


        char *new_buf = (char *)PyMem_RawCalloc(s1.size, 1);
        Buffer<ENCODING::UTF8> buf1((char *)s1.buf, s1.size);
        Buffer<ENCODING::UTF8> buf2((char *)s2.buf, s2.size);
        Buffer<ENCODING::UTF8> outbuf(new_buf, s1.size);
        size_t new_buf_size = string_lrstrip_chars
                (buf1, buf2, outbuf, striptype);

        if (NpyString_pack(oallocator, ops, new_buf, new_buf_size) < 0) {
            npy_gil_error(PyExc_MemoryError, "Failed to pack string in %s",
                          ufunc_name);
            goto fail;
        }

        PyMem_RawFree(new_buf);

        in1 += strides[0];
        in2 += strides[1];
        out += strides[2];
    }

    NpyString_release_allocators(3, allocators);
    return 0;

fail:
    NpyString_release_allocators(3, allocators);
    return -1;

}

static NPY_CASTING
strip_whitespace_resolve_descriptors(
        struct PyArrayMethodObject_tag *NPY_UNUSED(method),
        PyArray_DTypeMeta *NPY_UNUSED(dtypes[]),
        PyArray_Descr *given_descrs[],
        PyArray_Descr *loop_descrs[],
        npy_intp *NPY_UNUSED(view_offset))
{
    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];

    PyArray_Descr *out_descr = NULL;

    if (given_descrs[1] == NULL) {
        out_descr = (PyArray_Descr *)new_stringdtype_instance(
                ((PyArray_StringDTypeObject *)given_descrs[0])->na_object,
                ((PyArray_StringDTypeObject *)given_descrs[0])->coerce,
                NULL);

        if (out_descr == NULL) {
            return (NPY_CASTING)-1;
        }
    }
    else {
        Py_INCREF(given_descrs[1]);
        out_descr = given_descrs[1];
    }

    loop_descrs[1] = out_descr;

    return NPY_NO_CASTING;
}

static int
string_lrstrip_whitespace_strided_loop(
        PyArrayMethod_Context *context,
        char *const data[], npy_intp const dimensions[],
        npy_intp const strides[], NpyAuxData *NPY_UNUSED(auxdata))
{
    const char *ufunc_name = ((PyUFuncObject *)context->caller)->name;
    STRIPTYPE striptype = *(STRIPTYPE *)context->method->static_data;
    PyArray_StringDTypeObject *descr = (PyArray_StringDTypeObject *)context->descriptors[0];
    int has_string_na = descr->has_string_na;
    int has_null = descr->na_object != NULL;
    const npy_static_string *default_string = &descr->default_string;

    npy_string_allocator *allocators[2] = {};
    NpyString_acquire_allocators(2, context->descriptors, allocators);
    npy_string_allocator *allocator = allocators[0];
    npy_string_allocator *oallocator = allocators[1];

    char *in = data[0];
    char *out = data[1];

    npy_intp N = dimensions[0];

    while (N--) {
        const npy_packed_static_string *ps = (npy_packed_static_string *)in;
        npy_static_string s = {0, NULL};
        int s_isnull = NpyString_load(allocator, ps, &s);

        if (s_isnull == -1) {
            npy_gil_error(PyExc_MemoryError, "Failed to load string in %s",
                          ufunc_name);
            goto fail;
        }

        npy_packed_static_string *ops = (npy_packed_static_string *)out;

        if (NPY_UNLIKELY(s_isnull)) {
            if (has_string_na || !has_null) {
                s = *default_string;
            }
            else {
                npy_gil_error(PyExc_ValueError,
                              "Cannot strip null values that are not strings");
                goto fail;
            }
        }

        char *new_buf = (char *)PyMem_RawCalloc(s.size, 1);
        Buffer<ENCODING::UTF8> buf((char *)s.buf, s.size);
        Buffer<ENCODING::UTF8> outbuf(new_buf, s.size);
        size_t new_buf_size = string_lrstrip_whitespace(
                buf, outbuf, striptype);

        if (NpyString_pack(oallocator, ops, new_buf, new_buf_size) < 0) {
            npy_gil_error(PyExc_MemoryError, "Failed to pack string in %s",
                          ufunc_name);
            goto fail;
        }

        PyMem_RawFree(new_buf);

        in += strides[0];
        out += strides[1];
    }

    NpyString_release_allocators(2, allocators);

    return 0;

  fail:
    NpyString_release_allocators(2, allocators);

    return -1;

}

static int
string_replace_promoter(PyObject *NPY_UNUSED(ufunc),
                        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
                        PyArray_DTypeMeta *new_op_dtypes[])
{
    new_op_dtypes[0] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[1] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[2] = NPY_DT_NewRef(&PyArray_StringDType);
    new_op_dtypes[3] = NPY_DT_NewRef(&PyArray_Int64DType);
    new_op_dtypes[4] = NPY_DT_NewRef(&PyArray_StringDType);
    return 0;
}

static NPY_CASTING
replace_resolve_descriptors(struct PyArrayMethodObject_tag *NPY_UNUSED(method),
                            PyArray_DTypeMeta *NPY_UNUSED(dtypes[]),
                            PyArray_Descr *given_descrs[],
                            PyArray_Descr *loop_descrs[],
                            npy_intp *NPY_UNUSED(view_offset))
{
    PyArray_StringDTypeObject *descr1 = (PyArray_StringDTypeObject *)given_descrs[0];
    PyArray_StringDTypeObject *descr2 = (PyArray_StringDTypeObject *)given_descrs[1];
    PyArray_StringDTypeObject *descr3 = (PyArray_StringDTypeObject *)given_descrs[2];

    // _eq_comparison has a short-circuit pointer comparison fast path, so
    // no need to do it here
    int eq_res = (_eq_comparison(descr1->coerce, descr2->coerce,
                                 descr1->na_object, descr2->na_object) &&
                  _eq_comparison(descr1->coerce, descr3->coerce,
                                 descr1->na_object, descr3->na_object));

    if (eq_res < 0) {
        return (NPY_CASTING)-1;
    }

    if (eq_res != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "String replace is only supported with equal StringDType "
                        "instances.");
        return (NPY_CASTING)-1;
    }

    Py_INCREF(given_descrs[0]);
    loop_descrs[0] = given_descrs[0];
    Py_INCREF(given_descrs[1]);
    loop_descrs[1] = given_descrs[1];
    Py_INCREF(given_descrs[2]);
    loop_descrs[2] = given_descrs[2];
    Py_INCREF(given_descrs[3]);
    loop_descrs[3] = given_descrs[3];

    PyArray_Descr *out_descr = NULL;

    if (given_descrs[4] == NULL) {
        out_descr = (PyArray_Descr *)new_stringdtype_instance(
                ((PyArray_StringDTypeObject *)given_descrs[0])->na_object,
                ((PyArray_StringDTypeObject *)given_descrs[0])->coerce,
                NULL);

        if (out_descr == NULL) {
            return (NPY_CASTING)-1;
        }
    }
    else {
        Py_INCREF(given_descrs[4]);
        out_descr = given_descrs[4];
    }

    loop_descrs[4] = out_descr;

    return NPY_NO_CASTING;
}

static int
string_replace_strided_loop(
        PyArrayMethod_Context *context,
        char *const data[], npy_intp const dimensions[],
        npy_intp const strides[], NpyAuxData *NPY_UNUSED(auxdata))
{
    char *in1 = data[0];
    char *in2 = data[1];
    char *in3 = data[2];
    char *in4 = data[3];
    char *out = data[4];

    npy_intp N = dimensions[0];

    PyArray_StringDTypeObject *descr0 =
            (PyArray_StringDTypeObject *)context->descriptors[0];
    int has_string_na = descr0->has_string_na;
    const npy_static_string *default_string = &descr0->default_string;


    npy_string_allocator *allocators[5] = {};
    NpyString_acquire_allocators(5, context->descriptors, allocators);
    npy_string_allocator *i1allocator = allocators[0];
    npy_string_allocator *i2allocator = allocators[1];
    npy_string_allocator *i3allocator = allocators[2];
    // allocators[3] is NULL
    npy_string_allocator *oallocator = allocators[4];



    while (N--) {
        const npy_packed_static_string *i1ps = (npy_packed_static_string *)in1;
        npy_static_string i1s = {0, NULL};
        const npy_packed_static_string *i2ps = (npy_packed_static_string *)in2;
        npy_static_string i2s = {0, NULL};
        const npy_packed_static_string *i3ps = (npy_packed_static_string *)in3;
        npy_static_string i3s = {0, NULL};
        npy_packed_static_string *ops = (npy_packed_static_string *)out;

        int i1_isnull = NpyString_load(i1allocator, i1ps, &i1s);
        int i2_isnull = NpyString_load(i2allocator, i2ps, &i2s);
        int i3_isnull = NpyString_load(i3allocator, i3ps, &i3s);

        if (i1_isnull == -1 || i2_isnull == -1 || i3_isnull == -1) {
            npy_gil_error(PyExc_MemoryError, "Failed to load string in replace");
            goto fail;
        }
        else if (i1_isnull || i2_isnull || i3_isnull) {
            if (!has_string_na) {
                npy_gil_error(PyExc_ValueError,
                              "Null values are not supported as replacement arguments "
                              "for replace");
                goto fail;
            }
            else {
                if (i1_isnull) {
                    i1s = *default_string;
                }
                if (i2_isnull) {
                    i2s = *default_string;
                }
                if (i3_isnull) {
                    i3s = *default_string;
                }
            }
        }

        // conservatively overallocate
        // TODO check overflow
        size_t max_size;
        if (i2s.size == 0) {
            // interleaving
            max_size = i1s.size + (i1s.size + 1)*(i3s.size);
        }
        else {
            // replace i2 with i3
            max_size = i1s.size * (i3s.size/i2s.size + 1);
        }
        char *new_buf = (char *)PyMem_RawCalloc(max_size, 1);
        Buffer<ENCODING::UTF8> buf1((char *)i1s.buf, i1s.size);
        Buffer<ENCODING::UTF8> buf2((char *)i2s.buf, i2s.size);
        Buffer<ENCODING::UTF8> buf3((char *)i3s.buf, i3s.size);
        Buffer<ENCODING::UTF8> outbuf(new_buf, max_size);

        size_t new_buf_size = string_replace(
                buf1, buf2, buf3, *(npy_int64 *)in4, outbuf);

        if (NpyString_pack(oallocator, ops, new_buf, new_buf_size) < 0) {
            npy_gil_error(PyExc_MemoryError, "Failed to pack string in replace");
            goto fail;
        }

        PyMem_RawFree(new_buf);

        in1 += strides[0];
        in2 += strides[1];
        in3 += strides[2];
        in4 += strides[3];
        out += strides[4];
    }

    NpyString_release_allocators(5, allocators);
    return 0;

  fail:
    NpyString_release_allocators(5, allocators);
    return -1;
}


NPY_NO_EXPORT int
string_inputs_promoter(
        PyObject *ufunc_obj, PyArray_DTypeMeta *op_dtypes[],
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[],
        PyArray_DTypeMeta *final_dtype,
        PyArray_DTypeMeta *result_dtype)
{
    PyUFuncObject *ufunc = (PyUFuncObject *)ufunc_obj;
    /* set all input operands to final_dtype */
    for (int i = 0; i < ufunc->nin; i++) {
        PyArray_DTypeMeta *tmp = final_dtype;
        if (signature[i]) {
            tmp = signature[i]; /* never replace a fixed one. */
        }
        Py_INCREF(tmp);
        new_op_dtypes[i] = tmp;
    }
    /* don't touch output dtypes if they are set */
    for (int i = ufunc->nin; i < ufunc->nargs; i++) {
        if (op_dtypes[i] != NULL) {
            Py_INCREF(op_dtypes[i]);
            new_op_dtypes[i] = op_dtypes[i];
        }
        else {
            Py_INCREF(result_dtype);
            new_op_dtypes[i] = result_dtype;
        }
    }

    return 0;
}

static int
string_object_bool_output_promoter(
        PyObject *ufunc, PyArray_DTypeMeta *op_dtypes[],
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    return string_inputs_promoter(
            ufunc, op_dtypes, signature,
            new_op_dtypes, &PyArray_ObjectDType, &PyArray_BoolDType);
}

static int
string_unicode_bool_output_promoter(
        PyObject *ufunc, PyArray_DTypeMeta *op_dtypes[],
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    return string_inputs_promoter(
            ufunc, op_dtypes, signature,
            new_op_dtypes, &PyArray_StringDType, &PyArray_BoolDType);
}

static int
is_integer_dtype(PyArray_DTypeMeta *DType)
{
    if (DType == &PyArray_PyIntAbstractDType) {
        return 1;
    }
    else if (DType == &PyArray_Int8DType) {
        return 1;
    }
    else if (DType == &PyArray_Int16DType) {
        return 1;
    }
    else if (DType == &PyArray_Int32DType) {
        return 1;
    }
    // int64 already has a loop registered for it,
    // so don't need to consider it
#if NPY_SIZEOF_BYTE == NPY_SIZEOF_SHORT
    else if (DType == &PyArray_ByteDType) {
        return 1;
    }
#endif
#if NPY_SIZEOF_SHORT == NPY_SIZEOF_INT
    else if (DType == &PyArray_ShortDType) {
        return 1;
    }
#endif
#if NPY_SIZEOF_INT == NPY_SIZEOF_LONG
    else if (DType == &PyArray_IntDType) {
        return 1;
    }
#endif
#if NPY_SIZEOF_LONGLONG == NPY_SIZEOF_LONG
    else if (DType == &PyArray_LongLongDType) {
        return 1;
    }
#endif
    else if (DType == &PyArray_UInt8DType) {
        return 1;
    }
    else if (DType == &PyArray_UInt16DType) {
        return 1;
    }
    else if (DType == &PyArray_UInt32DType) {
        return 1;
    }
    // uint64 already has a loop registered for it,
    // so don't need to consider it
#if NPY_SIZEOF_BYTE == NPY_SIZEOF_SHORT
    else if (DType == &PyArray_UByteDType) {
        return 1;
    }
#endif
#if NPY_SIZEOF_SHORT == NPY_SIZEOF_INT
    else if (DType == &PyArray_UShortDType) {
        return 1;
    }
#endif
#if NPY_SIZEOF_INT == NPY_SIZEOF_LONG
    else if (DType == &PyArray_UIntDType) {
        return 1;
    }
#endif
#if NPY_SIZEOF_LONGLONG == NPY_SIZEOF_LONG
    else if (DType == &PyArray_ULongLongDType) {
        return 1;
    }
#endif
    return 0;
}


static int
string_multiply_promoter(PyObject *ufunc_obj, PyArray_DTypeMeta *op_dtypes[],
                         PyArray_DTypeMeta *signature[],
                         PyArray_DTypeMeta *new_op_dtypes[])
{
    PyUFuncObject *ufunc = (PyUFuncObject *)ufunc_obj;
    for (int i = 0; i < ufunc->nin; i++) {
        PyArray_DTypeMeta *tmp = NULL;
        if (signature[i]) {
            tmp = signature[i];
        }
        else if (is_integer_dtype(op_dtypes[i])) {
            tmp = &PyArray_Int64DType;
        }
        else if (op_dtypes[i]) {
            tmp = op_dtypes[i];
        }
        else {
            tmp = &PyArray_StringDType;
        }
        Py_INCREF(tmp);
        new_op_dtypes[i] = tmp;
    }
    /* don't touch output dtypes if they are set */
    for (int i = ufunc->nin; i < ufunc->nargs; i++) {
        if (op_dtypes[i]) {
            Py_INCREF(op_dtypes[i]);
            new_op_dtypes[i] = op_dtypes[i];
        }
        else {
            Py_INCREF(&PyArray_StringDType);
            new_op_dtypes[i] = &PyArray_StringDType;
        }
    }
    return 0;
}

// Register a ufunc.
//
// Pass NULL for resolve_func to use the default_resolve_descriptors.
int
init_ufunc(PyObject *umath, const char *ufunc_name, PyArray_DTypeMeta **dtypes,
           PyArrayMethod_ResolveDescriptors *resolve_func,
           PyArrayMethod_StridedLoop *loop_func, int nin, int nout,
           NPY_CASTING casting, NPY_ARRAYMETHOD_FLAGS flags,
           void *static_data)
{
    PyObject *ufunc = PyObject_GetAttrString(umath, ufunc_name);
    if (ufunc == NULL) {
        return -1;
    }
    char loop_name[256] = {0};

    snprintf(loop_name, sizeof(loop_name), "string_%s", ufunc_name);

    PyArrayMethod_Spec spec;
    spec.name = loop_name;
    spec.nin = nin;
    spec.nout = nout;
    spec.casting = casting;
    spec.flags = flags;
    spec.dtypes = dtypes;
    spec.slots = NULL;

    PyType_Slot resolve_slots[] = {
            {NPY_METH_resolve_descriptors, (void *)resolve_func},
            {NPY_METH_strided_loop, (void *)loop_func},
            {_NPY_METH_static_data, static_data},
            {0, NULL}};

    spec.slots = resolve_slots;

    if (PyUFunc_AddLoopFromSpec_int(ufunc, &spec, 1) < 0) {
        Py_DECREF(ufunc);
        return -1;
    }

    Py_DECREF(ufunc);
    return 0;
}

int
add_promoter(PyObject *numpy, const char *ufunc_name,
             PyArray_DTypeMeta *dtypes[], size_t n_dtypes,
             PyArrayMethod_PromoterFunction *promoter_impl)
{
    PyObject *ufunc = PyObject_GetAttrString((PyObject *)numpy, ufunc_name);

    if (ufunc == NULL) {
        return -1;
    }

    PyObject *DType_tuple = PyTuple_New(n_dtypes);

    for (size_t i=0; i<n_dtypes; i++) {
        PyTuple_SET_ITEM(DType_tuple, i, (PyObject *)dtypes[i]);
    }


    if (DType_tuple == NULL) {
        Py_DECREF(ufunc);
        return -1;
    }

    PyObject *promoter_capsule = PyCapsule_New((void *)promoter_impl,
                                               "numpy._ufunc_promoter", NULL);

    if (promoter_capsule == NULL) {
        Py_DECREF(ufunc);
        Py_DECREF(DType_tuple);
        return -1;
    }

    if (PyUFunc_AddPromoter(ufunc, DType_tuple, promoter_capsule) < 0) {
        Py_DECREF(promoter_capsule);
        Py_DECREF(DType_tuple);
        Py_DECREF(ufunc);
        return -1;
    }

    Py_DECREF(promoter_capsule);
    Py_DECREF(DType_tuple);
    Py_DECREF(ufunc);

    return 0;
}

#define INIT_MULTIPLY(typename, shortname)                                 \
    PyArray_DTypeMeta *multiply_right_##shortname##_types[] = {            \
        &PyArray_StringDType, &PyArray_##typename##DType,                  \
        &PyArray_StringDType};                                             \
                                                                           \
    if (init_ufunc(umath, "multiply", multiply_right_##shortname##_types,  \
                   &multiply_resolve_descriptors,                          \
                   &multiply_right_strided_loop<npy_##shortname>, 2, 1,    \
                   NPY_NO_CASTING, (NPY_ARRAYMETHOD_FLAGS) 0, NULL) < 0) { \
        return -1;                                                         \
    }                                                                      \
                                                                           \
    PyArray_DTypeMeta *multiply_left_##shortname##_types[] = {             \
            &PyArray_##typename##DType, &PyArray_StringDType,              \
            &PyArray_StringDType};                                         \
                                                                           \
    if (init_ufunc(umath, "multiply", multiply_left_##shortname##_types,   \
                   &multiply_resolve_descriptors,                          \
                   &multiply_left_strided_loop<npy_##shortname>, 2, 1,     \
                   NPY_NO_CASTING, (NPY_ARRAYMETHOD_FLAGS) 0, NULL) < 0) { \
        return -1;                                                         \
    }

NPY_NO_EXPORT int
add_object_and_unicode_promoters(PyObject *umath, const char* ufunc_name,
                                 PyArrayMethod_PromoterFunction *unicode_promoter_wrapper,
                                 PyArrayMethod_PromoterFunction *object_promoter_wrapper)
{
    {
        PyArray_DTypeMeta *dtypes[] = {
            &PyArray_StringDType, &PyArray_UnicodeDType, &PyArray_BoolDType};
        if (add_promoter(umath, ufunc_name, dtypes, 3, unicode_promoter_wrapper) < 0) {
            return -1;
        }
    }

    {
        PyArray_DTypeMeta *dtypes[] = {
            &PyArray_UnicodeDType, &PyArray_StringDType, &PyArray_BoolDType};
        if (add_promoter(umath, ufunc_name, dtypes, 3, unicode_promoter_wrapper) < 0) {
            return -1;
        }
    }

    {
        PyArray_DTypeMeta *dtypes[] = {
            &PyArray_StringDType, &PyArray_ObjectDType, &PyArray_BoolDType};
        if (add_promoter(umath, ufunc_name, dtypes, 3, object_promoter_wrapper) < 0) {
            return -1;
        }
    }

    {
        PyArray_DTypeMeta *dtypes[] = {
            &PyArray_ObjectDType, &PyArray_StringDType, &PyArray_BoolDType};
        if (add_promoter(umath, ufunc_name, dtypes, 3, object_promoter_wrapper) < 0) {
            return -1;
        }
    }
    return 0;
}

NPY_NO_EXPORT int
init_stringdtype_ufuncs(PyObject *umath)
{
    static const char *comparison_ufunc_names[6] = {
            "equal", "not_equal",
            "less", "less_equal", "greater_equal", "greater",
    };

    PyArray_DTypeMeta *comparison_dtypes[] = {
            &PyArray_StringDType,
            &PyArray_StringDType, &PyArray_BoolDType};

    // eq and ne get recognized in string_cmp_strided_loop by having
    // res_for_lt == res_for_gt.
    static npy_bool comparison_ufunc_eq_lt_gt_results[6*3] = {
        NPY_TRUE, NPY_FALSE, NPY_FALSE, // eq: results for eq, lt, gt
        NPY_FALSE, NPY_TRUE, NPY_TRUE,  // ne
        NPY_FALSE, NPY_TRUE, NPY_FALSE, // lt
        NPY_TRUE, NPY_TRUE, NPY_FALSE,  // le
        NPY_TRUE, NPY_FALSE, NPY_TRUE,  // gt
        NPY_FALSE, NPY_FALSE, NPY_TRUE, // ge
    };

    for (int i = 0; i < 6; i++) {
        if (init_ufunc(umath, comparison_ufunc_names[i], comparison_dtypes,
                       &string_comparison_resolve_descriptors,
                       &string_comparison_strided_loop, 2, 1, NPY_NO_CASTING,
                       (NPY_ARRAYMETHOD_FLAGS) 0,
                       &comparison_ufunc_eq_lt_gt_results[i*3]) < 0) {
            return -1;
        }

        if (add_object_and_unicode_promoters(
                    umath, comparison_ufunc_names[i],
                    &string_unicode_bool_output_promoter,
                    &string_object_bool_output_promoter) < 0) {
            return -1;
        }
    }

    PyArray_DTypeMeta *bool_output_dtypes[] = {
        &PyArray_StringDType,
        &PyArray_BoolDType
    };

    if (init_ufunc(umath, "isnan", bool_output_dtypes,
                   &string_bool_output_resolve_descriptors,
                   &string_isnan_strided_loop, 1, 1, NPY_NO_CASTING,
                   (NPY_ARRAYMETHOD_FLAGS) 0, NULL) < 0) {
        return -1;
    }

    const char *unary_loop_names[] = {
        "isalpha", "isdecimal", "isdigit", "isnumeric", "isspace",
    };
    // Note: these are member function pointers, not regular function
    // function pointers, so we need to pass on their address, not value.
    static utf8_buffer_method unary_loop_buffer_methods[] = {
        &Buffer<ENCODING::UTF8>::isalpha,
        &Buffer<ENCODING::UTF8>::isdecimal,
        &Buffer<ENCODING::UTF8>::isdigit,
        &Buffer<ENCODING::UTF8>::isnumeric,
        &Buffer<ENCODING::UTF8>::isspace,
    };
    for (int i=0; i<5; i++) {
        if (init_ufunc(umath, unary_loop_names[i], bool_output_dtypes,
                       &string_bool_output_resolve_descriptors,
                       &string_bool_output_unary_strided_loop, 1, 1, NPY_NO_CASTING,
                       (NPY_ARRAYMETHOD_FLAGS) 0,
                       &unary_loop_buffer_methods[i]) < 0) {
            return -1;
        }
    }

    PyArray_DTypeMeta *intp_output_dtypes[] = {
        &PyArray_StringDType,
        &PyArray_IntpDType
    };

    if (init_ufunc(umath, "str_len", intp_output_dtypes,
                   &string_intp_output_resolve_descriptors,
                   &string_strlen_strided_loop, 1, 1, NPY_NO_CASTING,
                   (NPY_ARRAYMETHOD_FLAGS) 0, NULL) < 0) {
        return -1;
    }

    PyArray_DTypeMeta *binary_dtypes[] = {
            &PyArray_StringDType,
            &PyArray_StringDType,
            &PyArray_StringDType,
    };

    const char* minimum_maximum_names[] = {"minimum", "maximum"};

    static npy_bool minimum_maximum_invert[2] = {NPY_FALSE, NPY_TRUE};

    for (int i = 0; i < 2; i++) {
        if (init_ufunc(umath, minimum_maximum_names[i],
                       binary_dtypes, binary_resolve_descriptors,
                       &minimum_maximum_strided_loop, 2, 1, NPY_NO_CASTING,
                       (NPY_ARRAYMETHOD_FLAGS) 0,
                       &minimum_maximum_invert[i]) < 0) {
            return -1;
        }
    }

    if (init_ufunc(umath, "add", binary_dtypes, binary_resolve_descriptors,
                   &add_strided_loop, 2, 1, NPY_NO_CASTING,
                   (NPY_ARRAYMETHOD_FLAGS) 0, NULL) < 0) {
        return -1;
    }

    INIT_MULTIPLY(Int64, int64);
    INIT_MULTIPLY(UInt64, uint64);

    // all other integer dtypes are handled with a generic promoter

    PyArray_DTypeMeta *rdtypes[] = {
        &PyArray_StringDType,
        (PyArray_DTypeMeta *)Py_None,
        &PyArray_StringDType};

    if (add_promoter(umath, "multiply", rdtypes, 3, string_multiply_promoter) < 0) {
        return -1;
    }

    PyArray_DTypeMeta *ldtypes[] = {
        (PyArray_DTypeMeta *)Py_None,
        &PyArray_StringDType,
        &PyArray_StringDType};

    if (add_promoter(umath, "multiply", ldtypes, 3, string_multiply_promoter) < 0) {
        return -1;
    }

    PyArray_DTypeMeta *find_rfind_count_dtypes[] = {
        &PyArray_StringDType, &PyArray_StringDType,
        &PyArray_Int64DType, &PyArray_Int64DType,
        &PyArray_DefaultIntDType,
    };

    const char* find_rfind_count_names[] = {
        "find", "rfind", "count",
    };

    PyArray_DTypeMeta *find_rfind_count_promoter_dtypes[] = {
        &PyArray_StringDType, &PyArray_UnicodeDType,
        &PyArray_PyIntAbstractDType, &PyArray_PyIntAbstractDType,
        &PyArray_DefaultIntDType,
    };

    find_like_function *find_rfind_count_functions[] = {
        string_find<ENCODING::UTF8>,
        string_rfind<ENCODING::UTF8>,
        string_count<ENCODING::UTF8>,
    };

    for (int i=0; i<3; i++) {
        if (init_ufunc(umath, find_rfind_count_names[i], find_rfind_count_dtypes,
                       &string_find_rfind_count_resolve_descriptors,
                       &string_find_rfind_count_strided_loop, 4, 1, NPY_NO_CASTING,
                       (NPY_ARRAYMETHOD_FLAGS) 0,
                       (void *)find_rfind_count_functions[i]) < 0) {
            return -1;
        }


        if (add_promoter(umath, find_rfind_count_names[i],
                         find_rfind_count_promoter_dtypes,
                         5, string_find_rfind_count_promoter) < 0) {
            return -1;
        }
    }

    PyArray_DTypeMeta *startswith_endswith_dtypes[] = {
        &PyArray_StringDType, &PyArray_StringDType,
        &PyArray_Int64DType, &PyArray_Int64DType,
        &PyArray_BoolDType,
    };

    const char* startswith_endswith_names[] = {
        "startswith", "endswith",
    };

    PyArray_DTypeMeta *startswith_endswith_promoter_dtypes[] = {
        &PyArray_StringDType, &PyArray_UnicodeDType,
        &PyArray_PyIntAbstractDType, &PyArray_PyIntAbstractDType,
        &PyArray_BoolDType,
    };

    static STARTPOSITION startswith_endswith_startposition[] = {
        STARTPOSITION::FRONT,
        STARTPOSITION::BACK,
    };

    for (int i=0; i<2; i++) {
        if (init_ufunc(umath, startswith_endswith_names[i], startswith_endswith_dtypes,
                       &string_startswith_endswith_resolve_descriptors,
                       &string_startswith_endswith_strided_loop,
                       4, 1, NPY_NO_CASTING, (NPY_ARRAYMETHOD_FLAGS) 0,
                       &startswith_endswith_startposition[i]) < 0) {
            return -1;
        }


        if (add_promoter(umath, startswith_endswith_names[i],
                         startswith_endswith_promoter_dtypes,
                         5, string_startswith_endswith_promoter) < 0) {
            return -1;
        }
    }

    PyArray_DTypeMeta *strip_whitespace_dtypes[] = {
        &PyArray_StringDType, &PyArray_StringDType
    };

    const char *strip_whitespace_names[] = {
        "_lstrip_whitespace", "_rstrip_whitespace", "_strip_whitespace",
    };

    static STRIPTYPE strip_types[] = {
        STRIPTYPE::LEFTSTRIP,
        STRIPTYPE::RIGHTSTRIP,
        STRIPTYPE::BOTHSTRIP,
    };

    for (int i=0; i<3; i++) {
        if (init_ufunc(umath, strip_whitespace_names[i], strip_whitespace_dtypes,
                       &strip_whitespace_resolve_descriptors,
                       &string_lrstrip_whitespace_strided_loop,
                       1, 1, NPY_NO_CASTING, (NPY_ARRAYMETHOD_FLAGS) 0,
                       &strip_types[i]) < 0) {
            return -1;
        }
    }

    PyArray_DTypeMeta *strip_chars_dtypes[] = {
        &PyArray_StringDType, &PyArray_StringDType, &PyArray_StringDType
    };

    const char *strip_chars_names[] = {
        "_lstrip_chars", "_rstrip_chars", "_strip_chars",
    };

    PyArray_DTypeMeta *strip_chars_promoter_dtypes[] = {
        &PyArray_StringDType, &PyArray_UnicodeDType, &PyArray_StringDType
    };

    for (int i=0; i<3; i++) {
        if (init_ufunc(umath, strip_chars_names[i], strip_chars_dtypes,
                       &strip_chars_resolve_descriptors,
                       &string_lrstrip_chars_strided_loop,
                       2, 1, NPY_NO_CASTING, (NPY_ARRAYMETHOD_FLAGS) 0,
                       &strip_types[i]) < 0) {
            return -1;
        }

        if (add_promoter(umath, strip_chars_names[i],
                         strip_chars_promoter_dtypes, 3, strip_chars_promoter) < 0) {
            return -1;
        }
    }


    PyArray_DTypeMeta *replace_dtypes[] = {
        &PyArray_StringDType, &PyArray_StringDType, &PyArray_StringDType,
        &PyArray_Int64DType, &PyArray_StringDType,
    };

    if (init_ufunc(umath, "_replace", replace_dtypes,
                   &replace_resolve_descriptors,
                   &string_replace_strided_loop, 4, 1,
                   NPY_NO_CASTING,
                   (NPY_ARRAYMETHOD_FLAGS) 0, NULL) < 0) {
        return -1;
    }

    PyArray_DTypeMeta *replace_promoter_pyint_dtypes[] = {
        &PyArray_StringDType, &PyArray_UnicodeDType, &PyArray_UnicodeDType,
        &PyArray_PyIntAbstractDType, &PyArray_StringDType,
    };

    if (add_promoter(umath, "_replace", replace_promoter_pyint_dtypes, 5,
                     string_replace_promoter) < 0) {
        return -1;
    }

    PyArray_DTypeMeta *replace_promoter_int64_dtypes[] = {
        &PyArray_StringDType, &PyArray_UnicodeDType, &PyArray_UnicodeDType,
        &PyArray_Int64DType, &PyArray_StringDType,
    };

    if (add_promoter(umath, "_replace", replace_promoter_int64_dtypes, 5,
                     string_replace_promoter) < 0) {
        return -1;
    }

    return 0;
}
