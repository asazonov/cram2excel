#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "hfile_internal.h"

#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT ENOSYS
#endif

#define CRAM2EXCEL_SCHEME "cram2excel:"
#define DEFAULT_BUFFER_SIZE (128 * 1024)
#define MAX_WORKSHEET_ROWS 1048576U
#define MAX_CELL_TEXT 32767U

typedef struct {
    char *name;
    char *temp_path;
    FILE *stream;
    uint32_t row_count;
    bool finished;
} worksheet_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

typedef struct {
    hFILE base;
    char *output_path;
    worksheet_t header_sheet;
    worksheet_t *align_sheets;
    size_t align_count;
    size_t align_cap;
    char *pending;
    size_t pending_len;
    size_t pending_cap;
    int failure_errno;
} cram2excel_hfile_t;

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} zip_mem_entry_t;

typedef struct {
    char *name;
    char *path;
    uint32_t size;
    uint32_t crc;
    uint32_t offset;
    uint16_t mod_time;
    uint16_t mod_date;
} zip_file_entry_t;

static const struct hFILE_backend cram2excel_backend;

static void cram2excel_log(const char *message)
{
    fprintf(stderr, "[cram2excel] %s\n", message);
}

static void write_le16(FILE *fp, uint16_t value)
{
    fputc(value & 0xff, fp);
    fputc((value >> 8) & 0xff, fp);
}

static void write_le32(FILE *fp, uint32_t value)
{
    write_le16(fp, (uint16_t) (value & 0xffff));
    write_le16(fp, (uint16_t) ((value >> 16) & 0xffff));
}

static void zip_now(uint16_t *dos_time, uint16_t *dos_date)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    *dos_time = (uint16_t) (((tm_now.tm_hour & 0x1f) << 11)
                | ((tm_now.tm_min & 0x3f) << 5)
                | ((tm_now.tm_sec / 2) & 0x1f));
    *dos_date = (uint16_t) ((((tm_now.tm_year - 80) & 0x7f) << 9)
                | (((tm_now.tm_mon + 1) & 0x0f) << 5)
                | (tm_now.tm_mday & 0x1f));
}

static int strbuf_reserve(strbuf_t *buf, size_t extra)
{
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return 0;
    }

    size_t new_cap = buf->cap ? buf->cap : 256;
    while (new_cap < need) {
        if (new_cap > (SIZE_MAX / 2)) {
            errno = ENOMEM;
            return -1;
        }
        new_cap *= 2;
    }

    char *next = realloc(buf->data, new_cap);
    if (!next) {
        return -1;
    }

    buf->data = next;
    buf->cap = new_cap;
    return 0;
}

static int strbuf_append_len(strbuf_t *buf, const char *text, size_t text_len)
{
    if (strbuf_reserve(buf, text_len) != 0) {
        return -1;
    }

    memcpy(buf->data + buf->len, text, text_len);
    buf->len += text_len;
    buf->data[buf->len] = '\0';
    return 0;
}

static int strbuf_append(strbuf_t *buf, const char *text)
{
    return strbuf_append_len(buf, text, strlen(text));
}

static int strbuf_appendf(strbuf_t *buf, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int need;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (need < 0) {
        va_end(ap2);
        errno = EIO;
        return -1;
    }

    if (strbuf_reserve(buf, (size_t) need) != 0) {
        va_end(ap2);
        return -1;
    }

    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap2);
    va_end(ap2);
    buf->len += (size_t) need;
    return 0;
}

static char *dup_string_len(const char *text, size_t len)
{
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static char *dup_string(const char *text)
{
    return dup_string_len(text, strlen(text));
}

static char *alloc_printf(const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int needed;
    char *buffer;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) {
        va_end(ap2);
        errno = EIO;
        return NULL;
    }

    buffer = malloc((size_t) needed + 1);
    if (!buffer) {
        va_end(ap2);
        return NULL;
    }

    vsnprintf(buffer, (size_t) needed + 1, fmt, ap2);
    va_end(ap2);
    return buffer;
}

