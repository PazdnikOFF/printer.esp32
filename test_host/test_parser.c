/*
 * Host-side parser unit test (ТЗ §13.3).
 *
 * Build (from project root):
 *   gcc -std=c11 -Wall -I test_host -I components/sony898_emulator \
 *       components/sony898_emulator/sony898_image.c                   \
 *       components/sony898_emulator/sony898_status.c                  \
 *       components/sony898_emulator/sony898_parser.c                  \
 *       test_host/test_parser.c                                        \
 *       -o test_host/test_parser
 *
 * Run (provide captured print job):
 *   ./test_host/test_parser sony898_spdl.bin
 *
 * Expected output per ТЗ §13.3:
 *   found JOBSIZE=PJL-H
 *   found @PJL ENTER LANGUAGE=SONY-PDL-DS2
 *   found JOBSIZE=PDL,1229097
 *   PDL header size = 290
 *   width = 1280
 *   height = 960
 *   image payload = 1228800 bytes
 *   footer = 7 bytes
 *   found JOBSIZE=PJL-T,302
 *   found @PJL EOJ
 *   PASS: PDL length check 1229097 == 290 + 1280*960 + 7
 *   PASS: image ready
 *   PGM written to out.pgm
 */

#include "mock_esp.h"
#include "sony898_image.h"
#include "sony898_status.h"
#include "sony898_parser.h"

#include <stdio.h>
#include <string.h>

#define CHUNK_SIZE 512u

static int failures = 0;

#define EXPECT_EQ(a, b, msg) do {                                           \
    if ((a) != (b)) {                                                       \
        fprintf(stderr, "FAIL: %s — got %lld expected %lld\n",             \
                (msg), (long long)(a), (long long)(b));                     \
        failures++;                                                          \
    } else {                                                                \
        printf("PASS: %s\n", (msg));                                        \
    }                                                                       \
} while(0)

#define EXPECT_TRUE(cond, msg) do {                                         \
    if (!(cond)) {                                                          \
        fprintf(stderr, "FAIL: %s\n", (msg));                              \
        failures++;                                                          \
    } else {                                                                \
        printf("PASS: %s\n", (msg));                                        \
    }                                                                       \
} while(0)

