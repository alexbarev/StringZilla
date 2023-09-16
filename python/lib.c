/**
 *  @brief  Very light-weight CPython wrapper for StringZilla, with support for memory-mapping,
 *          native Python strings, Apache Arrow collections, and more.
 */
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h> // `stat`
#include <sys/mman.h> // `mmap`
#include <fcntl.h>    // `O_RDNLY`
#endif

#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <unistd.h> // `ssize_t`
#endif

#include <Python.h>

#include <stringzilla.h>

#pragma region Forward Declarations

static PyTypeObject MemoryMappedFileType;
static PyTypeObject StrType;

/**
 *  @brief  Describes an on-disk file mapped into RAM, which is different from Python's
 *          native `mmap` module, as it exposes the address of the mapping in memory.
 */
typedef struct {
    PyObject_HEAD;
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    HANDLE file_handle;
    HANDLE mapping_handle;
#else
    int file_descriptor;
#endif
    void *start;
    size_t length;
} MemoryMappedFile;

/**
 *  @brief  Type-punned StringZilla-string, that points to a slice of an existing Python `str`
 *          or a `MemoryMappedFile`.
 *
 *  When a slice is constructed, the `parent` object's reference count is being incremented to preserve lifetime.
 *  It usage in Python would look like:
 *
 *      - Str() # Empty string
 *      - Str("some-string") # Full-range slice of a Python `str`
 *      - Str(File("some-path.txt")) # Full-range view of a persisted file
 *      - Str(File("some-path.txt"), from=0, to=sys.maxint)
 */
typedef struct {
    PyObject_HEAD;
    PyObject *parent;
    char const *start;
    size_t length;
} Str;

#pragma endregion

#pragma region Helpers

void slice(size_t length, ssize_t start, ssize_t end, size_t *normalized_offset, size_t *normalized_length) {

    // clang-format off
    // Normalize negative indices
    if (start < 0) start += length;
    if (end < 0) end += length;

    // Clamp indices to a valid range
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > (ssize_t)length) start = length;
    if (end > (ssize_t)length) end = length;

    // Ensure start <= end
    if (start > end) start = end;
    // clang-format on

    *normalized_offset = start;
    *normalized_length = end - start;
}

int export_string_like(PyObject *object, char const **start, size_t *length) {
    if (PyUnicode_Check(object)) {
        // Handle Python str
        Py_ssize_t signed_length;
        *start = PyUnicode_AsUTF8AndSize(object, &signed_length);
        *length = (size_t)signed_length;
        return 1;
    }
    else if (PyBytes_Check(object)) {
        // Handle Python str
        Py_ssize_t signed_length;
        if (PyBytes_AsStringAndSize(object, (char **)start, &signed_length) == -1) {
            PyErr_SetString(PyExc_TypeError, "Mapping bytes failed");
            return 0;
        }
        *length = (size_t)signed_length;
        return 1;
    }
    else if (PyObject_TypeCheck(object, &StrType)) {
        Str *str = (Str *)object;
        *start = str->start;
        *length = str->length;
        return 1;
    }
    else if (PyObject_TypeCheck(object, &MemoryMappedFileType)) {
        MemoryMappedFile *file = (MemoryMappedFile *)object;
        *start = file->start;
        *length = file->length;
        return 1;
    }
    return 0;
}

#pragma endregion

#pragma region Global Functions