static void strbuf_free(strbuf_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int append_pending(cram2excel_hfile_t *fp, const char *data, size_t len)
{
    size_t need = fp->pending_len + len + 1;
    if (need > fp->pending_cap) {
        size_t new_cap = fp->pending_cap ? fp->pending_cap : 4096;
        while (new_cap < need) {
            if (new_cap > (SIZE_MAX / 2)) {
                errno = ENOMEM;
                return -1;
            }
            new_cap *= 2;
        }

        char *next = realloc(fp->pending, new_cap);
        if (!next) {
            return -1;
        }

        fp->pending = next;
        fp->pending_cap = new_cap;
    }

    memcpy(fp->pending + fp->pending_len, data, len);
    fp->pending_len += len;
    fp->pending[fp->pending_len] = '\0';
    return 0;
}

static int worksheet_open(worksheet_t *sheet, const char *sheet_name)
{
    char pattern[] = "/tmp/cram2excel_sheetXXXXXX";
    int fd = mkstemp(pattern);
    if (fd < 0) {
        return -1;
    }

    FILE *stream = fdopen(fd, "w");
    if (!stream) {
        int save_errno = errno;
        close(fd);
        unlink(pattern);
        errno = save_errno;
        return -1;
    }

    sheet->name = dup_string(sheet_name);
    sheet->temp_path = dup_string(pattern);
    if (!sheet->name || !sheet->temp_path) {
        int save_errno = errno;
        fclose(stream);
        unlink(pattern);
        free(sheet->name);
        free(sheet->temp_path);
        sheet->name = NULL;
        sheet->temp_path = NULL;
        errno = save_errno;
        return -1;
    }

    sheet->stream = stream;
    sheet->row_count = 0;
    sheet->finished = false;

    if (fprintf(stream,
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                "<sheetData>") < 0) {
        return -1;
    }

    return 0;
}

static void worksheet_discard(worksheet_t *sheet)
{
    if (sheet->stream) {
        fclose(sheet->stream);
    }
    if (sheet->temp_path) {
        unlink(sheet->temp_path);
    }
    free(sheet->name);
    free(sheet->temp_path);

    sheet->name = NULL;
    sheet->temp_path = NULL;
    sheet->stream = NULL;
    sheet->row_count = 0;
    sheet->finished = false;
}

static int worksheet_finish(worksheet_t *sheet)
{
    if (!sheet->stream || sheet->finished) {
        return 0;
    }

    if (fputs("</sheetData></worksheet>", sheet->stream) == EOF) {
        return -1;
    }
    if (fclose(sheet->stream) != 0) {
        sheet->stream = NULL;
        return -1;
    }

    sheet->stream = NULL;
    sheet->finished = true;
    return 0;
}

static void excel_column_name(int column, char *buffer, size_t size)
{
    char reversed[8];
    size_t used = 0;

    while (column > 0 && used + 1 < sizeof(reversed)) {
        column--;
        reversed[used++] = (char) ('A' + (column % 26));
        column /= 26;
    }

    size_t out = 0;
    while (used > 0 && out + 1 < size) {
        buffer[out++] = reversed[--used];
    }
    buffer[out] = '\0';
}

static int xml_write_escaped(FILE *stream, const char *text)
{
    size_t written_chars = 0;
    size_t index = 0;
    size_t len = strlen(text);
    bool truncated = false;

    if (len > MAX_CELL_TEXT) {
        len = MAX_CELL_TEXT - 3;
        truncated = true;
    }

    while (index < len) {
        unsigned char ch = (unsigned char) text[index++];

        if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
            continue;
        }

        if (written_chars >= MAX_CELL_TEXT) {
            break;
        }

        switch (ch) {
        case '&':
            if (fputs("&amp;", stream) == EOF) return -1;
            break;
        case '<':
            if (fputs("&lt;", stream) == EOF) return -1;
            break;
        case '>':
            if (fputs("&gt;", stream) == EOF) return -1;
            break;
        default:
            if (fputc(ch, stream) == EOF) return -1;
            break;
        }

        written_chars++;
    }

    if (truncated && fputs("...", stream) == EOF) {
        return -1;
    }

    return 0;
}

static int worksheet_begin_row(worksheet_t *sheet)
{
    sheet->row_count++;
    if (fprintf(sheet->stream, "<row r=\"%" PRIu32 "\">", sheet->row_count) < 0) {
        return -1;
    }
    return 0;
}

static int worksheet_end_row(worksheet_t *sheet)
{
    return fputs("</row>", sheet->stream) == EOF ? -1 : 0;
}

