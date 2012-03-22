/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <libcouchstore/couch_db.h>
#include <snappy-c.h>

#define SNAPPY_FLAG 128

static void printsb(sized_buf *sb)
{
    if (sb->buf == NULL) {
        printf("null\n");
        return;
    }
    printf("%.*s\n", (int) sb->size, sb->buf);
}

static int foldprint(Db *db, DocInfo *docinfo, void *ctx)
{
    int *count = (int *) ctx;
    Doc *doc;
    uint64_t cas;
    uint32_t expiry, flags;
    cas = htonll(*((uint64_t *) docinfo->rev_meta.buf));
    expiry = htonl(*((uint32_t *) (docinfo->rev_meta.buf + 8)));
    flags = htonl(*((uint32_t *) (docinfo->rev_meta.buf + 12)));
    open_doc_with_docinfo(db, docinfo, &doc, 0);
    printf("Doc seq: %"PRIu64"\n", docinfo->db_seq);
    printf("     id: ");
    printsb(&docinfo->id);
    printf("     content_meta: %d\n", docinfo->content_meta);
    printf("     cas: %"PRIu64", expiry: %"PRIu64", flags: %"PRIu64"\n", (long long unsigned int) cas, (long long unsigned int) expiry, (long long unsigned int) flags);
    if (docinfo->deleted) {
        printf("     doc deleted\n");
    }

    if (docinfo->content_meta & SNAPPY_FLAG) {
        size_t rlen;
        snappy_uncompressed_length(doc->data.buf, doc->data.size, &rlen);
        char *decbuf = (char *) malloc(rlen);
        size_t uncompr_len;
        snappy_uncompress(doc->data.buf, doc->data.size, decbuf, &uncompr_len);
        printf("     data: (snappy) %.*s\n", (int) uncompr_len, decbuf);
    } else {
        printf("     data: ");
        printsb(&doc->data);
    }

    free_doc(doc);
    (*count)++;
    return 0;
}

static int process_file(const char *file, int *total)
{
    Db *db = NULL;
    couchstore_error_t errcode = couchstore_open_db(file, 0, &db);
    if (errcode != COUCHSTORE_SUCCESS) {
        fprintf(stderr, "Failed to open \"%s\": %s\n",
                file, couchstore_strerror(errcode));
        return -1;
    }

    int count = 0;
    errcode = changes_since(db, 0, 0, foldprint, &count);
    (void)couchstore_close_db(db);

    if (errcode < 0) {
        fprintf(stderr, "Failed to dump database \"%s\": %s\n",
                file, couchstore_strerror(errcode));
        return -1;
    }

    *total += count;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("USAGE: %s <file.couch>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int error = 0;
    int count;
    for (int ii = 1; ii < argc; ++ii) {
        error += process_file(argv[ii], &count);
    }

    printf("\nTotal docs: %d\n", count);
    if (error) {
        exit(EXIT_FAILURE);
    } else {
        exit(EXIT_SUCCESS);
    }
}

