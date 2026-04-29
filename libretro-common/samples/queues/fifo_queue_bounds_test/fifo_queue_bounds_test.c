/* Regression test for the fifo_queue bounds checks added in
 * libretro-common/queues/fifo_queue.c.
 *
 * Background
 * ----------
 * fifo_write / fifo_read previously trusted @len blindly: passing
 * len > FIFO_WRITE_AVAIL would walk off the end of the ring's
 * backing buffer (the wrap-around copy `memcpy(buffer->buffer,
 * src + first_write, rest_write)` writes rest_write bytes into a
 * size-byte buffer, overrunning by len - size).  Worse, the
 * `buffer->end + len > buffer->size` check itself wraps in size_t
 * for huge @len, mis-routing the caller down a single-memcpy
 * branch with no wrap-around bound at all.  fifo_initialize
 * accepted len == SIZE_MAX, which made `len + 1` wrap to 0, so
 * calloc(1, 0) might return a non-NULL zero-byte buffer and
 * subsequent fifo_write would `% 0` (division by zero) on the
 * end-pointer update.
 *
 * What this test asserts
 * ----------------------
 * 1. fifo_initialize rejects SIZE_MAX (no wrap to zero-byte buf).
 * 2. fifo_write caps @len at FIFO_WRITE_AVAIL: writing more than
 *    available drops the excess silently rather than overrunning
 *    the backing buffer.  ASan/LSan-clean.
 * 3. fifo_read caps @len at FIFO_READ_AVAIL: reading more than
 *    available leaves the trailing portion of @in_buf untouched.
 * 4. The cap survives integer-overflow attempts on @len (very
 *    large @len that would wrap (end + len) to a small value
 *    in size_t arithmetic, which the original code mis-routed).
 * 5. Wrap-around writes/reads still work correctly when the cap
 *    isn't engaged.
 *
 * Build under -fsanitize=address,undefined to catch any future
 * regression that re-introduces the OOB write or the SIZE_MAX
 * wrap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <queues/fifo_queue.h>

static int failures = 0;

#define EXPECT(cond, fmt, ...) do {                              \
   if (!(cond)) {                                                \
      fprintf(stderr, "[FAIL] %s:%d: " fmt "\n",                 \
            __func__, __LINE__, ##__VA_ARGS__);                  \
      failures++;                                                \
   }                                                             \
} while (0)

/* Test 1: SIZE_MAX is rejected.  Without the guard, len + 1
 * would wrap to 0 and the buffer would be unusable. */
static void test_initialize_size_max(void)
{
   fifo_buffer_t buf;

   EXPECT(!fifo_initialize(&buf, SIZE_MAX),
         "SIZE_MAX should be rejected (would wrap len + 1)");
   /* If it incorrectly succeeded we'd leak; we asserted failure
    * so no buffer was allocated. */
   printf("[PASS] initialize_size_max\n");
}

/* Test 2: Normal init still works. */
static void test_initialize_normal(void)
{
   fifo_buffer_t buf;

   EXPECT(fifo_initialize(&buf, 256), "normal init should succeed");
   /* size is len + 1 (one slot reserved for empty/full) */
   /* Available bytes for writing == len */
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 256,
         "fresh buffer should have 256 bytes available, got %zu",
         FIFO_WRITE_AVAIL(&buf));
   EXPECT(FIFO_READ_AVAIL(&buf) == 0,
         "fresh buffer should have nothing to read, got %zu",
         FIFO_READ_AVAIL(&buf));
   fifo_deinitialize(&buf);
   printf("[PASS] initialize_normal\n");
}

/* Test 3: write cap.  Pass more than available; the overrun
 * should be silently truncated rather than corrupting memory.
 * If ASan is enabled, an OOB write would trip it. */
static void test_write_capped(void)
{
   fifo_buffer_t buf;
   uint8_t       payload[2048];
   size_t        i;

   for (i = 0; i < sizeof(payload); i++)
      payload[i] = (uint8_t)(i & 0xff);

   EXPECT(fifo_initialize(&buf, 100), "init");
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 100, "100 avail");

   /* Try to write 2048 into a 100-byte ring. */
   fifo_write(&buf, payload, sizeof(payload));

   EXPECT(FIFO_READ_AVAIL(&buf) == 100,
         "after over-write, read avail should be 100, got %zu",
         FIFO_READ_AVAIL(&buf));
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 0,
         "after over-write, write avail should be 0, got %zu",
         FIFO_WRITE_AVAIL(&buf));

   fifo_deinitialize(&buf);
   printf("[PASS] write_capped\n");
}

/* Test 4: read cap.  Try to read more than available; the
 * over-read should be capped at FIFO_READ_AVAIL and the trailing
 * portion of the destination buffer should remain untouched. */
static void test_read_capped(void)
{
   fifo_buffer_t buf;
   const char   *msg = "hello";
   uint8_t       out[64];

   EXPECT(fifo_initialize(&buf, 256), "init");
   fifo_write(&buf, msg, 5);
   EXPECT(FIFO_READ_AVAIL(&buf) == 5, "5 readable");

   memset(out, 0xaa, sizeof(out));
   /* Ask for more than available. */
   fifo_read(&buf, out, 20);

   EXPECT(memcmp(out, msg, 5) == 0,
         "first 5 bytes should be 'hello'");
   /* The cap means only 5 were actually written into out; the
    * rest stays at the 0xaa sentinel.  This is the documented
    * post-cap behaviour. */
   EXPECT(out[5] == 0xaa,
         "byte after capped read should be untouched (was 0x%02x)",
         out[5]);
   EXPECT(out[19] == 0xaa,
         "trailing bytes should be untouched (was 0x%02x)",
         out[19]);

   EXPECT(FIFO_READ_AVAIL(&buf) == 0,
         "after capped read, read avail should be 0, got %zu",
         FIFO_READ_AVAIL(&buf));

   fifo_deinitialize(&buf);
   printf("[PASS] read_capped\n");
}