static int worksheet_write_inline_cell(worksheet_t *sheet, int column, const char *value)
{
    char col_name[8];

    excel_column_name(column, col_name, sizeof(col_name));

    if (fprintf(sheet->stream,
                "<c r=\"%s%" PRIu32 "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">",
                col_name, sheet->row_count) < 0) {
        return -1;
    }
    if (xml_write_escaped(sheet->stream, value ? value : "") != 0) {
        return -1;
    }
    if (fputs("</t></is></c>", sheet->stream) == EOF) {
        return -1;
    }

    return 0;
}

static int worksheet_write_number_cell(worksheet_t *sheet, int column, long long value)
{
    char col_name[8];

    excel_column_name(column, col_name, sizeof(col_name));

    if (fprintf(sheet->stream, "<c r=\"%s%" PRIu32 "\"><v>%lld</v></c>",
                col_name, sheet->row_count, value) < 0) {
        return -1;
    }

    return 0;
}

static int worksheet_write_text_row(worksheet_t *sheet, const char *text)
{
    if (worksheet_begin_row(sheet) != 0) {
        return -1;
    }
    if (worksheet_write_inline_cell(sheet, 1, text) != 0) {
        return -1;
    }
    return worksheet_end_row(sheet);
}

static int write_alignment_header_row(worksheet_t *sheet)
{
    static const char *columns[] = {
        "QNAME", "FLAG", "RNAME", "POS", "MAPQ", "CIGAR",
        "RNEXT", "PNEXT", "TLEN", "SEQ", "QUAL", "TAGS"
    };
    size_t i;

    if (worksheet_begin_row(sheet) != 0) {
        return -1;
    }

    for (i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        if (worksheet_write_inline_cell(sheet, (int) i + 1, columns[i]) != 0) {
            return -1;
        }
    }

    return worksheet_end_row(sheet);
}

static int add_alignment_sheet(cram2excel_hfile_t *fp)
{
    if (fp->align_count == fp->align_cap) {
        size_t new_cap = fp->align_cap ? fp->align_cap * 2 : 4;
        worksheet_t *next = realloc(fp->align_sheets, new_cap * sizeof(*next));
        if (!next) {
            return -1;
        }
        memset(next + fp->align_cap, 0, (new_cap - fp->align_cap) * sizeof(*next));
        fp->align_sheets = next;
        fp->align_cap = new_cap;
    }

    worksheet_t *sheet = &fp->align_sheets[fp->align_count];
    char name[32];
    snprintf(name, sizeof(name), "Alignments%zu", fp->align_count + 1);

    if (worksheet_open(sheet, name) != 0) {
        return -1;
    }
    if (write_alignment_header_row(sheet) != 0) {
        return -1;
    }

    fp->align_count++;
    return 0;
}

static worksheet_t *current_alignment_sheet(cram2excel_hfile_t *fp)
{
    if (fp->align_count == 0) {
        if (add_alignment_sheet(fp) != 0) {
            return NULL;
        }
    }

    worksheet_t *sheet = &fp->align_sheets[fp->align_count - 1];
    if (sheet->row_count >= MAX_WORKSHEET_ROWS) {
        if (add_alignment_sheet(fp) != 0) {
            return NULL;
        }
        sheet = &fp->align_sheets[fp->align_count - 1];
    }

    return sheet;
}

static int parse_integer(const char *text, long long *value)
{
    char *end = NULL;
    long long parsed;

    if (!text || !*text || strcmp(text, "*") == 0) {
        return 0;
    }

    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return 0;
    }

    *value = parsed;
    return 1;
}

static char *join_optional_tags(char **fields, size_t count)
{
    if (count == 0) {
        char *empty = dup_string("");
        return empty;
    }

    size_t total = 1;
    size_t i;
    for (i = 0; i < count; i++) {
        total += strlen(fields[i]);
        if (i + 1 < count) {
            total += 3;
        }
    }

    char *joined = malloc(total);
    if (!joined) {
        return NULL;
    }

    joined[0] = '\0';
    for (i = 0; i < count; i++) {
        strcat(joined, fields[i]);
        if (i + 1 < count) {
            strcat(joined, " | ");
        }
    }

    return joined;
}

static int write_maybe_numeric_cell(worksheet_t *sheet, int column, const char *text)
{
    long long value;
    if (parse_integer(text, &value)) {
        return worksheet_write_number_cell(sheet, column, value);
    }
    return worksheet_write_inline_cell(sheet, column, text);
}

