#ifndef LE_CODEBUFFER_H
#define LE_CODEBUFFER_H




typedef uint8_T le_mem_t;

typedef struct {
    PyObject_HEAD
    le_mem_t *pos;
    le_mem_t *soft_limit;
    PyObject *held_references;
} LeWriter;



typedef struct {
    PyObject_HEAD
    Py_ssize_t pos;
    unsigned char *buffer;
    Py_ssize_t len;
    PyObject held_references;
    PyObject linked_buffers;
} LeCodeBuffer;

LeCodeBuffer_NewMain();
LeCodeBuffer_NewAux();


le_mem_t *le_writer__begin(LeWriter *w);
int le_writer__end(LeWriter *w, le_mem_t *new_pos);


void le_writer
void le_writer__write_n(LeWriter *w, int n, uint8_t *v);
void le_writer__write_int8(LeWriter *w, int8_t v);
void le_writer__write_uint8(LeWriter *w, uint8_t v);
void le_writer__write_int16(LeWriter *w, int16_t v);
void le_writer__write_uint16(LeWriter *w, uint16_t v);
void le_writer__write_int32(LeWriter *w, int32_t v);
void le_writer__write_uint32(LeWriter *w, uint32_t v);
void le_writer__write_int64(LeWriter *w, int64_t v);
void le_writer__write_uint64(LeWriter *w, uint64_t v);





#endif /* LE_CODEBUFFER_H */
