/* Standalone regression for the file/memory reader APIs and bounded processing. */
#include "../BtrieveFileSaverLib.h"
#include <limits.h>
#include <stddef.h>

static void write_u16(unsigned char *bytes, size_t offset, unsigned int value)
{
    bytes[offset] = (unsigned char)value;
    bytes[offset + 1] = (unsigned char)(value >> 8);
}
static int progress(void *context, uint64_t units)
{
    uint64_t *remaining = (uint64_t*)context;
    if (units > *remaining) return 0;
    *remaining -= units;
    return 1;
}
static void fixture(unsigned char *bytes)
{
    unsigned int index;
    memset(bytes, 0, 1536);
    write_u16(bytes, 6, 0x0500);
    write_u16(bytes, 8, 512);
    write_u16(bytes, 0x16, 16);
    write_u16(bytes, 0x18, 20);
    write_u16(bytes, 0x1c, 1);
    bytes[0x38] = 1;
    write_u16(bytes, 512 + 4, 0x8001);
    bytes[512 + 6 + 8] = 1;
    bytes[512 + 6 + 17] = 2;
    write_u16(bytes, 1024 + 510, 16);
    write_u16(bytes, 1024 + 508, 32);
    for (index = 16; index < 32; ++index) bytes[1024 + index] = 'Z';
}
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #expression); return 1; } } while (0)

int main(int argc, char **argv)
{
    unsigned char bytes[1536], original[1536];
    char file_record[1024], memory_record[1024];
    unsigned long file_length = sizeof(file_record), memory_length = sizeof(memory_record);
    uint64_t remaining = 0;
    unsigned int index;
    CLIENT_STRUCT file_client, memory_client;
    BF_READ_OPTIONS options = {4096, NULL, NULL};
    FILE *output;
    CHECK(argc == 2);
    CHECK(offsetof(FCR, FixedRecordLength) == 0x16);
    CHECK(offsetof(FCR, IFixedRecordLength) == 0x18);
    CHECK(offsetof(FCR, VarRecsAllowed) == 0x38);
    CHECK(offsetof(CLIENT_STRUCT, numDATPages) % sizeof(unsigned long) == 0);
    CHECK(offsetof(CLIENT_STRUCT, datCapacity) % sizeof(unsigned long) == 0);
    CHECK(offsetof(CLIENT_STRUCT, DATArr) % sizeof(PAGE_LINK*) == 0);
    fixture(bytes);
    memcpy(original, bytes, sizeof(bytes));
    output = fopen(argv[1], "wb");
    CHECK(output != NULL);
    CHECK(fwrite(bytes, 1, sizeof(bytes), output) == sizeof(bytes));
    CHECK(fclose(output) == 0);
    CHECK(BF_OPEN_WITH_OPTIONS(&file_client, argv[1], &options) == NO_ERROR);
    CHECK(BF_OPEN_MEMORY(&memory_client, bytes, sizeof(bytes), &options) == NO_ERROR);
    CHECK(BF_GET_REC(&file_client, file_record, &file_length) == NO_ERROR);
    CHECK(BF_GET_REC(&memory_client, memory_record, &memory_length) == NO_ERROR);
    CHECK(memory_length == file_length && memcmp(file_record, memory_record, memory_length) == 0);
    CHECK(memory_length == 32);
    CHECK(BF_CLOSE(&file_client) == NO_ERROR);
    CHECK(BF_CLOSE(&memory_client) == NO_ERROR);
    CHECK(memcmp(bytes, original, sizeof(bytes)) == 0);

    options.maximum_file_bytes = 1;
    CHECK(BF_OPEN_MEMORY(&memory_client, bytes, sizeof(bytes), &options) == PROCESSING_LIMIT);
    BF_CLOSE(&memory_client);
    options.maximum_file_bytes = 4096;
    options.progress = progress;
    options.context = &remaining;
    CHECK(BF_OPEN_MEMORY(&memory_client, bytes, sizeof(bytes), &options) == PROCESSING_LIMIT);
    BF_CLOSE(&memory_client);
    options.progress = NULL;
    CHECK(BF_OPEN_MEMORY(&memory_client, bytes, sizeof(bytes), &options) == NO_ERROR);
    memory_client.progress = progress;
    memory_client.progressContext = &remaining;
    memory_length = sizeof(memory_record);
    CHECK(BF_GET_REC(&memory_client, memory_record, &memory_length) == PROCESSING_LIMIT);
    BF_CLOSE(&memory_client);

    bytes[512 + 6 + 17] = 0;
    CHECK(BF_OPEN_MEMORY(&memory_client, bytes, sizeof(bytes), &options) == NO_ERROR);
    memory_length = sizeof(memory_record);
    CHECK(BF_GET_REC(&memory_client, memory_record, &memory_length) == NO_ERROR);
    CHECK(memory_length == 16);
    BF_CLOSE(&memory_client);

    /* Header, fragment index/table, out-of-file and cycle mutations. */
    for (index = 0; index < 8; ++index) {
        unsigned short status;
        fixture(bytes);
        if (index == 0) write_u16(bytes, 8, 0);
        if (index == 1) write_u16(bytes, 8, 1);
        if (index == 2) write_u16(bytes, 0x18, 0);
        if (index == 3) write_u16(bytes, 0x16, 600);
        if (index == 4) bytes[512 + 6 + 19] = 255;
        if (index == 5) { write_u16(bytes, 1534, 0x7000); write_u16(bytes, 1532, 0x7010); }
        if (index == 6) memset(bytes + 1024, 0xff, 510);
        if (index == 7) { write_u16(bytes, 1534, 0x8010); bytes[1040] = 0; bytes[1041] = 2; bytes[1042] = 0; bytes[1043] = 0; }
        remaining = UINT64_C(100000);
        options.progress = progress;
        status = BF_OPEN_MEMORY(&memory_client, bytes, sizeof(bytes), &options);
        if (status == NO_ERROR) {
            memory_length = sizeof(memory_record);
            status = BF_GET_REC(&memory_client, memory_record, &memory_length);
        }
        CHECK(status != NO_ERROR);
        CHECK(BF_CLOSE(&memory_client) == NO_ERROR);
    }
    CHECK(remove(argv[1]) == 0);
    puts("memory/file parity, limits, cancellation, cleanup and malformed inputs passed");
    return 0;
}
