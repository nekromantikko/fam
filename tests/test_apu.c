// Based on APU tests by Chris "100th_Coin" Siebert:
// https://github.com/100thCoin/AccuracyCoin
// The AccuracyCoin APU tests are in turn largely based on APU tests by blargg.

#include <unity.h>
#include <fam/apu.h>

static FamApu *apu;

static void clock_apu(int cpu_cycles) {
    // NOTE: fam_apu_clock advances by one APU cycle (= 2 CPU cycles)
    // Might need to increase the granularity if we'll end up having more
    // precise timing tests
    int apu_cycles = cpu_cycles / 2;
    for (int i = 0; i < apu_cycles; i++) {
        fam_apu_clock(apu);
    }
}

static void test_length_counter(uint8_t channel_mask, uint16_t load_addr, uint16_t loop_addr) {
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

    if (loop_addr == 0) return;

    // Test 8: If the channel is set to play infinitely, it shouldn't clock the length counter.
    fam_apu_write_register(apu, 0x4017, 0x00); // Reset frame counter
    fam_apu_write_register(apu, load_addr, 0x18); // Load length counter with value 2
    fam_apu_write_register(apu, loop_addr, 0x30); // Set loop = true
    fam_apu_write_register(apu, 0x4017, 0x80); // Try to clock length counter
    fam_apu_write_register(apu, 0x4017, 0x80); // This should not do anything since loop is enabled
    fam_apu_write_register(apu, loop_addr, 0x10); // Set loop = false
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
    test_length_counter(0x01, 0x4003, 0x4000);
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
    test_length_counter(0x02, 0x4007, 0x4004);
}

static void test_length_table_pulse2(void) {
    setup_pulse2();
    test_length_table(0x4007);
}

static void setup_triangle(void) {
    fam_apu_write_register(apu, 0x4017, 0x40); // Disable the frame counter IRQ's
    fam_apu_write_register(apu, 0x4015, 0x04); // Enable triangle
    fam_apu_write_register(apu, 0x400A, 0xFF); // Max value to timer low byte
}

static void test_length_counter_triangle(void) {
    setup_triangle();
    test_length_counter(0x04, 0x400B, 0);
}

static void test_length_table_triangle(void) {
    setup_triangle();
    test_length_table(0x400B);
}

static void setup_noise(void) {
    fam_apu_write_register(apu, 0x4017, 0x40); // Disable the frame counter IRQ's
    fam_apu_write_register(apu, 0x4015, 0x08); // Enable noise
    fam_apu_write_register(apu, 0x400C, 0x10); // Loop = false, constant volume = true
}

static void test_length_counter_noise(void) {
    setup_noise();
    test_length_counter(0x08, 0x400F, 0x400C);
}

static void test_length_table_noise(void) {
    setup_noise();
    test_length_table(0x400F);
}

