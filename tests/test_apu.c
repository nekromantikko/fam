// Based on APU tests by Chris "100th_Coin" Siebert:
// https://github.com/100thCoin/AccuracyCoin
// The AccuracyCoin APU tests are in turn largely based on APU tests by blargg.

#include <unity.h>
#include <familib.h>

static fam_Apu *apu;

static void clock_apu(int cpu_cycles) {
    // NOTE: fam_apu_clock advances by one APU cycle (= 2 CPU cycles)
    // Might need to increase the granularity if we'll end up having more
    // precise timing tests
    int apu_cycles = cpu_cycles / 2;
    for (int i = 0; i < apu_cycles; i++) {
        fam_apu_clock(apu, NULL);
    }
}

static void test_length_counter(uint16_t loop_addr, uint16_t load_addr, uint8_t channel_mask) {
    uint8_t status;

    // Test 1: Reading from $4015 should not state that the channel is playing before writing length counter load
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 1: channel should not be playing before length counter load write");

    // Test 2: Reading from $4015 should state that the channel is playing after writing length counter load.
    fam_apu_write_register(apu, load_addr, 0x18);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(channel_mask, status, "Test 2: pulse 1 should be playing after length counter load write");

    // Test 3: The audio channel should automatically stop playing if you wait for the length counter to expire.
    clock_apu(29780 * 15); // Wait for 15 frames (TODO: PAL probably has a different CPU cycle count per frame)
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 3: channel should stop playing after the length counter expires");

    // Test 4: Writing $80 to $4017 should immediately clock the Length Counter.
    fam_apu_write_register(apu, 0x4017, 0x00); // Reset frame counter
    fam_apu_write_register(apu, load_addr, 0x18); // Load length counter with value 2
    fam_apu_write_register(apu, 0x4017, 0x80); // 5-step mode, should clock length counter
    fam_apu_write_register(apu, 0x4017, 0x80); // Clock again, length counter should be 0
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 4: two $80->$4017 writes should clock the length counter from 2 down to 0");

    // Test 5: Writing $00 to $4017 should not clock the Length Counter.
    fam_apu_write_register(apu, 0x4017, 0x00); // Reset frame counter
    fam_apu_write_register(apu, load_addr, 0x18); // Load length counter with value 2
    fam_apu_write_register(apu, 0x4017, 0x00); // Should NOT clock length counter
    fam_apu_write_register(apu, 0x4017, 0x00); // Should NOT clock length counter
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(channel_mask, status, "Test 5: $00->$4017 writes should not clock the length counter, so channel keeps playing");

    // Test 6: Disabling the audio channel should immediately clear the length counter to zero.
    fam_apu_write_register(apu, load_addr, 0x18); // Load length counter with value 2
    fam_apu_write_register(apu, 0x4015, 0x00); // Disable channel
    fam_apu_write_register(apu, 0x4015, channel_mask); // Enable again
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 6: disabling channel via $4015 should clear the length counter to 0");

    // Test 7: The length counter shouldn't be set when the channel is disabled.
    fam_apu_write_register(apu, 0x4015, 0x00); // Disable channel
    fam_apu_write_register(apu, load_addr, 0x18); // Should not affect length counter
    fam_apu_write_register(apu, 0x4015, channel_mask); // Enable channel
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 7: length counter should not be loaded when channel is disabled");

    // Test 8: If the channel is set to play infinitely, it shouldn't clock the length counter.
    fam_apu_write_register(apu, 0x4017, 0x00); // Reset frame counter
    fam_apu_write_register(apu, load_addr, 0x18); // Load length counter with value 2
    fam_apu_write_register(apu, loop_addr, 0x30); // Set loop = true
    fam_apu_write_register(apu, 0x4017, 0x80); // Try to clock length counter
    fam_apu_write_register(apu, 0x4017, 0x80); // This should not do anything since loop is enabled
    fam_apu_write_register(apu, 0x4000, 0x10); // Set loop = false
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(channel_mask, status, "Test 8: while loop is enabled the length counter should not be clocked, so channel keeps playing");

    // Test 9: If the channel is set to play infinitely, the length counter should be left unchanged.
    fam_apu_write_register(apu, 0x4017, 0x00); // Reset frame counter
    fam_apu_write_register(apu, load_addr, 0x18); // Load length counter with value 2
    fam_apu_write_register(apu, loop_addr, 0x30); // Set loop = true
    fam_apu_write_register(apu, 0x4017, 0x80); // Try to clock length counter
    fam_apu_write_register(apu, 0x4017, 0x80); // This should not do anything since loop is enabled
    fam_apu_write_register(apu, loop_addr, 0x10); // Set loop = false
    fam_apu_write_register(apu, 0x4017, 0x80); // Try to clock length counter
    fam_apu_write_register(apu, 0x4017, 0x80); // This time it should work
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 9: the length counter is left unchanged (not reloaded) while looping, then clocks to 0 once loop is cleared");
}