static int process_sam_line(cram2excel_hfile_t *fp, char *line)
{
    char *fields[512];
    size_t field_count = 0;
    char *cursor = line;
    worksheet_t *sheet;
    char *tags;

    if (!line || line[0] == '\0') {
        return 0;
    }

    if (line[0] == '@') {
        return worksheet_write_text_row(&fp->header_sheet, line);
    }

    fields[field_count++] = cursor;
    while (*cursor) {
        if (*cursor == '\t') {
            *cursor = '\0';
            if (field_count >= sizeof(fields) / sizeof(fields[0])) {
                errno = EOVERFLOW;
                return -1;
            }
            fields[field_count++] = cursor + 1;
        }
        cursor++;
    }

    if (field_count < 11) {
        errno = EPROTO;
        cram2excel_log("Expected SAM text from samtools view; the stream looked non-SAM or incomplete.");
        return -1;
    }

    sheet = current_alignment_sheet(fp);
    if (!sheet) {
        return -1;
    }

    tags = join_optional_tags(field_count > 11 ? &fields[11] : NULL,
                              field_count > 11 ? field_count - 11 : 0);
    if (!tags) {
        return -1;
    }

    if (worksheet_begin_row(sheet) != 0
        || worksheet_write_inline_cell(sheet, 1, fields[0]) != 0
        || write_maybe_numeric_cell(sheet, 2, fields[1]) != 0
        || worksheet_write_inline_cell(sheet, 3, fields[2]) != 0
        || write_maybe_numeric_cell(sheet, 4, fields[3]) != 0
        || write_maybe_numeric_cell(sheet, 5, fields[4]) != 0
        || worksheet_write_inline_cell(sheet, 6, fields[5]) != 0
        || worksheet_write_inline_cell(sheet, 7, fields[6]) != 0
        || write_maybe_numeric_cell(sheet, 8, fields[7]) != 0
        || write_maybe_numeric_cell(sheet, 9, fields[8]) != 0
        || worksheet_write_inline_cell(sheet, 10, fields[9]) != 0
        || worksheet_write_inline_cell(sheet, 11, fields[10]) != 0
        || worksheet_write_inline_cell(sheet, 12, tags) != 0
        || worksheet_end_row(sheet) != 0) {
        int save_errno = errno;
        free(tags);
        errno = save_errno;
        return -1;
    }

    free(tags);
    return 0;
}

static int flush_pending_line(cram2excel_hfile_t *fp)
{
    if (fp->pending_len == 0) {
        return 0;
    }

    if (fp->pending[fp->pending_len - 1] == '\r') {
        fp->pending[--fp->pending_len] = '\0';
    }

    if (process_sam_line(fp, fp->pending) != 0) {
        return -1;
    }

    fp->pending_len = 0;
    fp->pending[0] = '\0';
    return 0;
}

static int process_chunk(cram2excel_hfile_t *fp, const char *data, size_t nbytes)
{
    const char *cursor = data;
    const char *end = data + nbytes;

    while (cursor < end) {
        const void *nl_ptr = memchr(cursor, '\n', (size_t) (end - cursor));
        if (!nl_ptr) {
            return append_pending(fp, cursor, (size_t) (end - cursor));
        }

        const char *nl = (const char *) nl_ptr;
        if (append_pending(fp, cursor, (size_t) (nl - cursor)) != 0) {
            return -1;
        }
        if (flush_pending_line(fp) != 0) {
            return -1;
        }
        cursor = nl + 1;
    }

    return 0;
}

static int crc_and_size_for_path(const char *path, uint32_t *size_out, uint32_t *crc_out)
{
    unsigned char buffer[16384];
    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint64_t total = 0;
    FILE *stream = fopen(path, "rb");
    if (!stream) {
        return -1;
    }

    for (;;) {
        size_t n = fread(buffer, 1, sizeof(buffer), stream);
        if (n > 0) {
            crc = crc32(crc, buffer, (uInt) n);
            total += n;
            if (total > UINT32_MAX) {
                fclose(stream);
                errno = EFBIG;
                return -1;
            }
        }
        if (n < sizeof(buffer)) {
            if (ferror(stream)) {
                int save_errno = errno ? errno : EIO;
                fclose(stream);
                errno = save_errno;
                return -1;
            }
            break;
        }
    }

    fclose(stream);
    *size_out = (uint32_t) total;
    *crc_out = crc;
    return 0;
}

