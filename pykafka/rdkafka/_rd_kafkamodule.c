#include <Python.h>
#include <librdkafka/rdkafka.h>


typedef struct {
    PyObject_HEAD
    rd_kafka_t *rdk_handle;
    rd_kafka_queue_t *rdk_queue_handle;
    rd_kafka_topic_t *rdk_topic_handle;
    PyObject *partition_ids;
} Consumer;


static void
Consumer_dealloc(Consumer *self) {
    // Call stop on all partitions, then destroy all handles

    if (self->rdk_topic_handle != NULL) {
        Py_ssize_t i, len = PyList_Size(self->partition_ids);
        for (i = 0; i != len; ++i) {
            long part_id = PyInt_AsLong(PyList_GetItem(self->partition_ids, i));
            if (part_id == -1) {
                // An error occurred, but we'll have to try mop op the rest
                // as best we can.  TODO log this
                continue;
            }
            if (-1 == rd_kafka_consume_stop(self->rdk_topic_handle, part_id)) {
                // TODO check errno, log this
                continue;
            }
        }
        Py_CLEAR(self->partition_ids);
        rd_kafka_topic_destroy(self->rdk_topic_handle);
        self->rdk_topic_handle = NULL;
    }
    if (self->rdk_queue_handle != NULL) {
        rd_kafka_queue_destroy(self->rdk_queue_handle);
        self->rdk_queue_handle = NULL;
    }
    if (self->rdk_handle != NULL) {
        rd_kafka_destroy(self->rdk_handle);
        self->rdk_handle = NULL;
    }
    self->ob_type->tp_free((PyObject*)self);
}


static int
Consumer_init(Consumer *self, PyObject *args, PyObject *kwds) {
    char *keywords[] = {
        "brokers",
        "topic_name",
        "partition_ids",
        "start_offsets",  // same order as partition_ids
        NULL};
    const char *brokers = NULL;
    const char *topic_name = NULL;
    PyObject *partition_ids = NULL;
    PyObject *start_offsets= NULL;
    if (! PyArg_ParseTupleAndKeywords(args,
                                      kwds,
                                      "ssOO",
                                      keywords,
                                      &brokers,
                                      &topic_name,
                                      &partition_ids,
                                      &start_offsets)) {
        return -1;
    }

    // We'll keep our own copy of partition_ids, because the one handed to us
    // might be mutable, and weird things could happen if the list used on init
    // is different than that on dealloc
    if (self->partition_ids) {
        // TODO set exception, expected a fresh Consumer
        return -1;
    }
    self->partition_ids = PySequence_List(partition_ids);
    if (! self->partition_ids) return -1;

    // Configure and start a new RD_KAFKA_CONSUMER
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    char errstr[512];
    self->rdk_handle = rd_kafka_new(
            RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (! self->rdk_handle) return 0;  // TODO set exception, return -1
    if (rd_kafka_brokers_add(self->rdk_handle, brokers) == 0) {
        // TODO set exception, return -1
        // XXX add brokers via conf setting instead?
        return 0;
    }

    // Configure and take out a topic handle
    // TODO disable offset-storage etc
    rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();
    self->rdk_topic_handle =
        rd_kafka_topic_new(self->rdk_handle, topic_name, topic_conf);

    // Start a queue and add all partition_ids to it
    self->rdk_queue_handle = rd_kafka_queue_new(self->rdk_handle);
    if (! self->rdk_queue_handle) return 0;  // TODO set exception, return -1
    Py_ssize_t i, len = PyList_Size(self->partition_ids);
    for (i = 0; i != len; ++i) {
        // We don't do much type-checking on partition_ids/start_offsets as this
        // module is intended solely for use with the py class that wraps it
        int32_t part_id = PyInt_AsLong(PyList_GetItem(self->partition_ids, i));
        if (part_id == -1 && PyErr_Occurred()) return -1;
        PyObject *offset_obj = PySequence_GetItem(start_offsets, i);
        if (! offset_obj) return -1;  // shorter seq than partition_ids?
        int64_t offset = PyLong_AsLongLong(offset_obj);
        if (-1 == rd_kafka_consume_start_queue(self->rdk_topic_handle,
                                               part_id,
                                               offset,
                                               self->rdk_queue_handle)) {
            // TODO set exception
            return -1;
        }
    }
    return 0;
}


static PyObject *
Consumer_consume(PyObject *self, PyObject *args) {
    int timeout_ms = 0;
    if (! PyArg_ParseTuple(args, "i", &timeout_ms)) return NULL;

    rd_kafka_message_t *rkmessage;
    rkmessage = rd_kafka_consume_queue(((Consumer *)self)->rdk_queue_handle,
                                       timeout_ms);
    if (!rkmessage) {
        // Either ETIMEDOUT or ENOENT occurred, but the latter would imply we
        // forgot to call rd_kafka_consume_start_queue, which is unlikely in
        // this setup.  We'll assume it was ETIMEDOUT then:
        Py_INCREF(Py_None);
        return Py_None;
    }
    // TODO check rkmessage->err - especially handle PARTITION_EOF!
    PyObject *retval = Py_BuildValue("s#s#lL",
                                     rkmessage->payload, rkmessage->len,
                                     rkmessage->key, rkmessage->key_len,
                                     rkmessage->partition,
                                     rkmessage->offset);
    rd_kafka_message_destroy(rkmessage);
    return retval;
}


static PyMethodDef Consumer_methods[] = {
    {"consume", Consumer_consume, METH_VARARGS, "Consume from kafka."},
    {NULL, NULL, 0, NULL}
};


static PyTypeObject ConsumerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pykafka.rd_kafka.Consumer",
    sizeof(Consumer),
    0,                             /*tp_itemsize*/
    (destructor)Consumer_dealloc,  /*tp_dealloc*/
    0,                             /*tp_print*/
    0,                             /*tp_getattr*/
    0,                             /*tp_setattr*/
    0,                             /*tp_compare*/
    0,                             /*tp_repr*/
    0,                             /*tp_as_number*/
    0,                             /*tp_as_sequence*/
    0,                             /*tp_as_mapping*/
    0,                             /*tp_hash */
    0,                             /*tp_call*/
    0,                             /*tp_str*/
    0,                             /*tp_getattro*/
    0,                             /*tp_setattro*/
    0,                             /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,            /*tp_flags*/
    0,                             /* tp_doc */
    0,                             /* tp_traverse */
    0,                             /* tp_clear */
    0,                             /* tp_richcompare */
    0,                             /* tp_weaklistoffset */
    0,                             /* tp_iter */
    0,                             /* tp_iternext */
    Consumer_methods,              /* tp_methods */
    0,                             /* tp_members */
    0,                             /* tp_getset */
    0,                             /* tp_base */
    0,                             /* tp_dict */
    0,                             /* tp_descr_get */
    0,                             /* tp_descr_set */
    0,                             /* tp_dictoffset */
    (initproc)Consumer_init,       /* tp_init */
};


PyMODINIT_FUNC
init_rd_kafka(void) {
    PyObject *mod = Py_InitModule("pykafka.rdkafka._rd_kafka", NULL);
    if (mod == NULL) return;

    ConsumerType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&ConsumerType) != 0) return;
    Py_INCREF(&ConsumerType);

    PyModule_AddObject(mod, "Consumer", (PyObject *)&ConsumerType);
}