static void test_frame_counter_irq(void) {
    uint8_t status;

    // Test 1: The IRQ flag is set when the APU Frame counter is in the 4-step mode, and the IRQ flag is enabled.
    fam_apu_write_register(apu, 0x4017, 0x00); // 4-step mode, enable IRQ
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 1: frame IRQ flag should be set in 4-step mode with IRQ enabled");

    // Test 2: The IRQ flag should not be set when the APU frame counter is in the 4-step mode, and the IRQ flag is disabled.
    fam_apu_write_register(apu, 0x4017, 0x40); // 4-step mode, disable IRQ
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 2: frame IRQ flag should not be set in 4-step mode when IRQ is disabled ($40 to $4017)");

    // Test 3: The IRQ flag should not be set when the APU frame counter is in the 5-step mode, and the IRQ flag is enabled.
    fam_apu_write_register(apu, 0x4017, 0x80); // 5-step mode, enable IRQ (Which should do nothing)
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 3: frame IRQ flag should never be set in 5-step mode, even with the IRQ-enable bit clear");

    // Test 4: The IRQ flag should not be set when the APU frame counter is in the 5-step mode, and the IRQ flag is disabled.
    fam_apu_write_register(apu, 0x4017, 0xC0); // 5-step mode, disable IRQ
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 4: frame IRQ flag should never be set in 5-step mode, with IRQ disabled");

    // Test 5: Reading the IRQ flag should clear the IRQ flag.
    fam_apu_write_register(apu, 0x4017, 0x00); // 4-step mode, enable IRQ
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_read_register(apu, 0x4015, &status); // Read to clear IRQ flag
    fam_apu_read_register(apu, 0x4015, &status); // Read again, should be cleared now
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 5: reading $4015 should clear the frame IRQ flag, so the second read returns 0");

    // Test 6: The IRQ flag should be cleared when the APU transitions from a "put" cycle to a "get" cycle.
    // Test 7: The IRQ flag should not be cleared yet the APU transitions from a "get" cycle to a "put" cycle.

    // TODO: Do we care about such precise timing things? How would this even be implemented?

    // Test 8: Changing the frame counter to 5-step mode after the flag was set should not clear the flag.
    fam_apu_write_register(apu, 0x4017, 0x00); // 4-step mode, enable IRQ
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_write_register(apu, 0x4017, 0x80); // 5-step mode, enable IRQ
    fam_apu_read_register(apu, 0x4015, &status); // IRQ flag should still be set
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 8: switching to 5-step mode after the flag is already set should leave it set");

    // Test 9: Disabling the IRQ flag should clear the IRQ flag.
    fam_apu_write_register(apu, 0x4017, 0x00); // 4-step mode, enable IRQ
    clock_apu(30000); // wait long enough that the IRQ flag would be set
    fam_apu_write_register(apu, 0x4017, 0x40); // clear the IRQ flag
    fam_apu_read_register(apu, 0x4015, &status); // IRQ flag should be cleared
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status, "Test 9: setting the IRQ-disable bit ($40 to $4017) should clear an already-set frame IRQ flag");

    // Test A: The IRQ flag was enabled too early. (writing to $4017 on an odd CPU cycle.)
    // Test B: The IRQ flag was enabled too late. (writing to $4017 on an odd CPU cycle.)
    // Test C: The IRQ flag was enabled too early. (writing to $4017 on an even CPU cycle.)
    // Test D: The IRQ flag was enabled too late. (writing to $4017 on an even CPU cycle.)
    // Test E: Reading $4015 on the last cycle before the IRQ flag is set should not clear the IRQ flag. (it gets set on the following 2 CPU cycles)
    // Test F: Reading $4015 on the same cycle the IRQ flag is set should not clear the IRQ flag. (it gets set again on the following CPU cycle)
    // Test G: Reading $4015 1 cycle later than the previous test should not clear the IRQ flag. (it gets set again on this CPU cycle)
    // Test H: Reading $4015 1 cycle later than the previous test should clear the IRQ flag.
    // Test I: The Frame Counter Interrupt flag should not have been set 29827 cycles after resetting the frame counter.
    // Test J: The Frame Counter Interrupt flag should have been set 29828 cycles after resetting the frame counter, even if supressing Frame Counter Interrupts.
    // Test K: The Frame Counter Interrupt flag should have been set 29829 cycles after resetting the frame counter, even if supressing Frame Counter Interrupts.
    // Test L: The Frame Counter Interrupt flag should not have been set 29830 cycles after resetting the frame counter if supressing Frame Counter Interrupts.
    // Test M: Despite the Frame Counter Interrupt flag being set for those 2 CPU cycles, if suppressing Frame Counter Interrupts, an IRQ should not occur.
    // Test N: The IRQ Occurs on the wrong CPU cycle.
    // Test O: The IRQ Occurs on the wrong CPU cycle.

    // TODO: This is more precise timing stuff
}