static int zip_write_file_data(FILE *zip, const char *path)
{
    unsigned char buffer[16384];
    FILE *stream = fopen(path, "rb");
    if (!stream) {
        return -1;
    }

    for (;;) {
        size_t n = fread(buffer, 1, sizeof(buffer), stream);
        if (n > 0 && fwrite(buffer, 1, n, zip) != n) {
            int save_errno = errno ? errno : EIO;
            fclose(stream);
            errno = save_errno;
            return -1;
        }
        if (n < sizeof(buffer)) {
            if (ferror(stream)) {
                int save_errno = errno ? errno : EIO;
                fclose(stream);
                errno = save_errno;
                return -1;
            }
            break;
        }
    }

    fclose(stream);
    return 0;
}

static int zip_add_file(FILE *zip, zip_file_entry_t *entry)
{
    long offset = ftell(zip);
    size_t name_len = strlen(entry->name);

    if (offset < 0 || (uint64_t) offset > UINT32_MAX) {
        errno = EFBIG;
        return -1;
    }

    entry->offset = (uint32_t) offset;

    write_le32(zip, 0x04034b50);
    write_le16(zip, 20);
    write_le16(zip, 0);
    write_le16(zip, 0);
    write_le16(zip, entry->mod_time);
    write_le16(zip, entry->mod_date);
    write_le32(zip, entry->crc);
    write_le32(zip, entry->size);
    write_le32(zip, entry->size);
    write_le16(zip, (uint16_t) name_len);
    write_le16(zip, 0);

    if (fwrite(entry->name, 1, name_len, zip) != name_len) {
        return -1;
    }
    if (zip_write_file_data(zip, entry->path) != 0) {
        return -1;
    }

    return 0;
}

static int zip_add_memory(FILE *zip, zip_file_entry_t *entry, const zip_mem_entry_t *mem)
{
    long offset = ftell(zip);
    size_t name_len = strlen(entry->name);

    if (mem->size > UINT32_MAX || offset < 0 || (uint64_t) offset > UINT32_MAX) {
        errno = EFBIG;
        return -1;
    }

    entry->offset = (uint32_t) offset;
    entry->size = (uint32_t) mem->size;
    entry->crc = crc32(0L, mem->data, (uInt) mem->size);

    write_le32(zip, 0x04034b50);
    write_le16(zip, 20);
    write_le16(zip, 0);
    write_le16(zip, 0);
    write_le16(zip, entry->mod_time);
    write_le16(zip, entry->mod_date);
    write_le32(zip, entry->crc);
    write_le32(zip, entry->size);
    write_le32(zip, entry->size);
    write_le16(zip, (uint16_t) name_len);
    write_le16(zip, 0);

    if (fwrite(entry->name, 1, name_len, zip) != name_len) {
        return -1;
    }
    if (mem->size > 0 && fwrite(mem->data, 1, mem->size, zip) != mem->size) {
        return -1;
    }

    return 0;
}

static int zip_write_central_directory(FILE *zip,
                                       const zip_file_entry_t *entries,
                                       size_t entry_count)
{
    long cd_start = ftell(zip);
    size_t i;

    if (cd_start < 0 || (uint64_t) cd_start > UINT32_MAX) {
        errno = EFBIG;
        return -1;
    }

    for (i = 0; i < entry_count; i++) {
        size_t name_len = strlen(entries[i].name);
        write_le32(zip, 0x02014b50);
        write_le16(zip, 20);
        write_le16(zip, 20);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le16(zip, entries[i].mod_time);
        write_le16(zip, entries[i].mod_date);
        write_le32(zip, entries[i].crc);
        write_le32(zip, entries[i].size);
        write_le32(zip, entries[i].size);
        write_le16(zip, (uint16_t) name_len);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le32(zip, 0);
        write_le32(zip, entries[i].offset);
        if (fwrite(entries[i].name, 1, name_len, zip) != name_len) {
            return -1;
        }
    }

    long cd_end = ftell(zip);
    if (cd_end < 0 || (uint64_t) cd_end > UINT32_MAX) {
        errno = EFBIG;
        return -1;
    }

    write_le32(zip, 0x06054b50);
    write_le16(zip, 0);
    write_le16(zip, 0);
    write_le16(zip, (uint16_t) entry_count);
    write_le16(zip, (uint16_t) entry_count);
    write_le32(zip, (uint32_t) (cd_end - cd_start));
    write_le32(zip, (uint32_t) cd_start);
    write_le16(zip, 0);

    return 0;
}

