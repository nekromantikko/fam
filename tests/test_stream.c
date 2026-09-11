// Tests for the .fam loader in src/stream.c.
//
// NOTE: The header layout built below mirrors parse_header() in src/stream.c. Keeping a second
// copy of it here is deliberate: it pins the on-disk format, so a change to the layout has to be
// a conscious change to this file as well.

#include <unity.h>
#include <string.h>
#include <stdlib.h>
#include <fam/stream.h>
#include <fam/common.h>

// magic + version major/minor + usage + channel id/mask + dpcm bank count/offset +
// stream length/offset + loop point + machine
#define HEADER_SIZE (4 + 4 + 4 + 1 + 8 + 4 + 8 + 4 + 8 + 4 + 1)
#define FILE_SIZE (HEADER_SIZE + 2) // Header plus a single stream operation

// Format limits of this version of fam. These mirror stream_types.h, which tests can't include
// since it's private to the library
#define CHANNEL_COUNT 5
#define CHANNEL_MASK_ALL ((1ull << CHANNEL_COUNT) - 1)
#define SFX_CHANNELS 4
#define NO_LOOP 0xFFFFFFFFu

#define USAGE_MUSIC 0
#define USAGE_SFX 1

typedef struct FileParams {
    const char* magic; // 4 bytes, including the terminator
    uint32_t version_major;
    uint32_t version_minor;
    uint8_t usage;
    uint64_t channel_id_mask;
    uint32_t dpcm_bank_count;
    uint64_t dpcm_bank_offset;
    uint32_t stream_length;
    uint64_t stream_offset;
    uint32_t loop_point;
    uint8_t machine;
} FileParams;

static FileParams music_params(void) {
    FileParams params = {
        .magic = "FAM",
        .version_major = 1,
        .version_minor = 0,
        .usage = USAGE_MUSIC,
        .channel_id_mask = CHANNEL_MASK_ALL,
        .dpcm_bank_count = 0,
        .dpcm_bank_offset = 0,
        .stream_length = 1,
        .stream_offset = HEADER_SIZE,
        .loop_point = NO_LOOP,
        .machine = FAM_MACHINE_NTSC
    };
    return params;
}

static FileParams sfx_params(void) {
    FileParams params = music_params();
    params.usage = USAGE_SFX;
    params.channel_id_mask = 0; // Channel ID, not a mask
    return params;
}

static size_t write_field(uint8_t* buffer, size_t offset, const void* data, size_t size) {
    memcpy(buffer + offset, data, size);
    return offset + size;
}

// Writes a complete file into buffer (which must hold FILE_SIZE bytes) and returns its size
static size_t build_file(const FileParams* params, uint8_t* buffer) {
    size_t offset = 0;

    offset = write_field(buffer, offset, params->magic, 4);
    offset = write_field(buffer, offset, &params->version_major, sizeof(uint32_t));
    offset = write_field(buffer, offset, &params->version_minor, sizeof(uint32_t));
    offset = write_field(buffer, offset, &params->usage, sizeof(uint8_t));
    offset = write_field(buffer, offset, &params->channel_id_mask, sizeof(uint64_t));
    offset = write_field(buffer, offset, &params->dpcm_bank_count, sizeof(uint32_t));
    offset = write_field(buffer, offset, &params->dpcm_bank_offset, sizeof(uint64_t));
    offset = write_field(buffer, offset, &params->stream_length, sizeof(uint32_t));
    offset = write_field(buffer, offset, &params->stream_offset, sizeof(uint64_t));
    offset = write_field(buffer, offset, &params->loop_point, sizeof(uint32_t));
    offset = write_field(buffer, offset, &params->machine, sizeof(uint8_t));

    TEST_ASSERT_EQUAL_size_t_MESSAGE(HEADER_SIZE, offset,
        "the header built by the test should match HEADER_SIZE");

    buffer[offset++] = 0xFF; // OP_ENDSTREAM
    buffer[offset++] = 0x00;

    return offset;
}

