/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "internal.h"
#include "node_types.h"
#include "couch_btree.h"
#include "bitfield.h"
#include "reduces.h"
#include "util.h"

#define ROOT_BASE_SIZE 12
#define HEADER_BASE_SIZE 25

// Initializes one of the db's root node pointers from data in the file header
static couchstore_error_t read_db_root(Db *db, node_pointer **root,
                                       void *root_data, int root_size)
{
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    if (root_size > 0) {
        error_unless(root_size >= ROOT_BASE_SIZE, COUCHSTORE_ERROR_CORRUPT);
        *root = read_root(root_data, root_size);
        error_unless(*root, COUCHSTORE_ERROR_ALLOC_FAIL);
        error_unless((*root)->pointer < db->header.position, COUCHSTORE_ERROR_CORRUPT);
    } else {
        *root = NULL;
    }
cleanup:
    return errcode;
}

// Attempts to initialize the database from a header at the given file position
static couchstore_error_t find_header_at_pos(Db *db, cs_off_t pos)
{
    int seqrootsize;
    int idrootsize;
    int localrootsize;
    char *root_data;
    int header_len;
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    union {
        raw_file_header *raw;
        char *buf;
    } header_buf = { NULL };
    uint8_t buf[2];
    ssize_t readsize = db->file.ops->pread(&db->file.lastError, db->file.handle,
                                           buf, 2, pos);
    error_unless(readsize == 2, COUCHSTORE_ERROR_READ);
    if (buf[0] == 0) {
        return COUCHSTORE_ERROR_NO_HEADER;
    } else if (buf[0] != 1) {
        return COUCHSTORE_ERROR_CORRUPT;
    }

    header_len = pread_header(&db->file, pos, &header_buf.buf, MAX_DB_HEADER_SIZE);
    if (header_len < 0) {
        error_pass(static_cast<couchstore_error_t>(header_len));
    }

    db->header.position = pos;
    db->header.disk_version = decode_raw08(header_buf.raw->version);

    // Only 12 and 11 are valid
    error_unless(db->header.disk_version == COUCH_DISK_VERSION ||
                 db->header.disk_version == COUCH_DISK_VERSION_11,
                 COUCHSTORE_ERROR_HEADER_VERSION);
    db->header.update_seq = decode_raw48(header_buf.raw->update_seq);
    db->header.purge_seq = decode_raw48(header_buf.raw->purge_seq);
    db->header.purge_ptr = decode_raw48(header_buf.raw->purge_ptr);
    error_unless(db->header.purge_ptr <= db->header.position, COUCHSTORE_ERROR_CORRUPT);
    seqrootsize = decode_raw16(header_buf.raw->seqrootsize);
    idrootsize = decode_raw16(header_buf.raw->idrootsize);
    localrootsize = decode_raw16(header_buf.raw->localrootsize);
    error_unless(header_len == HEADER_BASE_SIZE + seqrootsize + idrootsize + localrootsize,
                 COUCHSTORE_ERROR_CORRUPT);

    root_data = (char*) (header_buf.raw + 1);  // i.e. just past *header_buf
    error_pass(read_db_root(db, &db->header.by_seq_root, root_data, seqrootsize));
    root_data += seqrootsize;
    error_pass(read_db_root(db, &db->header.by_id_root, root_data, idrootsize));
    root_data += idrootsize;
    error_pass(read_db_root(db, &db->header.local_docs_root, root_data, localrootsize));

cleanup:
    free(header_buf.raw);
    return errcode;
}

// Finds the database header by scanning back from the end of the file at 4k boundaries
static couchstore_error_t find_header(Db *db, int64_t start_pos)
{
    couchstore_error_t last_header_errcode = COUCHSTORE_ERROR_NO_HEADER;
    int64_t pos = start_pos;
    pos -= pos % COUCH_BLOCK_SIZE;
    for (; pos >= 0; pos -= COUCH_BLOCK_SIZE) {
        couchstore_error_t errcode = find_header_at_pos(db, pos);
        switch(errcode) {
            case COUCHSTORE_SUCCESS:
                // Found it!
                return COUCHSTORE_SUCCESS;
            case COUCHSTORE_ERROR_NO_HEADER:
                // No header here, so keep going
                break;
            case COUCHSTORE_ERROR_ALLOC_FAIL:
                // Fatal error
                return errcode;
            default:
                // Invalid header; continue, but remember the last error
                last_header_errcode = errcode;
                break;
        }
    }
    return last_header_errcode;
}

/**
 * Calculates how large in bytes the current header will be
 * when written to disk.
 *
 * The seqrootsize, idrootsize and localrootsize params are
 * used to return the respective sizes in this header if
 * needed.
 */
size_t calculate_header_size(Db *db, size_t& seqrootsize,
                             size_t& idrootsize, size_t& localrootsize)
{
    seqrootsize = idrootsize = localrootsize = 0;

    if (db->header.by_seq_root) {
        seqrootsize = ROOT_BASE_SIZE + db->header.by_seq_root->reduce_value.size;
    }
    if (db->header.by_id_root) {
        idrootsize = ROOT_BASE_SIZE + db->header.by_id_root->reduce_value.size;
    }
    if (db->header.local_docs_root) {
        localrootsize = ROOT_BASE_SIZE + db->header.local_docs_root->reduce_value.size;
    }
    return sizeof(raw_file_header) + seqrootsize + idrootsize + localrootsize;
}