/* Test 5: huge @len that would have wrapped (end + len) in
 * size_t.  The original code's `buffer->end + len > buffer->size`
 * misclassifies this as fitting in one chunk, taking the
 * single-memcpy path with len bytes of OOB write into the ring.
 * The cap reduces len to FIFO_WRITE_AVAIL before any memcpy.
 *
 * Note: fifo_write reads exactly @len bytes from @in_buf -- the
 * cap only protects the destination ring, not the source buffer.
 * Callers must always supply a source buffer of at least @len
 * bytes (or now, after the cap, at least FIFO_WRITE_AVAIL bytes).
 * For this test we therefore use a source buffer big enough to
 * cover the post-cap copy (which will be 99 bytes here). */
static void test_write_size_max_len(void)
{
   fifo_buffer_t buf;
   uint8_t       byte = 0x42;
   uint8_t       big_src[256];

   memset(big_src, 0xcd, sizeof(big_src));

   EXPECT(fifo_initialize(&buf, 100), "init");
   /* Set end != 0 so end + SIZE_MAX would wrap to a small value: */
   fifo_write(&buf, &byte, 1);
   /* Now end == 1.  Pass SIZE_MAX as len; without the cap, the
    * (end + len) addition wraps to 0 (for end=1, len=SIZE_MAX),
    * the comparison "> size" is false, and the function would
    * memcpy SIZE_MAX bytes from big_src into buffer->buffer + 1
    * -- a destination overrun of essentially the entire address
    * space.  With the cap, len becomes FIFO_WRITE_AVAIL = 99
    * and the write completes safely. */
   fifo_write(&buf, big_src, SIZE_MAX);

   /* Buffer should be full now. */
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 0,
         "after SIZE_MAX write, write avail should be 0, got %zu",
         FIFO_WRITE_AVAIL(&buf));
   EXPECT(FIFO_READ_AVAIL(&buf) == 100,
         "after SIZE_MAX write, read avail should be 100, got %zu",
         FIFO_READ_AVAIL(&buf));

   fifo_deinitialize(&buf);
   printf("[PASS] write_size_max_len\n");
}

/* Test 6: wrap-around writes still work when not engaging the
 * cap.  Write to fill, read half, write half: the wrap-around
 * branch in fifo_write should produce the right contents. */
static void test_wrap_around(void)
{
   fifo_buffer_t buf;
   uint8_t       in[10] = {0,1,2,3,4,5,6,7,8,9};
   uint8_t       out[10];

   EXPECT(fifo_initialize(&buf, 10), "init"); /* 10 usable */

   /* Fill it. */
   fifo_write(&buf, in, 10);
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 0, "full");

   /* Drain half. */
   fifo_read(&buf, out, 5);
   EXPECT(memcmp(out, in, 5) == 0, "first 5 bytes");
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 5, "5 free");
   EXPECT(FIFO_READ_AVAIL(&buf) == 5, "5 used");

   /* Write 5 more — engages the wrap-around branch. */
   {
      uint8_t more[5] = {10,11,12,13,14};
      fifo_write(&buf, more, 5);
   }
   EXPECT(FIFO_WRITE_AVAIL(&buf) == 0, "full again");

   /* Read all 10 — also engages wrap-around branch. */
   memset(out, 0, sizeof(out));
   fifo_read(&buf, out, 10);
   EXPECT(out[0] == 5,  "out[0]=5, got %u", out[0]);
   EXPECT(out[4] == 9,  "out[4]=9, got %u", out[4]);
   EXPECT(out[5] == 10, "out[5]=10, got %u", out[5]);
   EXPECT(out[9] == 14, "out[9]=14, got %u", out[9]);

   fifo_deinitialize(&buf);
   printf("[PASS] wrap_around\n");
}

/* Test 7: zero-len write/read should be a defined no-op. */
static void test_zero_len(void)
{
   fifo_buffer_t buf;
   uint8_t       byte;

   EXPECT(fifo_initialize(&buf, 32), "init");

   /* Zero-len write on empty buffer. */
   fifo_write(&buf, &byte, 0);
   EXPECT(FIFO_READ_AVAIL(&buf) == 0, "still empty");

   /* Zero-len read on empty buffer. */
   fifo_read(&buf, &byte, 0);
   EXPECT(FIFO_READ_AVAIL(&buf) == 0, "still empty");

   /* Zero-len read on non-empty buffer. */
   fifo_write(&buf, "x", 1);
   fifo_read(&buf, &byte, 0);
   EXPECT(FIFO_READ_AVAIL(&buf) == 1, "still 1 byte");

   fifo_deinitialize(&buf);
   printf("[PASS] zero_len\n");
}

int main(void)
{
   test_initialize_size_max();
   test_initialize_normal();
   test_write_capped();
   test_read_capped();
   test_write_size_max_len();
   test_wrap_around();
   test_zero_len();

   if (failures)
   {
      fprintf(stderr, "\n%d fifo_queue test(s) failed\n", failures);
      return 1;
   }
   printf("\nAll fifo_queue bounds tests passed.\n");
   return 0;
}