void test_dmc(void) {
    // Setup
    fam_apu_write_register(apu, 0x4012, 0x00); // Sample address $C000
    fam_apu_write_register(apu, 0x4013, 0x01); // Length of 1 * 16 + 1 = 17
    fam_apu_write_register(apu, 0x4010, 0x0F); // Fastest sample rate, disable DMC IRQ
    clock_apu(4000);

    uint8_t status;

    // Test 1: Reading address $4015 should set bit 4 when the DMC is playing and clear bit 4 when the sample ends.
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should be playing by now
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test 1: DMC should be playing (bit 4 set) while the sample is in progress");
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should have stopped by now
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test 1: DMC should have stopped (bit 4 clear) after the sample ends");

    // Test 2: Restarting the DMC should re-load the sample length.
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(4320);
    fam_apu_write_register(apu, 0x4015, 0x00);
    fam_apu_write_register(apu, 0x4015, 0x10); // Restart DMC (The sample length should be reset to 17)
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should still be playing
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test 2: DMC should still be playing after a restart reloads the sample length");
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should have stopped by now
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test 2: DMC should have stopped once the restarted sample finishes");

    // Test 3: Writing $10 to $4015 should start playing a new sample if the previous one ended.
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(8640); // Wait for sample to end
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should be playing
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test 3: writing $10 to $4015 after the previous sample ended should start a new sample (DMC playing)");

    // Test 4: Writing $10 to $4015 while a sample is currently playing shouldn't affect anything.
    clock_apu(4320);
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(4320);
    fam_apu_write_register(apu, 0x4015, 0x10);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should still be playing
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test 4: writing $10 to $4015 while a sample is already playing should not restart it (still playing)");
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should have stopped
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test 4: the sample should still finish normally after the redundant $10 writes (DMC stopped)");

    // Test 5: Writing $00 to $4015 should immediately stop the sample.
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(4320);
    fam_apu_write_register(apu, 0x4015, 0x00);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should have stopped
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test 5: writing $00 to $4015 should immediately stop the sample (bit 4 clear)");

    // Test 6: Writing to $4013 shouldn't change the sample length of the currently playing sample.
    fam_apu_write_register(apu, 0x4015, 0x10); // Start the sample
    fam_apu_write_register(apu, 0x4013, 0x02); // 33 byte sample
    clock_apu(8640);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should have stopped
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test 6: the $4013 write must not extend the currently-playing 17-byte sample - it should still finish (DMC stopped)");
    // Sample length is now 33
    fam_apu_write_register(apu, 0x4015, 0x10);
    fam_apu_write_register(apu, 0x4013, 0x01); // Set sample length back to 17
    clock_apu(12960);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should be playing
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test 6: the now-playing 33-byte sample should still be in progress - the $4013=0x01 write must not shorten it");
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status); // DMC should have stopped
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test 6: the 33-byte sample should have finished by now (DMC stopped)");

    // Test 7: The DMC IRQ flag should not be set when disabled.
    fam_apu_write_register(apu, 0x4015, 0x10); // Start the sample
    clock_apu(8640);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test 7: DMC IRQ flag (bit 7) should not be set when the IRQ is disabled");

    // Test 8: The DMC IRQ flag should be set when enabled, and a sample ends.
    fam_apu_write_register(apu, 0x4010, 0x8F); // Enable IRQ, fastest sample rate
    fam_apu_write_register(apu, 0x4015, 0x10); // Start the sample
    clock_apu(8640);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, status & 0x80, "Test 8: DMC IRQ flag (bit 7) should be set when the IRQ is enabled");

    // Test 9: Reading $4015 should not clear the IRQ flag.
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, status & 0x80, "Test 9: DMC IRQ flag (bit 7) should still be set when status is read again");

    // Test A: Writing to $4015 should clear the IRQ flag.
    fam_apu_write_register(apu, 0x4015, 0x10);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test A: DMC IRQ flag (bit 7) should be cleared when status is written");

    // Test B: Disabling the IRQ flag should clear the IRQ flag.
    fam_apu_write_register(apu, 0x4015, 0x00);
    fam_apu_write_register(apu, 0x4015, 0x10); // Restart sample
    clock_apu(8640); // Wait for interrupt flag to be set
    fam_apu_write_register(apu, 0x4010, 0x0F); // Disable IRQ
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test B: DMC IRQ flag (bit 7) should be cleared when DMC IRQ is disabled");

    // Test C: Looping samples should loop.
    fam_apu_write_register(apu, 0x4010, 0x4F); // Enable loop
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(50000); // Wait for a while...
    fam_apu_read_register(apu, 0x4015, &status); // Should still be playing
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test C: a looping sample should still be playing after a long wait (bit 4 stays set)");
    fam_apu_write_register(apu, 0x4015, 0x00); // Should stop the sample
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test C: writing $00 to $4015 should stop even a looping sample");

    // Test D: Looping samples should not set the IRQ flag when they loop.
    fam_apu_write_register(apu, 0x4010, 0xCF); // Enable loop + IRQ
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(50000);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test D: DMC IRQ flag (bit 7) should not be set by looping samples");
    // NOTE: AccuracyCoin just writes the read status back here, not stopping anything
    // Not sure if it's a bug in the code or a mistaken comment
    fam_apu_write_register(apu, 0x4015, 0x00);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test D: DMC IRQ flag (bit 7) should not be set by looping samples, even when force-stopped");

    // Test E: Clearing the looping flag and then setting it again should keep the sample looping.
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(26352);
    fam_apu_write_register(apu, 0x4010, 0x8F); // Disable loop
    fam_apu_write_register(apu, 0x4010, 0xCF); // Enable loop again
    clock_apu(50000);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test E: clearing then immediately re-setting the loop flag should keep the sample looping (still playing)");

    // Test F: Clearing the looping flag should not immediately end the sample. The sample should then play for its remaining bytes.
    fam_apu_write_register(apu, 0x4015, 0x00);
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(26352);
    fam_apu_write_register(apu, 0x4010, 0x8F); // Disable loop
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test F: DMC IRQ flag (bit 7) should NOT be set yet - the sample is still playing right after the loop flag is cleared");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test F: clearing the loop flag must not immediately end the sample - it should still be playing its remaining bytes");
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, status & 0x80, "Test F: DMC IRQ flag (bit 7) should be set once the sample finishes its remaining bytes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test F: DMC should have stopped (bit 4 clear) after playing out its remaining bytes");

    // Test G: A looping sample should reload the sample length from $4013 every time the sample loops.
    fam_apu_write_register(apu, 0x4010, 0xCF); // Enable loop + IRQ
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(26352);
    fam_apu_write_register(apu, 0x4013, 0x02); // Set sample length to 33
    clock_apu(4320);
    fam_apu_write_register(apu, 0x4010, 0x8F); // Disable loop
    clock_apu(10000);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x80, "Test G: DMC IRQ flag (bit 7) should NOT be set yet - the reloaded sample is still playing after the loop flag is cleared");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10, status & 0x10, "Test G: the reloaded 33-byte sample should still be playing (bit 4 set)");
    clock_apu(4320);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, status & 0x80, "Test G: DMC IRQ flag (bit 7) should be set once the reloaded 33-byte sample finishes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test G: DMC should have stopped (bit 4 clear) after the reloaded 33-byte sample plays out");

    // Test H: Writing $00 to $4013 should result in the following sample being 1 byte long.
    fam_apu_write_register(apu, 0x4010, 0x0F); // Disable IRQ and loop
    fam_apu_write_register(apu, 0x4013, 0x00); // 1-byte sample
    fam_apu_write_register(apu, 0x4015, 0x10);
    clock_apu(1728);
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test H: a $00 write to $4013 makes a 1-byte sample, which should finish almost immediately (DMC stopped)");

    // Test I: There should be a one-byte buffer that's filled immediately if empty.
    fam_apu_write_register(apu, 0x4010, 0x8F); // Enable IRQ, fastest speed
    fam_apu_write_register(apu, 0x4013, 0x01); // 17 byte sample
    fam_apu_write_register(apu, 0x4015, 0x10);
    // Wait until sample ends
    do {
        fam_apu_clock(apu);
        fam_apu_read_register(apu, 0x4015, &status);
        // TODO: Prevent infinite loop somehow
    } while (status & 0x10);
    clock_apu(1758);
    fam_apu_write_register(apu, 0x4013, 0x00); // 1-byte sample
    fam_apu_write_register(apu, 0x4015, 0x10); // Enable DMC again
    fam_apu_read_register(apu, 0x4015, &status);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, status & 0x90, "Test I: after re-enabling, the 1-byte sample should have already ended with its IRQ set (status & 0x90 == 0x80)");
    fam_apu_write_register(apu, 0x4015, 0x10); // Enable DMC again
    fam_apu_read_register(apu, 0x4015, &status); // Should be playing
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(0x00, status, "Test I: re-enabling the DMC should start it playing again (status nonzero)");
    clock_apu(1728);
    fam_apu_read_register(apu, 0x4015, &status); // Now should be stopped
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, status & 0x10, "Test I: the 1-byte sample should have stopped after playing out (bit 4 clear)");

    // Test J: The DMA occurred on the wrong CPU cycle.
    // Test K: The sample address should overflow to $8000 instead of $0000
    // Test L: Writing to $4015 when the DMC timer has 2 cycles until clocked should not trigger a DMC DMA until after the 3 or 4 CPU cycle delay of writing to $4015.
    // Test M: Writing to $4015 when the DMC timer has 1 cycle until clocked should not trigger a DMC DMA until after the 3 or 4 CPU cycle delay of writing to $4015.
    // Test N: Writing to $4015 when the DMC timer has 0 cycles until clocked should not trigger a DMC DMA until after the 3 or 4 CPU cycle delay of writing to $4015.

    // This is more precise timing stuff
}

void setUp(void) {
    fam_apu_init(&apu);
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
    RUN_TEST(test_length_counter_triangle);
    RUN_TEST(test_length_table_triangle);
    RUN_TEST(test_length_counter_noise);
    RUN_TEST(test_length_table_noise);
    RUN_TEST(test_frame_counter_irq);
    RUN_TEST(test_dmc);
    return UNITY_END();
}