static int build_content_types_xml(cram2excel_hfile_t *fp, strbuf_t *xml)
{
    size_t i;

    if (strbuf_append(xml,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
            "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>") != 0) {
        return -1;
    }

    for (i = 0; i < fp->align_count + 1; i++) {
        if (strbuf_appendf(xml,
                "<Override PartName=\"/xl/worksheets/sheet%zu.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>",
                i + 1) != 0) {
            return -1;
        }
    }

    return strbuf_append(xml, "</Types>");
}

static int build_root_rels_xml(strbuf_t *xml)
{
    return strbuf_append(xml,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
            "</Relationships>");
}

static int build_styles_xml(strbuf_t *xml)
{
    return strbuf_append(xml,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font></fonts>"
            "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
            "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
            "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
            "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
            "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
            "</styleSheet>");
}

static int build_workbook_xml(cram2excel_hfile_t *fp, strbuf_t *xml)
{
    size_t i;
    size_t relation_id = 1;

    if (strbuf_append(xml,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
            "<sheets>") != 0) {
        return -1;
    }

    for (i = 0; i < fp->align_count; i++, relation_id++) {
        if (strbuf_appendf(xml,
                "<sheet name=\"%s\" sheetId=\"%zu\" r:id=\"rId%zu\"/>",
                fp->align_sheets[i].name, i + 1, relation_id) != 0) {
            return -1;
        }
    }

    if (strbuf_appendf(xml,
            "<sheet name=\"%s\" sheetId=\"%zu\" r:id=\"rId%zu\"/>",
            fp->header_sheet.name, fp->align_count + 1, relation_id) != 0) {
        return -1;
    }

    return strbuf_append(xml, "</sheets></workbook>");
}

static int build_workbook_rels_xml(cram2excel_hfile_t *fp, strbuf_t *xml)
{
    size_t i;
    size_t relation_id = 1;

    if (strbuf_append(xml,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">") != 0) {
        return -1;
    }

    for (i = 0; i < fp->align_count; i++, relation_id++) {
        if (strbuf_appendf(xml,
                "<Relationship Id=\"rId%zu\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%zu.xml\"/>",
                relation_id, i + 1) != 0) {
            return -1;
        }
    }

    if (strbuf_appendf(xml,
            "<Relationship Id=\"rId%zu\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%zu.xml\"/>",
            relation_id, fp->align_count + 1) != 0) {
        return -1;
    }

    if (strbuf_appendf(xml,
            "<Relationship Id=\"rId%zu\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>",
            relation_id + 1) != 0) {
        return -1;
    }

    return strbuf_append(xml, "</Relationships>");
}