// Loads a music file built from params and checks the result. A malformed file is rejected by
// the size query rather than by loading, so both steps are checked against the same expectation
static void assert_music_result(const FileParams* params, FamResult expected, const char* message) {
    uint8_t buffer[FILE_SIZE];
    size_t size = build_file(params, buffer);

    size_t required_size = 0;
    FamResult size_result = fam_music_get_memory_required(size, buffer, &required_size);
    if (size_result != FAM_SUCCESS) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected, size_result, message);
        return;
    }

    void* memory = malloc(required_size);
    TEST_ASSERT_NOT_NULL(memory);

    FamMusic* music = NULL;
    FamResult result = fam_music_from_buffer(&music, memory, size, buffer);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, result, message);

    if (result == FAM_SUCCESS) {
        TEST_ASSERT_NOT_NULL_MESSAGE(music, "a successful load should return a music object");
    }

    free(memory);
}

static void assert_sfx_result(const FileParams* params, FamResult expected, const char* message) {
    uint8_t buffer[FILE_SIZE];
    size_t size = build_file(params, buffer);

    size_t required_size = 0;
    FamResult size_result = fam_sfx_get_memory_required(size, buffer, &required_size);
    if (size_result != FAM_SUCCESS) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected, size_result, message);
        return;
    }

    void* memory = malloc(required_size);
    TEST_ASSERT_NOT_NULL(memory);

    FamSfx* sfx = NULL;
    FamResult result = fam_sfx_from_buffer(&sfx, memory, size, buffer);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, result, message);

    if (result == FAM_SUCCESS) {
        TEST_ASSERT_NOT_NULL_MESSAGE(sfx, "a successful load should return an sfx object");
    }

    free(memory);
}

static void test_channel_mask(void) {
    FileParams params = music_params();

    // Test 1: Any combination of the channels we know about should load
    params.channel_id_mask = CHANNEL_MASK_ALL;
    assert_music_result(&params, FAM_SUCCESS, "Test 1: all 2A03 channels should load");
    params.channel_id_mask = 0x01;
    assert_music_result(&params, FAM_SUCCESS, "Test 1: a single channel should load");
    params.channel_id_mask = 0x00;
    assert_music_result(&params, FAM_SUCCESS, "Test 1: an empty channel mask should load");

    // Test 2: Channels we don't know about belong to an expansion chip, so the stream needs a
    // newer version of fam and should be refused instead of played without them
    params.channel_id_mask = CHANNEL_MASK_ALL + 1;
    assert_music_result(&params, FAM_ERROR_UNSUPPORTED_FEATURE,
        "Test 2: an unknown channel should be refused");
    params.channel_id_mask = CHANNEL_MASK_ALL | (CHANNEL_MASK_ALL + 1);
    assert_music_result(&params, FAM_ERROR_UNSUPPORTED_FEATURE,
        "Test 2: known channels plus an unknown one should be refused, not partially played");
    params.channel_id_mask = 1ull << 63;
    assert_music_result(&params, FAM_ERROR_UNSUPPORTED_FEATURE,
        "Test 2: the highest channel bit should be refused");
}

static void test_sfx_channel_id(void) {
    FileParams params = sfx_params();

    // Test 1: Sound effects can play on any channel but the DMC
    for (uint64_t channel = 0; channel < SFX_CHANNELS; channel++) {
        params.channel_id_mask = channel;
        assert_sfx_result(&params, FAM_SUCCESS, "Test 1: every sfx channel should load");
    }

    // Test 2: Channel IDs past the last sfx channel are invalid
    params.channel_id_mask = SFX_CHANNELS;
    assert_sfx_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 2: an out of range sfx channel ID should be refused");
}