static PyObject *str_find_vectorcall(PyObject *_, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    // Check the number of arguments and types
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs < 2 || nargs > 4) {
        PyErr_SetString(PyExc_TypeError, "Invalid arguments");
        return NULL;
    }

    // Parse the haystack.
    PyObject *haystack_obj = args[0];
    struct strzl_haystack_t haystack;
    if (!export_string_like(haystack_obj, &haystack.ptr, &haystack.len)) {
        PyErr_SetString(PyExc_TypeError, "First argument (haystack) must be string-like");
        return NULL;
    }

    // Parse the needle.
    PyObject *needle_obj = args[1];
    struct strzl_needle_t needle;
    needle.anomaly_offset = 0;
    if (!export_string_like(needle_obj, &needle.ptr, &needle.len)) {
        PyErr_SetString(PyExc_TypeError, "Second argument (needle) must be string-like");
        return NULL;
    }

    // Limit the haystack range.
    Py_ssize_t start = (nargs > 2) ? PyLong_AsSsize_t(args[2]) : 0;
    Py_ssize_t end = (nargs > 3) ? PyLong_AsSsize_t(args[3]) : PY_SSIZE_T_MAX;
    size_t normalized_offset, normalized_length;
    slice(haystack.len, start, end, &normalized_offset, &normalized_length);

    haystack.ptr = haystack.ptr + normalized_offset;
    haystack.len = normalized_length;
    size_t position = strzl_neon_find_substr(haystack, needle);
    return PyLong_FromSize_t(position);
}

#pragma endregion

#pragma region MemoryMappingFile

static void MemoryMappedFile_dealloc(MemoryMappedFile *self) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    if (self->start) {
        UnmapViewOfFile(self->start);
        self->start = NULL;
    }
    if (self->mapping_handle) {
        CloseHandle(self->mapping_handle);
        self->mapping_handle = NULL;
    }
    if (self->file_handle) {
        CloseHandle(self->file_handle);
        self->file_handle = NULL;
    }
#else
    if (self->start) {
        munmap(self->start, self->length);
        self->start = NULL;
        self->length = 0;
    }
    if (self->file_descriptor != 0) {
        close(self->file_descriptor);
        self->file_descriptor = 0;
    }
#endif
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *MemoryMappedFile_new(PyTypeObject *type, PyObject *positional_args, PyObject *named_args) {
    MemoryMappedFile *self;
    self = (MemoryMappedFile *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    self->file_handle = NULL;
    self->mapping_handle = NULL;
#else
    self->file_descriptor = 0;
#endif
    self->start = NULL;
    self->length = 0;
}

static int MemoryMappedFile_init(MemoryMappedFile *self, PyObject *positional_args, PyObject *named_args) {
    const char *path;
    if (!PyArg_ParseTuple(positional_args, "s", &path))
        return -1;

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    self->file_handle = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (self->file_handle == INVALID_HANDLE_VALUE) {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }

    self->mapping_handle = CreateFileMapping(self->file_handle, 0, PAGE_READONLY, 0, 0, 0);
    if (self->mapping_handle == 0) {
        CloseHandle(self->file_handle);
        self->file_handle = NULL;
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }

    char *file = (char *)MapViewOfFile(self->mapping_handle, FILE_MAP_READ, 0, 0, 0);
    if (file == 0) {
        CloseHandle(self->mapping_handle);
        self->mapping_handle = NULL;
        CloseHandle(self->file_handle);
        self->file_handle = NULL;
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }
    self->start = file;
    self->length = GetFileSize(self->file_handle, 0);
#else
    struct stat sb;
    self->file_descriptor = open(path, O_RDONLY);
    if (fstat(self->file_descriptor, &sb) != 0) {
        close(self->file_descriptor);
        self->file_descriptor = 0;
        PyErr_SetString(PyExc_RuntimeError, "Can't retrieve file size!");
        return -1;
    }
    size_t file_size = sb.st_size;
    void *map = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, self->file_descriptor, 0);
    if (map == MAP_FAILED) {
        close(self->file_descriptor);
        self->file_descriptor = 0;
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }
    self->start = map;
    self->length = file_size;
#endif

    return 0;
}

static PyMethodDef MemoryMappedFile_methods[] = { //
    {NULL, NULL, 0, NULL}};