couchstore_error_t db_write_header(Db *db)
{
    sized_buf writebuf;
    size_t seqrootsize, idrootsize, localrootsize;
    writebuf.size = calculate_header_size(db, seqrootsize,
                                          idrootsize, localrootsize);
    writebuf.buf = (char *) calloc(1, writebuf.size);
    raw_file_header* header = (raw_file_header*)writebuf.buf;
    header->version = encode_raw08(db->header.disk_version);
    encode_raw48(db->header.update_seq, &header->update_seq);
    encode_raw48(db->header.purge_seq, &header->purge_seq);
    encode_raw48(db->header.purge_ptr, &header->purge_ptr);
    header->seqrootsize = encode_raw16((uint16_t)seqrootsize);
    header->idrootsize = encode_raw16((uint16_t)idrootsize);
    header->localrootsize = encode_raw16((uint16_t)localrootsize);
    uint8_t *root = (uint8_t*)(header + 1);
    encode_root(root, db->header.by_seq_root);
    root += seqrootsize;
    encode_root(root, db->header.by_id_root);
    root += idrootsize;
    encode_root(root, db->header.local_docs_root);
    cs_off_t pos;
    couchstore_error_t errcode = write_header(&db->file, &writebuf, &pos);
    if (errcode == COUCHSTORE_SUCCESS) {
        db->header.position = pos;
    }
    free(writebuf.buf);
    return errcode;
}

static couchstore_error_t create_header(Db *db)
{
    // Select the version based upon selected CRC
    if (db->file.crc_mode == CRC32) {
        // user is creating down-level files
        db->header.disk_version = COUCH_DISK_VERSION_11;
    } else {
        // user is using latest
        db->header.disk_version = COUCH_DISK_VERSION;
    }
    db->header.update_seq = 0;
    db->header.by_id_root = NULL;
    db->header.by_seq_root = NULL;
    db->header.local_docs_root = NULL;
    db->header.purge_seq = 0;
    db->header.purge_ptr = 0;
    db->header.position = 0;
    return db_write_header(db);
}

LIBCOUCHSTORE_API
uint64_t couchstore_get_header_position(Db *db)
{
    return db->header.position;
}

/**
 * Precommit should occur before writing a header, it has two
 * purposes. Firstly it ensures data is written before we attempt
 * to write the header. This means it's impossible for the header
 * to be written before the data. This is accomplished through
 * a sync.
 *
 * The second purpose is to extend the file to be large enough
 * to include the subsequently written header. This is done so
 * the fdatasync performed by writing a header doesn't have to
 * do an additional (expensive) modified metadata flush on top
 * of the one we're already doing.
 */