static int build_xlsx(cram2excel_hfile_t *fp)
{
    strbuf_t content_types = {0};
    strbuf_t root_rels = {0};
    strbuf_t workbook = {0};
    strbuf_t workbook_rels = {0};
    strbuf_t styles = {0};
    zip_file_entry_t *entries = NULL;
    size_t entry_count = 0;
    size_t total_entries = fp->align_count + 6;
    FILE *zip = NULL;
    uint16_t dos_time = 0;
    uint16_t dos_date = 0;
    size_t i;
    int ret = -1;

    if (worksheet_finish(&fp->header_sheet) != 0) {
        goto cleanup;
    }
    for (i = 0; i < fp->align_count; i++) {
        if (worksheet_finish(&fp->align_sheets[i]) != 0) {
            goto cleanup;
        }
    }

    if (build_content_types_xml(fp, &content_types) != 0
        || build_root_rels_xml(&root_rels) != 0
        || build_workbook_xml(fp, &workbook) != 0
        || build_workbook_rels_xml(fp, &workbook_rels) != 0
        || build_styles_xml(&styles) != 0) {
        goto cleanup;
    }

    entries = calloc(total_entries, sizeof(*entries));
    if (!entries) {
        goto cleanup;
    }

    zip_now(&dos_time, &dos_date);
    zip = fopen(fp->output_path, "wb");
    if (!zip) {
        goto cleanup;
    }

    entries[entry_count].name = dup_string("[Content_Types].xml");
    entries[entry_count].mod_time = dos_time;
    entries[entry_count].mod_date = dos_date;
    if (!entries[entry_count].name) goto cleanup;
    {
        zip_mem_entry_t mem = {
            entries[entry_count].name,
            (const unsigned char *) content_types.data,
            content_types.len
        };
        if (zip_add_memory(zip, &entries[entry_count], &mem) != 0) goto cleanup;
    }
    entry_count++;

    entries[entry_count].name = dup_string("_rels/.rels");
    entries[entry_count].mod_time = dos_time;
    entries[entry_count].mod_date = dos_date;
    if (!entries[entry_count].name) goto cleanup;
    {
        zip_mem_entry_t mem = {
            entries[entry_count].name,
            (const unsigned char *) root_rels.data,
            root_rels.len
        };
        if (zip_add_memory(zip, &entries[entry_count], &mem) != 0) goto cleanup;
    }
    entry_count++;

    entries[entry_count].name = dup_string("xl/workbook.xml");
    entries[entry_count].mod_time = dos_time;
    entries[entry_count].mod_date = dos_date;
    if (!entries[entry_count].name) goto cleanup;
    {
        zip_mem_entry_t mem = {
            entries[entry_count].name,
            (const unsigned char *) workbook.data,
            workbook.len
        };
        if (zip_add_memory(zip, &entries[entry_count], &mem) != 0) goto cleanup;
    }
    entry_count++;

    entries[entry_count].name = dup_string("xl/_rels/workbook.xml.rels");
    entries[entry_count].mod_time = dos_time;
    entries[entry_count].mod_date = dos_date;
    if (!entries[entry_count].name) goto cleanup;
    {
        zip_mem_entry_t mem = {
            entries[entry_count].name,
            (const unsigned char *) workbook_rels.data,
            workbook_rels.len
        };
        if (zip_add_memory(zip, &entries[entry_count], &mem) != 0) goto cleanup;
    }
    entry_count++;

    entries[entry_count].name = dup_string("xl/styles.xml");
    entries[entry_count].mod_time = dos_time;
    entries[entry_count].mod_date = dos_date;
    if (!entries[entry_count].name) goto cleanup;
    {
        zip_mem_entry_t mem = {
            entries[entry_count].name,
            (const unsigned char *) styles.data,
            styles.len
        };
        if (zip_add_memory(zip, &entries[entry_count], &mem) != 0) goto cleanup;
    }
    entry_count++;

    for (i = 0; i < fp->align_count; i++) {
        entries[entry_count].name = alloc_printf("xl/worksheets/sheet%zu.xml", i + 1);
        if (!entries[entry_count].name) {
            goto cleanup;
        }
        entries[entry_count].path = fp->align_sheets[i].temp_path;
        entries[entry_count].mod_time = dos_time;
        entries[entry_count].mod_date = dos_date;
        if (crc_and_size_for_path(entries[entry_count].path,
                                  &entries[entry_count].size,
                                  &entries[entry_count].crc) != 0) {
            goto cleanup;
        }
        if (zip_add_file(zip, &entries[entry_count]) != 0) {
            goto cleanup;
        }
        entry_count++;
    }

    entries[entry_count].name = alloc_printf("xl/worksheets/sheet%zu.xml", fp->align_count + 1);
    if (!entries[entry_count].name) {
        goto cleanup;
    }
    entries[entry_count].path = fp->header_sheet.temp_path;
    entries[entry_count].mod_time = dos_time;
    entries[entry_count].mod_date = dos_date;
    if (crc_and_size_for_path(entries[entry_count].path,
                              &entries[entry_count].size,
                              &entries[entry_count].crc) != 0) {
        goto cleanup;
    }
    if (zip_add_file(zip, &entries[entry_count]) != 0) {
        goto cleanup;
    }
    entry_count++;

    if (zip_write_central_directory(zip, entries, entry_count) != 0) {
        goto cleanup;
    }
    if (fclose(zip) != 0) {
        zip = NULL;
        goto cleanup;
    }
    zip = NULL;

    ret = 0;

cleanup:
    if (zip) {
        fclose(zip);
        if (ret != 0) {
            unlink(fp->output_path);
        }
    }
    for (i = 0; entries && i < total_entries; i++) {
        free(entries[i].name);
    }
    free(entries);
    strbuf_free(&content_types);
    strbuf_free(&root_rels);
    strbuf_free(&workbook);
    strbuf_free(&workbook_rels);
    strbuf_free(&styles);
    return ret;
}