static PyTypeObject MemoryMappedFileType = {
    PyObject_HEAD_INIT(NULL).tp_name = "stringzilla.MemoryMappedFile",
    .tp_doc = "Memory mapped file class, that exposes the memory range for low-level access",
    .tp_basicsize = sizeof(MemoryMappedFile),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = MemoryMappedFile_methods,
    .tp_new = (newfunc)MemoryMappedFile_new,
    .tp_init = (initproc)MemoryMappedFile_init,
    .tp_dealloc = (destructor)MemoryMappedFile_dealloc,

    // PyBufferProcs *tp_as_buffer;

    // reprfunc tp_repr;
    // PyNumberMethods *tp_as_number;
    // PySequenceMethods *tp_as_sequence;
    // PyMappingMethods *tp_as_mapping;
    // ternaryfunc tp_call;
    // reprfunc tp_str;
    // getattrofunc tp_getattro;
    // setattrofunc tp_setattro;
};

#pragma endregion

#pragma region Str

static int Str_init(Str *self, PyObject *positional_args, PyObject *named_args) {
    PyObject *parent = NULL;
    Py_ssize_t from = 0;
    Py_ssize_t to = PY_SSIZE_T_MAX;

    // The `named_args` would be `NULL`
    if (named_args) {
        static char *names[] = {"parent", "from", "to", NULL};
        if (!PyArg_ParseTupleAndKeywords(positional_args, named_args, "|Onn", names, &parent, &from, &to))
            return -1;
    }
    else if (!PyArg_ParseTuple(positional_args, "|Onn", &parent, &from, &to))
        return -1;

    // Handle empty string
    if (parent == NULL) {
        self->start = NULL;
        self->length = 0;
    }
    // Increment the reference count of the parent
    else if (export_string_like(parent, &self->start, &self->length)) {
        self->parent = parent;
        Py_INCREF(parent);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "Unsupported parent type");
        return -1;
    }

    // Apply slicing
    size_t normalized_offset, normalized_length;
    slice(self->length, from, to, &normalized_offset, &normalized_length);
    self->start = ((char *)self->start) + normalized_offset;
    self->length = normalized_length;
    return 0;
}

static PyObject *Str_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    Str *self;
    self = (Str *)type->tp_alloc(type, 0);
    if (!self)
        return NULL;

    self->parent = NULL;
    self->start = NULL;
    self->length = 0;
    return (PyObject *)self;
}

static void Str_dealloc(Str *self) {
    if (self->parent)
        Py_XDECREF(self->parent);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static Py_ssize_t Str_len(Str *self) { return self->length; }

static PyObject *Str_getitem(Str *self, Py_ssize_t i) {

    // Negative indexing
    if (i < 0)
        i += self->length;

    if (i < 0 || (size_t)i >= self->length) {
        PyErr_SetString(PyExc_IndexError, "Index out of range");
        return NULL;
    }

    // Assuming the underlying data is UTF-8 encoded
    return PyUnicode_FromStringAndSize(self->start + i, 1);
}

static PyObject *Str_subscript(Str *self, PyObject *key) {
    if (PySlice_Check(key)) {
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0)
            return NULL;
        if (PySlice_AdjustIndices(self->length, &start, &stop, step) < 0)
            return NULL;

        if (step != 1) {
            PyErr_SetString(PyExc_IndexError, "Efficient step is not supported");
            return NULL;
        }

        // Create a new `Str` object
        Str *self_slice = (Str *)StrType.tp_alloc(&StrType, 0);
        if (self_slice == NULL && PyErr_NoMemory())
            return NULL;

        // Set its properties based on the slice
        self_slice->start = self->start + start;
        self_slice->length = stop - start;
        self_slice->parent = (PyObject *)self; // Set parent to keep it alive

        // Increment the reference count of the parent
        Py_INCREF(self);
        return (PyObject *)self_slice;
    }
    else if (PyLong_Check(key)) {
        return Str_getitem(self, PyLong_AsSsize_t(key));
    }
    else {
        PyErr_SetString(PyExc_TypeError, "Str indices must be integers or slices");
        return NULL;
    }
}