static void write_pgm(const char *path) {
    if (!sony898_image_ready()) {
        fprintf(stderr, "FAIL: image not ready, cannot write PGM\n");
        failures++;
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen out.pgm"); failures++; return; }

    uint16_t w = sony898_get_width();
    uint16_t h = sony898_get_height();
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(sony898_get_image_buffer(), 1, sony898_get_image_size(), f);
    fclose(f);
    printf("PGM written to %s (%"PRIu16"x%"PRIu16" grayscale)\n", path, w, h);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <sony898_spdl.bin>\n", argv[0]);
        return 2;
    }

    /* Init modules */
    sony898_image_init();
    sony898_parser_init();

    /* Feed the binary file to the parser in 512-byte chunks */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    uint8_t buf[CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        esp_err_t r = sony898_parser_feed(buf, n);
        if (r != ESP_OK) {
            fprintf(stderr, "FAIL: parser_feed returned error\n");
            failures++;
            break;
        }
    }
    fclose(f);

    /* ── Verify results (ТЗ §13.3) ─────────────────────────────────────── */

    EXPECT_TRUE(sony898_image_ready(),
                "image_ready after parsing sony898_spdl.bin");

    EXPECT_EQ(sony898_get_width(),  1280,
              "width = 1280");

    EXPECT_EQ(sony898_get_height(), 960,
              "height = 960");

    uint32_t expected_image_bytes = 1280u * 960u;   /* = 1,228,800 */
    EXPECT_EQ(sony898_get_image_size(), expected_image_bytes,
              "image payload = 1228800 bytes");

    /* We infer pdl_len from the parsed dimensions */
    uint32_t computed_pdl = 290u + (uint32_t)sony898_get_width() *
                                   sony898_get_height() + 7u;
    EXPECT_EQ(computed_pdl, 1229097u,
              "PDL length check 1229097 == 290 + 1280*960 + 7");

    /* PSRAM check: on the host mock this always passes */
    EXPECT_TRUE(sony898_get_image_buffer() != NULL,
                "image buffer allocated");

    /* Write PGM for visual verification */
    write_pgm("out.pgm");

    /* ── Robustness tests ────────────────────────────────────────────────── */

    printf("\n--- Robustness tests ---\n");

    /* Wrong JOBSIZE prefix */
    sony898_parser_reset();
    {
        const uint8_t bad[256] = { 'B','A','D','D','A','T','A' };
        esp_err_t r = sony898_parser_feed(bad, 256);
        EXPECT_EQ(r, ESP_FAIL, "reject bad JOBSIZE prefix");
    }

    /* Zero dimensions (inject a fake PDL header with w=0, h=0) */
    sony898_parser_reset();
    {
        /* Build a minimal fake job with w=0 h=0 */
        uint8_t fake_jsize[256] = {0};
        snprintf((char *)fake_jsize, 256, "JOBSIZE=PJL-H,10,0,6,0,0,0");
        sony898_parser_feed(fake_jsize, 256);

        uint8_t pjl_hdr[10] = {0};
        sony898_parser_feed(pjl_hdr, 10);

        memset(fake_jsize, 0, 256);
        /* pdl_len that would match 0x0 image = 290 + 0 + 7 = 297 */
        snprintf((char *)fake_jsize, 256, "JOBSIZE=PDL,297");
        sony898_parser_feed(fake_jsize, 256);

        uint8_t pdl_hdr[290] = {0};
        /* w=0, h=0 at offset 0x24 */
        sony898_parser_feed(pdl_hdr, 290);
        EXPECT_EQ(sony898_parser_get_state(), PARSER_ERROR,
                  "reject PDL with zero dimensions");
    }

    /* Image too large */
    sony898_parser_reset();
    {
        uint8_t fake_jsize[256] = {0};
        snprintf((char *)fake_jsize, 256, "JOBSIZE=PJL-H,1");
        sony898_parser_feed(fake_jsize, 256);
        uint8_t dummy[1] = {0};
        sony898_parser_feed(dummy, 1);

        memset(fake_jsize, 0, 256);
        /* 3000x3000 = 9M > MAX_IMAGE_BYTES (6M) */
        uint32_t big_pdl = 290 + 3000u*3000u + 7;
        snprintf((char *)fake_jsize, 256, "JOBSIZE=PDL,%"PRIu32, big_pdl);
        sony898_parser_feed(fake_jsize, 256);

        uint8_t pdl_hdr[290] = {0};
        /* w=3000 h=3000 at offset 0x24 */
        pdl_hdr[0x24] = 0x0B; pdl_hdr[0x25] = 0xB8; /* 3000 = 0x0BB8 */
        pdl_hdr[0x26] = 0x0B; pdl_hdr[0x27] = 0xB8;
        sony898_parser_feed(pdl_hdr, 290);
        EXPECT_EQ(sony898_parser_get_state(), PARSER_ERROR,
                  "reject image larger than MAX_IMAGE_BYTES");
    }

    /* PDL length mismatch */
    sony898_parser_reset();
    {
        uint8_t fake_jsize[256] = {0};
        snprintf((char *)fake_jsize, 256, "JOBSIZE=PJL-H,1");
        sony898_parser_feed(fake_jsize, 256);
        uint8_t dummy[1] = {0};
        sony898_parser_feed(dummy, 1);

        memset(fake_jsize, 0, 256);
        snprintf((char *)fake_jsize, 256, "JOBSIZE=PDL,9999");
        sony898_parser_feed(fake_jsize, 256);

        uint8_t pdl_hdr[290] = {0};
        pdl_hdr[0x24] = 0x05; pdl_hdr[0x25] = 0x00; /* w=1280 */
        pdl_hdr[0x26] = 0x03; pdl_hdr[0x27] = 0xC0; /* h=960  */
        sony898_parser_feed(pdl_hdr, 290);
        EXPECT_EQ(sony898_parser_get_state(), PARSER_ERROR,
                  "reject PDL length mismatch");
    }

    /* Second job after first (reset + reparse) */
    sony898_release_image();
    sony898_parser_reset();
    EXPECT_TRUE(sony898_parser_can_accept_job(),
                "parser accepts second job after reset");

    printf("\n--- Summary ---\n");
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
}