static void test_machine(void) {
    uint8_t buffer[FILE_SIZE];
    FileParams params = music_params();

    // Test 1: The machine a stream was made for should survive loading
    params.machine = FAM_MACHINE_PAL;
    size_t size = build_file(&params, buffer);
    size_t required_size = 0;
    TEST_ASSERT_EQUAL_INT(FAM_SUCCESS, fam_music_get_memory_required(size, buffer, &required_size));
    void* music_memory = malloc(required_size);
    FamMusic* music = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_SUCCESS,
        fam_music_from_buffer(&music, music_memory, size, buffer),
        "Test 1: a PAL music stream should load");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_MACHINE_PAL, fam_music_get_machine(music),
        "Test 1: a PAL music stream should report itself as PAL");
    free(music_memory);

    // The same for sound effects, which is what the player checks before playing one
    FileParams sfx_file = sfx_params();
    sfx_file.machine = FAM_MACHINE_PAL;
    size = build_file(&sfx_file, buffer);
    TEST_ASSERT_EQUAL_INT(FAM_SUCCESS, fam_sfx_get_memory_required(size, buffer, &required_size));
    void* sfx_memory = malloc(required_size);
    FamSfx* sfx = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_SUCCESS,
        fam_sfx_from_buffer(&sfx, sfx_memory, size, buffer),
        "Test 1: a PAL sfx stream should load");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_MACHINE_PAL, fam_sfx_get_machine(sfx),
        "Test 1: a PAL sfx stream should report itself as PAL");
    free(sfx_memory);

    // Test 2: Machines we don't know about are invalid
    params.machine = FAM_MACHINE_PAL + 1;
    assert_music_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 2: an unknown machine should be refused");
}

static void test_header_validation(void) {
    FileParams params = music_params();

    // Test 1: A file that isn't a .fam file at all
    params.magic = "BAD";
    assert_music_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 1: a bad magic number should be refused");

    // Test 2: A different major version is a different format
    params = music_params();
    params.version_major = 2;
    assert_music_result(&params, FAM_ERROR_UNSUPPORTED_VERSION,
        "Test 2: a newer major version should be refused");

    // Test 3: A newer minor version may contain fields this version doesn't know how to read,
    // while an older one is fine
    params = music_params();
    params.version_minor = 1;
    assert_music_result(&params, FAM_ERROR_UNSUPPORTED_VERSION,
        "Test 3: a newer minor version should be refused");

    // Test 4: Music and sound effects are not interchangeable
    params = music_params();
    assert_sfx_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 4: loading a music stream as an sfx should be refused");
    FileParams sfx_file = sfx_params();
    assert_music_result(&sfx_file, FAM_ERROR_INVALID_FORMAT,
        "Test 4: loading an sfx stream as music should be refused");

    // Test 5: An unknown usage
    params = music_params();
    params.usage = 2;
    assert_music_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 5: an unknown usage should be refused");

    // Test 6: A loop point has to be inside the stream
    params = music_params();
    params.loop_point = params.stream_length;
    assert_music_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 6: a loop point past the end of the stream should be refused");

    // Test 7: Offsets have to be inside the file
    params = music_params();
    params.stream_offset = FILE_SIZE;
    assert_music_result(&params, FAM_ERROR_INVALID_FORMAT,
        "Test 7: a stream offset past the end of the file should be refused");

    // Test 8: A file that got cut short
    uint8_t buffer[FILE_SIZE];
    params = music_params();
    build_file(&params, buffer);
    size_t required_size = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_ERROR_INVALID_FORMAT,
        fam_music_get_memory_required(HEADER_SIZE / 2, buffer, &required_size),
        "Test 8: a truncated file should be refused");

    // Test 9: Missing arguments
    TEST_ASSERT_EQUAL_INT(FAM_SUCCESS, fam_music_get_memory_required(FILE_SIZE, buffer, &required_size));
    void* memory = malloc(required_size);
    FamMusic* music = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_ERROR_INVALID_ARGUMENT,
        fam_music_from_buffer(NULL, memory, FILE_SIZE, buffer),
        "Test 9: loading without an out parameter should fail");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_ERROR_INVALID_ARGUMENT,
        fam_music_from_buffer(&music, memory, FILE_SIZE, NULL),
        "Test 9: loading without a buffer should fail");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_ERROR_INVALID_ARGUMENT,
        fam_music_get_memory_required(FILE_SIZE, buffer, NULL),
        "Test 9: asking for the size without somewhere to put it should fail");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FAM_ERROR_INVALID_ARGUMENT,
        fam_music_from_buffer(&music, NULL, FILE_SIZE, buffer),
        "Test 9: loading without memory should fail");

    free(memory);
}

void setUp(void) {
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_header_validation);
    RUN_TEST(test_channel_mask);
    RUN_TEST(test_sfx_channel_id);
    RUN_TEST(test_machine);
    return UNITY_END();
}