// Will be called by the `PySequence_Contains`
static int Str_contains(Str *self, PyObject *arg) {

    struct strzl_needle_t needle_struct;
    needle_struct.anomaly_offset = 0;
    if (!export_string_like(arg, &needle_struct.ptr, &needle_struct.len)) {
        PyErr_SetString(PyExc_TypeError, "Unsupported argument type");
        return -1;
    }

    struct strzl_haystack_t haystack;
    haystack.ptr = self->start;
    haystack.len = self->length;
    size_t position = strzl_neon_find_substr(haystack, needle_struct);
    return position != haystack.len;
}

static Py_hash_t Str_hash(Str *self) { return (Py_hash_t)strzl_hash_crc32_native(self->start, self->length); }

static PyObject *Str_getslice(Str *self, PyObject *args) {
    PyObject *start_obj = NULL, *end_obj = NULL;
    ssize_t start = 0, end = self->length; // Default values

    if (!PyArg_ParseTuple(args, "|OO", &start_obj, &end_obj))
        return NULL;

    if (start_obj != NULL && start_obj != Py_None) {
        if (!PyLong_Check(start_obj)) {
            PyErr_SetString(PyExc_TypeError, "Start index must be an integer or None");
            return NULL;
        }
        start = PyLong_AsSsize_t(start_obj);
    }

    if (end_obj != NULL && end_obj != Py_None) {
        if (!PyLong_Check(end_obj)) {
            PyErr_SetString(PyExc_TypeError, "End index must be an integer or None");
            return NULL;
        }
        end = PyLong_AsSsize_t(end_obj);
    }

    size_t normalized_offset, normalized_length;
    slice(self->length, start, end, &normalized_offset, &normalized_length);

    if (normalized_length == 0)
        return PyUnicode_FromString("");

    // Create a new Str object
    Str *new_str = (Str *)PyObject_New(Str, &StrType);
    if (new_str == NULL)
        return NULL;

    // Set the parent to the original Str object and increment its reference count
    new_str->parent = (PyObject *)self;
    Py_INCREF(self);

    // Set the start and length to point to the slice
    new_str->start = self->start + normalized_offset;
    new_str->length = normalized_length;
    return (PyObject *)new_str;
}

static PyObject *Str_str(Str *self, PyObject *args) { return PyUnicode_FromStringAndSize(self->start, self->length); }

static PyObject *Str_richcompare(PyObject *self, PyObject *other, int op) {

    char const *a_start, *b_start;
    size_t a_length, b_length;
    if (!export_string_like(self, &a_start, &a_length) || !export_string_like(other, &b_start, &b_length))
        Py_RETURN_NOTIMPLEMENTED;

    // Perform byte-wise comparison up to the minimum length
    size_t min_length = a_length < b_length ? a_length : b_length;
    int cmp_result = memcmp(a_start, b_start, min_length);

    // If the strings are equal up to `min_length`, then the shorter string is smaller
    if (cmp_result == 0)
        cmp_result = (a_length > b_length) - (a_length < b_length);

    switch (op) {
    case Py_LT: return PyBool_FromLong(cmp_result < 0);
    case Py_LE: return PyBool_FromLong(cmp_result <= 0);
    case Py_EQ: return PyBool_FromLong(cmp_result == 0);
    case Py_NE: return PyBool_FromLong(cmp_result != 0);
    case Py_GT: return PyBool_FromLong(cmp_result > 0);
    case Py_GE: return PyBool_FromLong(cmp_result >= 0);
    default: Py_RETURN_NOTIMPLEMENTED;
    }
}

static PySequenceMethods Str_as_sequence = {
    .sq_length = Str_len,        //
    .sq_item = Str_getitem,      //
    .sq_contains = Str_contains, //
};

static PyMappingMethods Str_as_mapping = {
    .mp_length = Str_len,          //
    .mp_subscript = Str_subscript, // Is used to implement slices in Python
};