static void test_length_table(uint16_t load_addr) {
    static const uint8_t expected[32] = {
        10, 254, 20, 2, 40, 4, 80, 6,
        160, 8, 60, 10, 14, 12, 26, 14,
        12, 16, 24, 18, 48, 20, 96, 22,
        192, 24, 72, 26, 16, 28, 32, 30
    };

    for (int i = 0; i < 32; i++) {
        fam_apu_write_register(apu, 0x4017, 0x00); // Reset frame counter
        fam_apu_write_register(apu, load_addr, (uint8_t)(i << 3));

        int count = 0;
        uint8_t status;
        do {
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(expected[i], count, "Length counter did not reach zero in expected number of clocks");
            fam_apu_write_register(apu, 0x4017, 0x80); // Clock length counter
            count++;
            fam_apu_read_register(apu, 0x4015, &status);
        } while (status != 0x00);
        TEST_ASSERT_EQUAL_INT(expected[i], count);
    }
}

static void setup_pulse1(void) {
    fam_apu_write_register(apu, 0x4017, 0x40); // Disable the frame counter IRQ's
    fam_apu_write_register(apu, 0x4015, 0x01); // Enable pulse 1
    fam_apu_write_register(apu, 0x4000, 0x10); // Loop = false, constant volume = true
    fam_apu_write_register(apu, 0x4001, 0x7F); // Disable sweep
    fam_apu_write_register(apu, 0x4002, 0xFF); // Max value to timer low byte
}

static void test_length_counter_pulse1(void) {
    setup_pulse1();
    test_length_counter(0x4000, 0x4003, 0x01);
}

static void test_length_table_pulse1(void) {
    setup_pulse1(); 
    test_length_table(0x4003);
}

static void setup_pulse2(void) {
    fam_apu_write_register(apu, 0x4017, 0x40); // Disable the frame counter IRQ's
    fam_apu_write_register(apu, 0x4015, 0x02); // Enable pulse 2
    fam_apu_write_register(apu, 0x4004, 0x10); // Loop = false, constant volume = true
    fam_apu_write_register(apu, 0x4005, 0x7F); // Disable sweep
    fam_apu_write_register(apu, 0x4006, 0xFF); // Max value to timer low byte
}

static void test_length_counter_pulse2(void) {
    setup_pulse2();
    test_length_counter(0x4004, 0x4007, 0x02);
}

static void test_length_table_pulse2(void) {
    setup_pulse2();
    test_length_table(0x4007);
}

void setUp(void) {
    apu = fam_apu_init();
}

void tearDown(void) {
    fam_apu_free(apu);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_length_counter_pulse1);
    RUN_TEST(test_length_table_pulse1);
    RUN_TEST(test_length_counter_pulse2);
    RUN_TEST(test_length_table_pulse2);
    return UNITY_END();
}