couchstore_error_t precommit(Db *db)
{
    cs_off_t curpos = db->file.pos;

    db->file.pos = align_to_next_block(db->file.pos);
    sized_buf zerobyte = { const_cast<char*>("\0"), 1};

    size_t seqrootsize, idrootsize, localrootsize;
    db->file.pos += calculate_header_size(db, seqrootsize,
                                          idrootsize, localrootsize);

    //Extend file size to where end of header will land before we do first sync
    couchstore_error_t errcode = static_cast<couchstore_error_t>(
        db_write_buf(&db->file, &zerobyte, NULL, NULL));

    if (errcode == COUCHSTORE_SUCCESS) {
        errcode = db->file.ops->sync(&db->file.lastError, db->file.handle);
    }
    // Move cursor back to where it was
    db->file.pos = curpos;
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_commit(Db *db)
{
    couchstore_error_t errcode = precommit(db);

    if (errcode == COUCHSTORE_SUCCESS) {
        errcode = db_write_header(db);
    }

    if (errcode == COUCHSTORE_SUCCESS) {
        errcode = db->file.ops->sync(&db->file.lastError, db->file.handle);
    }

    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_db(const char *filename,
                                      couchstore_open_flags flags,
                                      Db **pDb)
{
    return couchstore_open_db_ex(filename, flags,
                                 couchstore_get_default_file_ops(), pDb);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_db_ex(const char *filename,
                                         couchstore_open_flags flags,
                                         FileOpsInterface* ops,
                                         Db **pDb)
{
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    Db *db;
    int openflags;
    bool buffered = true;
    cs_off_t pos;

    /* Sanity check input parameters */
    if ((flags & COUCHSTORE_OPEN_FLAG_RDONLY) &&
        (flags & COUCHSTORE_OPEN_FLAG_CREATE)) {
        return COUCHSTORE_ERROR_INVALID_ARGUMENTS;
    }

    if ((db = static_cast<Db*>(calloc(1, sizeof(Db)))) == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }

    if (flags & COUCHSTORE_OPEN_FLAG_RDONLY) {
        openflags = O_RDONLY;
    } else {
        openflags = O_RDWR;
    }

    if (flags & COUCHSTORE_OPEN_FLAG_CREATE) {
        openflags |= O_CREAT;
    }

    if (flags & COUCHSTORE_OPEN_FLAG_UNBUFFERED) {
        buffered = false;
    }

    // open with CRC unknown, CRC will be selected when header is read/or not found.
    error_pass(tree_file_open(&db->file, filename, openflags, CRC_UNKNOWN, ops,
                              buffered));

    pos = db->file.ops->goto_eof(&db->file.lastError, db->file.handle);
    db->file.pos = pos;
    if (pos == 0) {
        /* This is an empty file. Create a new fileheader unless the
         * user wanted a read-only version of the file
         */

        if (flags & COUCHSTORE_OPEN_FLAG_RDONLY) {
            error_pass(COUCHSTORE_ERROR_NO_HEADER);
        } else {

            // Select the CRC to use on this new file
            if (flags & COUCHSTORE_OPEN_WITH_LEGACY_CRC) {
                db->file.crc_mode = CRC32;
            } else {
                db->file.crc_mode = CRC32C;
            }

            error_pass(create_header(db));
        }
    } else if (pos > 0) {
        error_pass(find_header(db, db->file.pos - 2));

        if (db->header.disk_version <= COUCH_DISK_VERSION_11) {
            db->file.crc_mode = CRC32;
        } else {
            cb_assert(db->header.disk_version >= COUCH_DISK_VERSION_12);
            db->file.crc_mode = CRC32C;
        }

        // Not allowed. Can't request legacy_crc but be opening non legacy CRC files.
        if (db->file.crc_mode == CRC32C && flags & COUCHSTORE_OPEN_WITH_LEGACY_CRC) {
            errcode = COUCHSTORE_ERROR_INVALID_ARGUMENTS;
            goto cleanup;
        }
    } else {
        error_pass(static_cast<couchstore_error_t>(db->file.pos));
    }

    *pDb = db;
    db->dropped = 0;

cleanup:
    if(errcode != COUCHSTORE_SUCCESS) {
        couchstore_close_db(db);
    }

    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_drop_file(Db *db)
{
    if(db->dropped) {
        return COUCHSTORE_SUCCESS;
    }
    tree_file_close(&db->file);
    db->dropped = 1;
    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_rewind_db_header(Db *db)
{
    couchstore_error_t errcode;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    // free current header guts
    free(db->header.by_id_root);
    free(db->header.by_seq_root);
    free(db->header.local_docs_root);
    db->header.by_id_root = NULL;
    db->header.by_seq_root = NULL;
    db->header.local_docs_root = NULL;

    error_unless(db->header.position != 0, COUCHSTORE_ERROR_DB_NO_LONGER_VALID);
    // find older header
    error_pass(find_header(db, db->header.position - 2));

cleanup:
    // if we failed, free the handle and return an error
    if(errcode != COUCHSTORE_SUCCESS) {
        couchstore_close_db(db);
        errcode = COUCHSTORE_ERROR_DB_NO_LONGER_VALID;
    }
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_close_db(Db *db)
{
    if(!db) {
        return COUCHSTORE_SUCCESS;
    }

    if(!db->dropped) {
        tree_file_close(&db->file);
    }

    free(db->header.by_id_root);
    free(db->header.by_seq_root);
    free(db->header.local_docs_root);
    db->header.by_id_root = NULL;
    db->header.by_seq_root = NULL;
    db->header.local_docs_root = NULL;

    memset(db, 0xa5, sizeof(*db));
    free(db);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
const char* couchstore_get_db_filename(Db *db) {
    return db->file.path;
}

DocInfo* couchstore_alloc_docinfo(const sized_buf *id, const sized_buf *rev_meta) {
    size_t size = sizeof(DocInfo);
    if (id) {
        size += id->size;
    }
    if (rev_meta) {
        size += rev_meta->size;
    }
    DocInfo* docInfo = static_cast<DocInfo*>(malloc(size));
    if (!docInfo) {
        return NULL;
    }
    memset(docInfo, 0, sizeof(DocInfo));
    char *extra = (char *)docInfo + sizeof(DocInfo);
    if (id) {
        memcpy(extra, id->buf, id->size);
        docInfo->id.buf = extra;
        docInfo->id.size = id->size;
        extra += id->size;
    }
    if (rev_meta) {
        memcpy(extra, rev_meta->buf, rev_meta->size);
        docInfo->rev_meta.buf = extra;
        docInfo->rev_meta.size = rev_meta->size;
    }
    return docInfo;
}

LIBCOUCHSTORE_API
void couchstore_free_docinfo(DocInfo *docinfo)
{
    free(docinfo);
}

LIBCOUCHSTORE_API
void couchstore_free_document(Doc *doc)
{
    if (doc) {
        char *offset = (char *) (&((fatbuf *) NULL)->buf);
        fatbuf_free((fatbuf *) ((char *)doc - (char *)offset));
    }
}

couchstore_error_t by_seq_read_docinfo(DocInfo **pInfo,
                                       const sized_buf *k,
                                       const sized_buf *v)
{
    const raw_seq_index_value *raw = (const raw_seq_index_value*)v->buf;
    ssize_t extraSize = v->size - sizeof(*raw);
    if (extraSize < 0) {
        return COUCHSTORE_ERROR_CORRUPT;
    }

    uint32_t idsize, datasize;
    decode_kv_length(&raw->sizes, &idsize, &datasize);
    uint64_t bp = decode_raw48(raw->bp);
    int deleted = (bp & BP_DELETED_FLAG) != 0;
    bp &= ~BP_DELETED_FLAG;
    uint8_t content_meta = decode_raw08(raw->content_meta);
    uint64_t rev_seq = decode_raw48(raw->rev_seq);
    uint64_t db_seq = decode_sequence_key(k);

    sized_buf id = {v->buf + sizeof(*raw), idsize};
    sized_buf rev_meta = {id.buf + idsize, extraSize - id.size};
    DocInfo* docInfo = couchstore_alloc_docinfo(&id, &rev_meta);
    if (!docInfo) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }

    docInfo->db_seq = db_seq;
    docInfo->rev_seq = rev_seq;
    docInfo->deleted = deleted;
    docInfo->bp = bp;
    docInfo->size = datasize;
    docInfo->content_meta = content_meta;
    *pInfo = docInfo;
    return COUCHSTORE_SUCCESS;
}

static couchstore_error_t by_id_read_docinfo(DocInfo **pInfo,
                                             const sized_buf *k,
                                             const sized_buf *v)
{
    const raw_id_index_value *raw = (const raw_id_index_value*)v->buf;
    ssize_t revMetaSize = v->size - sizeof(*raw);
    if (revMetaSize < 0) {
        return COUCHSTORE_ERROR_CORRUPT;
    }

    uint32_t datasize, deleted;
    uint8_t content_meta;
    uint64_t bp, seq, revnum;

    seq = decode_raw48(raw->db_seq);
    datasize = decode_raw32(raw->size);
    bp = decode_raw48(raw->bp);
    deleted = (bp & BP_DELETED_FLAG) != 0;
    bp &= ~BP_DELETED_FLAG;
    content_meta = decode_raw08(raw->content_meta);
    revnum = decode_raw48(raw->rev_seq);

    sized_buf rev_meta = {v->buf + sizeof(*raw), static_cast<size_t>(revMetaSize)};
    DocInfo* docInfo = couchstore_alloc_docinfo(k, &rev_meta);
    if (!docInfo) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }

    docInfo->db_seq = seq;
    docInfo->rev_seq = revnum;
    docInfo->deleted = deleted;
    docInfo->bp = bp;
    docInfo->size = datasize;
    docInfo->content_meta = content_meta;
    *pInfo = docInfo;
    return COUCHSTORE_SUCCESS;
}

//Fill in doc from reading file.
static couchstore_error_t bp_to_doc(Doc **pDoc, Db *db, cs_off_t bp, couchstore_open_options options)
{
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    int bodylen = 0;
    char *docbody = NULL;
    fatbuf *docbuf = NULL;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);

    if (options & DECOMPRESS_DOC_BODIES) {
        bodylen = pread_compressed(&db->file, bp, &docbody);
    } else {
        bodylen = pread_bin(&db->file, bp, &docbody);
    }

    error_unless(bodylen >= 0, static_cast<couchstore_error_t>(bodylen));    // if bodylen is negative it's an error code
    error_unless(docbody || bodylen == 0, COUCHSTORE_ERROR_READ);

    error_unless(docbuf = fatbuf_alloc(sizeof(Doc) + bodylen), COUCHSTORE_ERROR_ALLOC_FAIL);
    *pDoc = (Doc *) fatbuf_get(docbuf, sizeof(Doc));

    if (bodylen == 0) { //Empty doc
        (*pDoc)->data.buf = NULL;
        (*pDoc)->data.size = 0;
        return COUCHSTORE_SUCCESS;
    }

    (*pDoc)->data.buf = (char *) fatbuf_get(docbuf, bodylen);
    (*pDoc)->data.size = bodylen;
    memcpy((*pDoc)->data.buf, docbody, bodylen);

cleanup:
    free(docbody);
    if (errcode < 0) {
        fatbuf_free(docbuf);
    }
    return errcode;
}

static couchstore_error_t docinfo_fetch_by_id(couchfile_lookup_request *rq,
                                              const sized_buf *k,
                                              const sized_buf *v)
{
    DocInfo **pInfo = (DocInfo **) rq->callback_ctx;
    if (v == NULL) {
        return COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }
    return by_id_read_docinfo(pInfo, k, v);
}

static couchstore_error_t docinfo_fetch_by_seq(couchfile_lookup_request *rq,
                                               const sized_buf *k,
                                               const sized_buf *v)
{
    DocInfo **pInfo = (DocInfo **) rq->callback_ctx;
    if (v == NULL) {
        return COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }
    return by_seq_read_docinfo(pInfo, k, v);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfo_by_id(Db *db,
                                            const void *id,
                                            size_t idlen,
                                            DocInfo **pInfo)
{
    sized_buf key;
    sized_buf *keylist = &key;
    couchfile_lookup_request rq;
    couchstore_error_t errcode;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);

    if (db->header.by_id_root == NULL) {
        return COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }

    key.buf = (char *) id;
    key.size = idlen;

    rq.cmp.compare = ebin_cmp;
    rq.file = &db->file;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = pInfo;
    rq.fetch_callback = docinfo_fetch_by_id;
    rq.node_callback = NULL;
    rq.fold = 0;

    errcode = btree_lookup(&rq, db->header.by_id_root->pointer);
    if (errcode == COUCHSTORE_SUCCESS) {
        if (*pInfo == NULL) {
            errcode = COUCHSTORE_ERROR_DOC_NOT_FOUND;
        }
    }
cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfo_by_sequence(Db *db,
                                                  uint64_t sequence,
                                                  DocInfo **pInfo)
{
    sized_buf key;
    sized_buf *keylist = &key;
    couchfile_lookup_request rq;
    couchstore_error_t errcode;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);

    if (db->header.by_id_root == NULL) {
        return COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }

    sequence = htonll(sequence);
    key.buf = (char *)&sequence + 2;
    key.size = 6;

    rq.cmp.compare = seq_cmp;
    rq.file = &db->file;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = pInfo;
    rq.fetch_callback = docinfo_fetch_by_seq;
    rq.node_callback = NULL;
    rq.fold = 0;

    errcode = btree_lookup(&rq, db->header.by_seq_root->pointer);
    if (errcode == COUCHSTORE_SUCCESS) {
        if (*pInfo == NULL) {
            errcode = COUCHSTORE_ERROR_DOC_NOT_FOUND;
        }
    }
cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_doc_with_docinfo(Db *db,
                                                    DocInfo *docinfo,
                                                    Doc **pDoc,
                                                    couchstore_open_options options)
{
    couchstore_error_t errcode;

    *pDoc = NULL;
    if (docinfo->bp == 0) {
        return COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }

    if (!(docinfo->content_meta & COUCH_DOC_IS_COMPRESSED)) {
        options &= ~DECOMPRESS_DOC_BODIES;
    }

    errcode = bp_to_doc(pDoc, db, docinfo->bp, options);
    if (errcode == COUCHSTORE_SUCCESS) {
        (*pDoc)->id.buf = docinfo->id.buf;
        (*pDoc)->id.size = docinfo->id.size;
    }

    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_document(Db *db,
                                            const void *id,
                                            size_t idlen,
                                            Doc **pDoc,
                                            couchstore_open_options options)
{
    couchstore_error_t errcode;
    DocInfo *info;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    *pDoc = NULL;
    errcode = couchstore_docinfo_by_id(db, id, idlen, &info);
    if (errcode == COUCHSTORE_SUCCESS) {
        errcode = couchstore_open_doc_with_docinfo(db, info, pDoc, options);
        if (errcode == COUCHSTORE_SUCCESS) {
            (*pDoc)->id.buf = (char *) id;
            (*pDoc)->id.size = idlen;
        }

        couchstore_free_docinfo(info);
    }
cleanup:
    return errcode;
}

// context info passed to lookup_callback via btree_lookup
typedef struct {
    Db *db;
    couchstore_docinfos_options options;
    couchstore_changes_callback_fn callback;
    void* callback_context;
    int by_id;
    int depth;
    couchstore_walk_tree_callback_fn walk_callback;
} lookup_context;

// btree_lookup callback, called while iterating keys
static couchstore_error_t lookup_callback(couchfile_lookup_request *rq,
                                          const sized_buf *k,
                                          const sized_buf *v)
{
    if (v == NULL) {
        return COUCHSTORE_SUCCESS;
    }

    const lookup_context *context = static_cast<const lookup_context *>(rq->callback_ctx);
    DocInfo *docinfo = NULL;
    couchstore_error_t errcode;
    if (context->by_id) {
        errcode = by_id_read_docinfo(&docinfo, k, v);
    } else {
        errcode = by_seq_read_docinfo(&docinfo, k, v);
    }
    if (errcode == COUCHSTORE_ERROR_CORRUPT && (context->options & COUCHSTORE_INCLUDE_CORRUPT_DOCS)) {
        // Invoke callback even if doc info is corrupted/unreadable, if magic flag is set
        docinfo = static_cast<DocInfo*>(calloc(sizeof(DocInfo), 1));
        docinfo->id = *k;
        docinfo->rev_meta = *v;
    } else if (errcode) {
        return errcode;
    }

    if ((context->options & COUCHSTORE_DELETES_ONLY) && docinfo->deleted == 0) {
        couchstore_free_docinfo(docinfo);
        return COUCHSTORE_SUCCESS;
    }

    if ((context->options & COUCHSTORE_NO_DELETES) && docinfo->deleted == 1) {
        couchstore_free_docinfo(docinfo);
        return COUCHSTORE_SUCCESS;
    }

    if (context->walk_callback) {
        errcode = static_cast<couchstore_error_t>(context->walk_callback(context->db,
                                                                         context->depth,
                                                                         docinfo,
                                                                         0,
                                                                         NULL,
                                                                         context->callback_context));
    } else {
        errcode = static_cast<couchstore_error_t>(context->callback(context->db,
                                                                    docinfo,
                                                                    context->callback_context));
    }
    if (errcode <= 0) {
        couchstore_free_docinfo(docinfo);
    } else {
        // User requested docinfo not be freed, don't free it, return success
        return COUCHSTORE_SUCCESS;
    }
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_changes_since(Db *db,
                                            uint64_t since,
                                            couchstore_docinfos_options options,
                                            couchstore_changes_callback_fn callback,
                                            void *ctx)
{
    char since_termbuf[6];
    sized_buf since_term;
    sized_buf *keylist = &since_term;
    lookup_context cbctx = {db, options, callback, ctx, 0, 0, NULL};
    couchfile_lookup_request rq;
    couchstore_error_t errcode;

    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    if (db->header.by_seq_root == NULL) {
        return COUCHSTORE_SUCCESS;
    }

    since_term.buf = since_termbuf;
    since_term.size = 6;
    encode_raw48(since, (raw_48*)since_term.buf);

    rq.cmp.compare = seq_cmp;
    rq.file = &db->file;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = &cbctx;
    rq.fetch_callback = lookup_callback;
    rq.node_callback = NULL;
    rq.fold = 1;

    errcode = btree_lookup(&rq, db->header.by_seq_root->pointer);
cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_all_docs(Db *db,
                                       const sized_buf* startKeyPtr,
                                       couchstore_docinfos_options options,
                                       couchstore_changes_callback_fn callback,
                                       void *ctx)
{
    sized_buf startKey = {NULL, 0};
    sized_buf *keylist = &startKey;
    lookup_context cbctx = {db, options, callback, ctx, 1, 0, NULL};
    couchfile_lookup_request rq;
    couchstore_error_t errcode;

    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    if (db->header.by_id_root == NULL) {
        return COUCHSTORE_SUCCESS;
    }

    if (startKeyPtr) {
        startKey = *startKeyPtr;
    }

    rq.cmp.compare = ebin_cmp;
    rq.file = &db->file;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = &cbctx;
    rq.fetch_callback = lookup_callback;
    rq.node_callback = NULL;
    rq.fold = 1;

    errcode = btree_lookup(&rq, db->header.by_id_root->pointer);
cleanup:
    return errcode;
}

static couchstore_error_t walk_node_callback(struct couchfile_lookup_request *rq,
                                                 uint64_t subtreeSize,
                                                 const sized_buf *reduceValue)
{
    lookup_context* context = static_cast<lookup_context*>(rq->callback_ctx);
    if (reduceValue) {
        int result = context->walk_callback(context->db,
                                            context->depth,
                                            NULL,
                                            subtreeSize,
                                            reduceValue,
                                            context->callback_context);
        context->depth++;
        if (result < 0)
            return static_cast<couchstore_error_t>(result);
    } else {
        context->depth--;
    }
    return COUCHSTORE_SUCCESS;
}

static
couchstore_error_t couchstore_walk_tree(Db *db,
                                        int by_id,
                                        const node_pointer* root,
                                        const sized_buf* startKeyPtr,
                                        couchstore_docinfos_options options,
                                        int (*compare)(const sized_buf *k1, const sized_buf *k2),
                                        couchstore_walk_tree_callback_fn callback,
                                        void *ctx)
{
    couchstore_error_t errcode;
    sized_buf startKey = {NULL, 0};
    sized_buf *keylist;
    couchfile_lookup_request rq;

    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    if (root == NULL) {
        return COUCHSTORE_SUCCESS;
    }

    // Invoke the callback on the root node:
    errcode = static_cast<couchstore_error_t>(callback(db, 0, NULL,
                                                       root->subtreesize,
                                                       &root->reduce_value,
                                                       ctx));
    if (errcode < 0) {
        return errcode;
    }

    if (startKeyPtr) {
        startKey = *startKeyPtr;
    }
    keylist = &startKey;

    {
        // Create a new scope here just to mute the warning from the
        // compiler that the goto in the macro error_unless
        // skips the initialization of lookup_ctx..
        lookup_context lookup_ctx = {db, options, NULL, ctx, by_id, 1, callback};

        rq.cmp.compare = compare;
        rq.file = &db->file;
        rq.num_keys = 1;
        rq.keys = &keylist;
        rq.callback_ctx = &lookup_ctx;
        rq.fetch_callback = lookup_callback;
        rq.node_callback = walk_node_callback;
        rq.fold = 1;

        error_pass(btree_lookup(&rq, root->pointer));
    }
cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_walk_id_tree(Db *db,
                                           const sized_buf* startDocID,
                                           couchstore_docinfos_options options,
                                           couchstore_walk_tree_callback_fn callback,
                                           void *ctx)
{
    return couchstore_walk_tree(db, 1, db->header.by_id_root, startDocID,
                                options, ebin_cmp, callback, ctx);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_walk_seq_tree(Db *db,
                                           uint64_t startSequence,
                                           couchstore_docinfos_options options,
                                           couchstore_walk_tree_callback_fn callback,
                                           void *ctx)
{
    raw_48 start_termbuf;
    encode_raw48(startSequence, &start_termbuf);
    sized_buf start_term = {(char*)&start_termbuf, 6};

    return couchstore_walk_tree(db, 0, db->header.by_seq_root, &start_term,
                                options, seq_cmp, callback, ctx);
}

static int id_ptr_cmp(const void *a, const void *b)
{
    sized_buf **buf1 = (sized_buf**) a;
    sized_buf **buf2 = (sized_buf**) b;
    return ebin_cmp(*buf1, *buf2);
}

static int seq_ptr_cmp(const void *a, const void *b)
{
    sized_buf **buf1 = (sized_buf**) a;
    sized_buf **buf2 = (sized_buf**) b;
    return seq_cmp(*buf1, *buf2);
}

// Common subroutine of couchstore_docinfos_by_{ids, sequence}
static couchstore_error_t iterate_docinfos(Db *db,
                                           const sized_buf keys[],
                                           unsigned numDocs,
                                           node_pointer *tree,
                                           int (*key_ptr_compare)(const void *, const void *),
                                           int (*key_compare)(const sized_buf *k1, const sized_buf *k2),
                                           couchstore_changes_callback_fn callback,
                                           int fold,
                                           void *ctx)
{
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    const sized_buf **keyptrs = NULL;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    // Nothing to do if the tree is empty
    if (tree == NULL) {
        return COUCHSTORE_SUCCESS;
    }

    if(numDocs <= 0) {
        return COUCHSTORE_ERROR_INVALID_ARGUMENTS;
    }

    // Create an array of *pointers to* sized_bufs, which is what btree_lookup wants:
    keyptrs = static_cast<const sized_buf**>(malloc(numDocs * sizeof(sized_buf*)));
    error_unless(keyptrs, COUCHSTORE_ERROR_ALLOC_FAIL);

    {
        unsigned i;
        for (i = 0; i< numDocs; ++i) {
            keyptrs[i] = &keys[i];
        }
        if (!fold) {
            // Sort the key pointers:
            qsort(keyptrs, numDocs, sizeof(keyptrs[0]), key_ptr_compare);
        }

        // Construct the lookup request:
        lookup_context cbctx = {db, 0, callback, ctx, (tree == db->header.by_id_root), 0, NULL};
        couchfile_lookup_request rq;
        rq.cmp.compare = key_compare;
        rq.file = &db->file;
        rq.num_keys = numDocs;
        rq.keys = (sized_buf**) keyptrs;
        rq.callback_ctx = &cbctx;
        rq.fetch_callback = lookup_callback;
        rq.node_callback = NULL;
        rq.fold = fold;

        // Go!
        error_pass(btree_lookup(&rq, tree->pointer));
    }
cleanup:
    free(keyptrs);
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfos_by_id(Db *db,
                                             const sized_buf ids[],
                                             unsigned numDocs,
                                             couchstore_docinfos_options options,
                                             couchstore_changes_callback_fn callback,
                                             void *ctx)
{
    return iterate_docinfos(db, ids, numDocs,
                            db->header.by_id_root, id_ptr_cmp, ebin_cmp,
                            callback,
                            (options & RANGES) != 0,
                            ctx);
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_docinfos_by_sequence(Db *db,
                                                   const uint64_t sequence[],
                                                   unsigned numDocs,
                                                   couchstore_docinfos_options options,
                                                   couchstore_changes_callback_fn callback,
                                                   void *ctx)
{
    // Create the array of keys:
    sized_buf *keylist = static_cast<sized_buf*>(malloc(numDocs * sizeof(sized_buf)));
    raw_by_seq_key *keyvalues = static_cast<raw_by_seq_key*>(malloc(numDocs * sizeof(raw_by_seq_key)));
    couchstore_error_t errcode;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    error_unless(keylist && keyvalues, COUCHSTORE_ERROR_ALLOC_FAIL);
    unsigned i;
    for (i = 0; i< numDocs; ++i) {
        encode_raw48(sequence[i], &keyvalues[i].sequence);
        keylist[i].buf = static_cast<char*>((void*) &keyvalues[i]);
        keylist[i].size = sizeof(keyvalues[i]);
    }

    error_pass(iterate_docinfos(db, keylist, numDocs,
                                db->header.by_seq_root, seq_ptr_cmp, seq_cmp,
                                callback,
                                (options & RANGES) != 0,
                                ctx));
cleanup:
    free(keylist);
    free(keyvalues);
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_db_info(Db *db, DbInfo* dbinfo) {
    if (db == NULL || dbinfo == NULL) {
        return COUCHSTORE_ERROR_INVALID_ARGUMENTS;
    }
    const node_pointer *id_root = db->header.by_id_root;
    const node_pointer *seq_root = db->header.by_seq_root;
    const node_pointer *local_root = db->header.local_docs_root;
    dbinfo->filename = db->file.path;
    dbinfo->header_position = db->header.position;
    dbinfo->last_sequence = db->header.update_seq;
    dbinfo->purge_seq = db->header.purge_seq;
    dbinfo->deleted_count = dbinfo->doc_count = dbinfo->space_used = 0;
    dbinfo->file_size = db->file.pos;
    if (id_root) {
        raw_by_id_reduce* id_reduce = (raw_by_id_reduce*) id_root->reduce_value.buf;
        dbinfo->doc_count = decode_raw40(id_reduce->notdeleted);
        dbinfo->deleted_count = decode_raw40(id_reduce->deleted);
        dbinfo->space_used = decode_raw48(id_reduce->size);
        dbinfo->space_used += id_root->subtreesize;
    }
    if(seq_root) {
        dbinfo->space_used += seq_root->subtreesize;
    }
    if(local_root) {
        dbinfo->space_used += local_root->subtreesize;
    }
    return COUCHSTORE_SUCCESS;
}

static couchstore_error_t local_doc_fetch(couchfile_lookup_request *rq,
                                          const sized_buf *k,
                                          const sized_buf *v)
{
    LocalDoc **lDoc = (LocalDoc **) rq->callback_ctx;
    LocalDoc *dp;

    if (!v) {
        *lDoc = NULL;
        return COUCHSTORE_SUCCESS;
    }
    fatbuf *ldbuf = fatbuf_alloc(sizeof(LocalDoc) + k->size + v->size);
    if (ldbuf == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }

    dp = *lDoc = (LocalDoc *) fatbuf_get(ldbuf, sizeof(LocalDoc));
    dp->id.buf = (char *) fatbuf_get(ldbuf, k->size);
    dp->id.size = k->size;

    dp->json.buf = (char *) fatbuf_get(ldbuf, v->size);
    dp->json.size = v->size;

    dp->deleted = 0;

    memcpy(dp->id.buf, k->buf, k->size);
    memcpy(dp->json.buf, v->buf, v->size);

    return COUCHSTORE_SUCCESS;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_open_local_document(Db *db,
                                                  const void *id,
                                                  size_t idlen,
                                                  LocalDoc **pDoc)
{
    sized_buf key;
    sized_buf *keylist = &key;
    couchfile_lookup_request rq;
    couchstore_error_t errcode;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);
    if (db->header.local_docs_root == NULL) {
        return COUCHSTORE_ERROR_DOC_NOT_FOUND;
    }

    key.buf = (char *) id;
    key.size = idlen;

    rq.cmp.compare = ebin_cmp;
    rq.file = &db->file;
    rq.num_keys = 1;
    rq.keys = &keylist;
    rq.callback_ctx = pDoc;
    rq.fetch_callback = local_doc_fetch;
    rq.node_callback = NULL;
    rq.fold = 0;

    errcode = btree_lookup(&rq, db->header.local_docs_root->pointer);
    if (errcode == COUCHSTORE_SUCCESS) {
        if (*pDoc == NULL) {
            errcode = COUCHSTORE_ERROR_DOC_NOT_FOUND;
        }
    }
cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_local_document(Db *db, LocalDoc *lDoc)
{
    couchstore_error_t errcode;
    couchfile_modify_action ldupdate;
    node_pointer *nroot = NULL;
    error_unless(!db->dropped, COUCHSTORE_ERROR_FILE_CLOSED);

    if (lDoc->deleted) {
        ldupdate.type = ACTION_REMOVE;
    } else {
        ldupdate.type = ACTION_INSERT;
    }

    ldupdate.key = &lDoc->id;
    ldupdate.value.data = &lDoc->json;

    couchfile_modify_request rq;
    rq.cmp.compare = ebin_cmp;
    rq.num_actions = 1;
    rq.actions = &ldupdate;
    rq.fetch_callback = NULL;
    rq.reduce = NULL;
    rq.rereduce = NULL;
    rq.file = &db->file;
    rq.enable_purging = false;
    rq.purge_kp = NULL;
    rq.purge_kv = NULL;
    rq.compacting = 0;
    rq.kv_chunk_threshold = DB_CHUNK_THRESHOLD;
    rq.kp_chunk_threshold = DB_CHUNK_THRESHOLD;

    nroot = modify_btree(&rq, db->header.local_docs_root, &errcode);
    if (errcode == COUCHSTORE_SUCCESS && nroot != db->header.local_docs_root) {
        free(db->header.local_docs_root);
        db->header.local_docs_root = nroot;
    }

cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
void couchstore_free_local_document(LocalDoc *lDoc)
{
    if (lDoc) {
        char *offset = (char *) (&((fatbuf *) NULL)->buf);
        fatbuf_free((fatbuf *) ((char *)lDoc - (char *)offset));
    }
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_last_os_error(const Db *db,
                                            char* buf,
                                            size_t size) {
    if (db == NULL) {
        return COUCHSTORE_ERROR_INVALID_ARGUMENTS;
    }
    const couchstore_error_info_t *err = &db->file.lastError;

#ifdef WIN32
    char* win_msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                   FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err->error,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPTSTR) &win_msg,
                   0, NULL);
    _snprintf(buf, size, "WINAPI error = %d: '%s'", err->error, win_msg);
    LocalFree(win_msg);
#else
    snprintf(buf, size, "errno = %d: '%s'", err->error, strerror(err->error));
#endif

    return COUCHSTORE_SUCCESS;
}

static couchstore_error_t btree_eval_seq_reduce(Db *db,
                                                uint64_t *accum,
                                                sized_buf *left,
                                                sized_buf *right,
                                                bool past_left_edge,
                                                uint64_t diskpos) {
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    int bufpos = 1, nodebuflen = 0;
    int node_type;
    char *nodebuf = NULL;
    nodebuflen = pread_compressed(&db->file, diskpos, &nodebuf);
    error_unless(nodebuflen >= 0, (static_cast<couchstore_error_t>(nodebuflen)));  // if negative, it's an error code

    node_type = nodebuf[0];
    while(bufpos < nodebuflen) {
        sized_buf k, v;
        bufpos += read_kv(nodebuf + bufpos, &k, &v);
        int left_cmp = seq_cmp(&k, left);
        int right_cmp = seq_cmp(&k, right);
        if(left_cmp < 0) {
            continue;
        }
        if(node_type == KP_NODE) {
            // In-range Item in a KP Node
            const raw_node_pointer *raw = (const raw_node_pointer*)v.buf;
            const raw_by_seq_reduce *rawreduce = (const raw_by_seq_reduce*) (v.buf + sizeof(raw_node_pointer));
            uint64_t subcount = decode_raw40(rawreduce->count);
            uint64_t pointer = decode_raw48(raw->pointer);
            if((left_cmp >= 0 && !past_left_edge) || right_cmp >= 0) {
                error_pass(btree_eval_seq_reduce(db, accum, left, right, past_left_edge, pointer));
                if(right_cmp >= 0) {
                    break;
                } else {
                    past_left_edge = true;
                }
            } else {
                *accum += subcount;
            }
        } else {
            if(right_cmp > 0) {
                break;
            }
            // In-range Item in a KV Node
            *accum += 1;
        }
    }
cleanup:
    if (nodebuf) {
        free(nodebuf);
    }
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_changes_count(Db* db,
                                            uint64_t min_seq,
                                            uint64_t max_seq,
                                            uint64_t *count) {
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    raw_48 leftkr, rightkr;
    sized_buf leftk, rightk;
    leftk.buf = (char*) &leftkr;
    rightk.buf = (char*) &rightkr;
    leftk.size = 6;
    rightk.size = 6;
    encode_raw48(min_seq, &leftkr);
    encode_raw48(max_seq, &rightkr);

    *count = 0;
    if(db->header.by_seq_root) {
        error_pass(btree_eval_seq_reduce(db, count, &leftk, &rightk, false,
                                         db->header.by_seq_root->pointer));
    }
cleanup:
    return errcode;
}