static PyMethodDef Str_methods[] = { //
    {"contains", (PyCFunction)Str_str, METH_NOARGS, "Convert to Python `str`"},
    // {"find", (PyCFunction)Str_len, METH_NOARGS, "Get length"},
    // {"__getitem__", (PyCFunction)Str_getitem, METH_O, "Indexing"},
    {NULL, NULL, 0, NULL}};

static PyTypeObject StrType = {
    PyObject_HEAD_INIT(NULL).tp_name = "stringzilla.Str",
    .tp_doc = "Immutable string/slice class with SIMD and SWAR-accelerated operations",
    .tp_basicsize = sizeof(Str),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = Str_methods,
    .tp_new = Str_new,
    .tp_init = Str_init,
    .tp_dealloc = Str_dealloc,
    .tp_as_sequence = &Str_as_sequence,
    .tp_as_mapping = &Str_as_mapping,
    .tp_hash = Str_hash, // String hashing functions
    .tp_richcompare = Str_richcompare,
    // .tp_as_buffer = (PyBufferProcs *)NULL, // Functions to access object as input/output buffer
};

#pragma endregion

static PyMethodDef stringzilla_methods[] = { //
    {NULL, NULL, 0, NULL}};

static PyModuleDef stringzilla_module = {
    PyModuleDef_HEAD_INIT,
    "stringzilla",
    "Crunch 100+ GB Strings in Python with ease",
    -1,
    stringzilla_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

// String functions:
static PyObject *vectorized_find = NULL;
static PyObject *vectorized_count = NULL;
static PyObject *vectorized_contains = NULL;
static PyObject *vectorized_levenstein = NULL;

// String collections:
static PyObject *vectorized_split = NULL;
static PyObject *vectorized_sort = NULL;
static PyObject *vectorized_shuffle = NULL;

PyMODINIT_FUNC PyInit_stringzilla(void) {
    PyObject *m;

    if (PyType_Ready(&StrType) < 0)
        return NULL;

    if (PyType_Ready(&MemoryMappedFileType) < 0)
        return NULL;

    m = PyModule_Create(&stringzilla_module);
    if (m == NULL)
        return NULL;

    Py_INCREF(&StrType);
    if (PyModule_AddObject(m, "Str", (PyObject *)&StrType) < 0) {
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        return NULL;
    }

    Py_INCREF(&MemoryMappedFileType);
    if (PyModule_AddObject(m, "MemoryMappedFile", (PyObject *)&MemoryMappedFileType) < 0) {
        Py_XDECREF(&MemoryMappedFileType);
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        return NULL;
    }

    // Create the 'find' function
    vectorized_find = PyObject_Malloc(sizeof(PyCFunctionObject));
    if (vectorized_find == NULL) {
        Py_XDECREF(&MemoryMappedFileType);
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        PyErr_NoMemory();
        return NULL;
    }
    PyObject_Init(vectorized_find, &PyCFunction_Type);
    ((PyCFunctionObject *)vectorized_find)->m_ml = NULL; // No regular PyMethodDef
    ((PyCFunctionObject *)vectorized_find)->vectorcall = str_find_vectorcall;

    // Add the 'find' function to the module
    if (PyModule_AddObject(m, "find", vectorized_find) < 0) {
        PyObject_Free(vectorized_find);
        Py_XDECREF(&MemoryMappedFileType);
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        return NULL;
    }

    return m;

cleanup:
    if (vectorized_find)
        Py_XDECREF(vectorized_find);
    if (vectorized_count)
        Py_XDECREF(vectorized_count);
    if (vectorized_contains)
        Py_XDECREF(vectorized_contains);
    if (vectorized_split)
        Py_XDECREF(vectorized_split);
    if (vectorized_sort)
        Py_XDECREF(vectorized_sort);
    if (vectorized_shuffle)
        Py_XDECREF(vectorized_shuffle);
    Py_XDECREF(m);
    PyErr_NoMemory();
    return NULL;
}