static void cleanup_plugin_state(cram2excel_hfile_t *fp)
{
    size_t i;

    worksheet_discard(&fp->header_sheet);
    for (i = 0; i < fp->align_count; i++) {
        worksheet_discard(&fp->align_sheets[i]);
    }
    free(fp->align_sheets);
    fp->align_sheets = NULL;
    fp->align_count = 0;
    fp->align_cap = 0;

    free(fp->output_path);
    fp->output_path = NULL;

    free(fp->pending);
    fp->pending = NULL;
    fp->pending_len = 0;
    fp->pending_cap = 0;
}

static ssize_t cram2excel_write(hFILE *fpv, const void *buffer, size_t nbytes)
{
    cram2excel_hfile_t *fp = (cram2excel_hfile_t *) fpv;

    if (fp->failure_errno) {
        errno = fp->failure_errno;
        return -1;
    }

    if (process_chunk(fp, (const char *) buffer, nbytes) != 0) {
        fp->failure_errno = errno ? errno : EIO;
        return -1;
    }

    return (ssize_t) nbytes;
}

static off_t cram2excel_seek(hFILE *fpv, off_t offset, int whence)
{
    (void) fpv;
    (void) offset;
    (void) whence;
    errno = ESPIPE;
    return -1;
}

static int cram2excel_flush(hFILE *fpv)
{
    (void) fpv;
    return 0;
}

static int cram2excel_close(hFILE *fpv)
{
    cram2excel_hfile_t *fp = (cram2excel_hfile_t *) fpv;
    int ret = 0;

    if (!fp->failure_errno && flush_pending_line(fp) != 0) {
        fp->failure_errno = errno ? errno : EIO;
    }

    if (!fp->failure_errno && build_xlsx(fp) != 0) {
        fp->failure_errno = errno ? errno : EIO;
    }

    cleanup_plugin_state(fp);

    if (fp->failure_errno) {
        errno = fp->failure_errno;
        ret = -1;
    }

    return ret;
}

static hFILE *cram2excel_open(const char *filename, const char *mode)
{
    cram2excel_hfile_t *fp = NULL;
    const char *path = filename;

    if (strncmp(path, CRAM2EXCEL_SCHEME, strlen(CRAM2EXCEL_SCHEME)) == 0) {
        path += strlen(CRAM2EXCEL_SCHEME);
    }

    if (strchr(mode, 'w') == NULL || strchr(mode, '+') != NULL || strchr(mode, 'a') != NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (!path || *path == '\0') {
        errno = EINVAL;
        return NULL;
    }

    fp = (cram2excel_hfile_t *) hfile_init(sizeof(*fp), mode, DEFAULT_BUFFER_SIZE);
    if (!fp) {
        return NULL;
    }
    memset(((char *) fp) + sizeof(fp->base), 0, sizeof(*fp) - sizeof(fp->base));
    fp->base.backend = &cram2excel_backend;

    fp->output_path = dup_string(path);
    if (!fp->output_path) {
        goto error;
    }

    if (worksheet_open(&fp->header_sheet, "Header") != 0) {
        goto error;
    }
    if (add_alignment_sheet(fp) != 0) {
        goto error;
    }
    if (append_pending(fp, "", 0) != 0) {
        goto error;
    }

    return &fp->base;

error:
    cleanup_plugin_state(fp);
    hfile_destroy((hFILE *) fp);
    return NULL;
}

static const struct hFILE_backend cram2excel_backend = {
    NULL,
    cram2excel_write,
    cram2excel_seek,
    cram2excel_flush,
    cram2excel_close
};

int hfile_plugin_init(struct hFILE_plugin *self)
{
    static const struct hFILE_scheme_handler handler = {
        cram2excel_open,
        hfile_always_local,
        "cram2excel",
        2000 + 60,
        NULL
    };

    if (self->api_version != 1) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    self->name = "CRAM2Excel";
    hfile_add_scheme_handler("cram2excel", &handler);
    return 0;
}
