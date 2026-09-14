#include "qr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_VERSION 10
#define MAX_MODULES (17 + 4 * MAX_VERSION)

typedef struct {
    int version;
    int total_bytes;
    int data_bytes;
    int ec_bytes_per_block;
    int num_blocks_g1;
    int data_bytes_g1;
    int num_blocks_g2;
    int data_bytes_g2;
    int align_count;
    int align_coords[7];
} QRVersionInfo;

static const QRVersionInfo VINFO[MAX_VERSION + 1] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, {0}},
    {1,  26,  19,  7, 1,  19, 0,   0, 0, {0}},
    {2,  44,  34, 10, 1,  34, 0,   0, 2, {6, 18}},
    {3,  70,  55, 15, 1,  55, 0,   0, 2, {6, 22}},
    {4, 100,  80, 20, 1,  80, 0,   0, 2, {6, 26}},
    {5, 134, 108, 26, 1, 108, 0,   0, 2, {6, 30}},
    {6, 172, 136, 18, 2,  68, 0,   0, 2, {6, 34}},
    {7, 196, 156, 20, 2,  78, 0,   0, 3, {6, 22, 38}},
    {8, 242, 194, 24, 2,  97, 0,   0, 3, {6, 24, 42}},
    {9, 292, 232, 30, 2, 116, 0,   0, 3, {6, 26, 46}},
    {10, 346, 274, 18, 2, 68, 2,  69, 3, {6, 28, 50}}
};

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_initialized = 0;

static void gf_init(void) {
    if (gf_initialized) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_exp[i + 255] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    gf_log[0] = 0;
    gf_initialized = 1;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[(gf_log[a] + gf_log[b]) % 255];
}

static void rs_generator_poly(int nsym, uint8_t *gen) {
    memset(gen, 0, nsym + 1);
    gen[0] = 1;
    for (int i = 0; i < nsym; i++) {
        uint8_t factor = gf_exp[i];
        for (int j = i + 1; j > 0; j--) {
            gen[j] = gen[j - 1] ^ gf_mul(gen[j], factor);
        }
        gen[0] = gf_mul(gen[0], factor);
    }
}

static void rs_encode(const uint8_t *data, int data_len, int ec_len, uint8_t *ec_out) {
    uint8_t gen[64];
    rs_generator_poly(ec_len, gen);

    memset(ec_out, 0, ec_len);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = data[i] ^ ec_out[0];
        memmove(&ec_out[0], &ec_out[1], ec_len - 1);
        ec_out[ec_len - 1] = 0;
        if (factor != 0) {
            for (int j = 0; j < ec_len; j++) {
                ec_out[j] ^= gf_mul(gen[ec_len - 1 - j], factor);
            }
        }
    }
}

/* Bit buffer helper */
typedef struct {
    uint8_t buf[512];
    int bit_len;
} BitBuffer;

static void bb_init(BitBuffer *bb) {
    memset(bb->buf, 0, sizeof(bb->buf));
    bb->bit_len = 0;
}

static void bb_append(BitBuffer *bb, uint32_t val, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        int bit = (val >> i) & 1;
        int byte_idx = bb->bit_len / 8;
        int bit_idx = 7 - (bb->bit_len % 8);
        if (bit) {
            bb->buf[byte_idx] |= (uint8_t)(1 << bit_idx);
        }
        bb->bit_len++;
    }
}

/* Format info BCH(15, 5) */
static uint16_t get_format_bits(int ec_level_bits, int mask) {
    uint32_t data = (ec_level_bits << 3) | mask;
    uint32_t rem = data << 10;
    for (int i = 14; i >= 10; i--) {
        if (rem & (1 << i)) {
            rem ^= (0x537 << (i - 10));
        }
    }
    return (uint16_t)(((data << 10) | rem) ^ 0x5412);
}

typedef struct {
    int size;
    int8_t modules[MAX_MODULES][MAX_MODULES];   /* 0: white, 1: black, -1: unassigned */
    uint8_t is_func[MAX_MODULES][MAX_MODULES];  /* 1: functional pattern (don't mask or overwrite) */
} QRMatrix;

static void set_func_module(QRMatrix *m, int r, int c, int val) {
    m->modules[r][c] = (int8_t)val;
    m->is_func[r][c] = 1;
}

static void add_finder_pattern(QRMatrix *m, int top_r, int left_c) {
    for (int r = -1; r <= 7; r++) {
        for (int c = -1; c <= 7; c++) {
            int mr = top_r + r;
            int mc = left_c + c;
            if (mr < 0 || mr >= m->size || mc < 0 || mc >= m->size) continue;
            int val = 0;
            if (r >= 0 && r <= 6 && c >= 0 && c <= 6) {
                if (r == 0 || r == 6 || c == 0 || c == 6 || (r >= 2 && r <= 4 && c >= 2 && c <= 4)) {
                    val = 1;
                } else {
                    val = 0;
                }
            }
            set_func_module(m, mr, mc, val);
        }
    }
}

static void add_alignment_pattern(QRMatrix *m, int center_r, int center_c) {
    /* Don't overwrite existing functional modules (e.g. finders) */
    if (m->is_func[center_r][center_c]) return;

    for (int r = -2; r <= 2; r++) {
        for (int c = -2; c <= 2; c++) {
            int mr = center_r + r;
            int mc = center_c + c;
            int val = (abs(r) == 2 || abs(c) == 2 || (r == 0 && c == 0)) ? 1 : 0;
            set_func_module(m, mr, mc, val);
        }
    }
}

static void init_matrix(QRMatrix *m, int version) {
    m->size = 17 + 4 * version;
    for (int r = 0; r < m->size; r++) {
        for (int c = 0; c < m->size; c++) {
            m->modules[r][c] = -1;
            m->is_func[r][c] = 0;
        }
    }

    /* Finders */
    add_finder_pattern(m, 0, 0);
    add_finder_pattern(m, 0, m->size - 7);
    add_finder_pattern(m, m->size - 7, 0);

    /* Timing patterns */
    for (int i = 8; i < m->size - 8; i++) {
        set_func_module(m, 6, i, (i % 2 == 0) ? 1 : 0);
        set_func_module(m, i, 6, (i % 2 == 0) ? 1 : 0);
    }

    /* Dark module */
    set_func_module(m, 4 * version + 9, 8, 1);

    /* Alignment patterns */
    const QRVersionInfo *vi = &VINFO[version];
    if (vi->align_count > 0) {
        for (int i = 0; i < vi->align_count; i++) {
            for (int j = 0; j < vi->align_count; j++) {
                add_alignment_pattern(m, vi->align_coords[i], vi->align_coords[j]);
            }
        }
    }

    /* Reserve Format Info areas */
    for (int i = 0; i <= 8; i++) {
        if (i != 6) {
            set_func_module(m, 8, i, 0);
            set_func_module(m, i, 8, 0);
        }
    }
    for (int i = m->size - 8; i < m->size; i++) {
        set_func_module(m, 8, i, 0);
        set_func_module(m, i, 8, 0);
    }
}

static int mask_condition(int mask, int r, int c) {
    switch (mask) {
        case 0: return (r + c) % 2 == 0;
        case 1: return r % 2 == 0;
        case 2: return c % 3 == 0;
        case 3: return (r + c) % 3 == 0;
        case 4: return (r / 2 + c / 3) % 2 == 0;
        case 5: return ((r * c) % 2) + ((r * c) % 3) == 0;
        case 6: return (((r * c) % 2) + ((r * c) % 3)) % 2 == 0;
        case 7: return (((r + c) % 2) + ((r * c) % 3)) % 2 == 0;
        default: return 0;
    }
}

static void place_data(QRMatrix *m, const uint8_t *data, int total_bytes, int mask) {
    int bit_idx = 0;
    int total_bits = total_bytes * 8;
    int upward = 1;

    for (int right = m->size - 1; right > 0; right -= 2) {
        if (right == 6) right--; /* Skip vertical timing column */

        int r = upward ? (m->size - 1) : 0;
        while (r >= 0 && r < m->size) {
            for (int col_offset = 0; col_offset < 2; col_offset++) {
                int c = right - col_offset;
                if (!m->is_func[r][c]) {
                    int bit = (bit_idx < total_bits) ? ((data[bit_idx / 8] >> (7 - (bit_idx % 8))) & 1) : 0;
                    bit_idx++;

                    /* Apply mask */
                    if (mask_condition(mask, r, c)) {
                        bit ^= 1;
                    }
                    m->modules[r][c] = (int8_t)bit;
                }
            }
            r += upward ? -1 : 1;
        }
        upward = !upward;
    }
}

static void apply_format_info(QRMatrix *m, int mask) {
    /* EC Level L is 01 */
    uint16_t fmt = get_format_bits(1, mask);

    /* Around top-left */
    for (int i = 0; i <= 5; i++) {
        m->modules[8][i] = (int8_t)((fmt >> (14 - i)) & 1);
    }
    m->modules[8][7] = (int8_t)((fmt >> 8) & 1);
    m->modules[8][8] = (int8_t)((fmt >> 7) & 1);
    m->modules[7][8] = (int8_t)((fmt >> 6) & 1);
    for (int i = 0; i <= 5; i++) {
        m->modules[5 - i][8] = (int8_t)((fmt >> i) & 1);
    }

    /* Around top-right and bottom-left */
    for (int i = 0; i < 8; i++) {
        m->modules[8][m->size - 1 - i] = (int8_t)((fmt >> i) & 1);
    }
    for (int i = 0; i < 7; i++) {
        m->modules[m->size - 7 + i][8] = (int8_t)((fmt >> (14 - i)) & 1);
    }
}

/* Penalty calculation according to ISO/IEC 18004 section 6.8.2 */
static int evaluate_penalty(const QRMatrix *m) {
    int penalty = 0;
    int size = m->size;

    /* N1: 5+ consecutive modules of same color in row/column */
    for (int r = 0; r < size; r++) {
        int count = 1;
        for (int c = 1; c < size; c++) {
            if (m->modules[r][c] == m->modules[r][c - 1]) {
                count++;
            } else {
                if (count >= 5) penalty += 3 + (count - 5);
                count = 1;
            }
        }
        if (count >= 5) penalty += 3 + (count - 5);
    }
    for (int c = 0; c < size; c++) {
        int count = 1;
        for (int r = 1; r < size; r++) {
            if (m->modules[r][c] == m->modules[r - 1][c]) {
                count++;
            } else {
                if (count >= 5) penalty += 3 + (count - 5);
                count = 1;
            }
        }
        if (count >= 5) penalty += 3 + (count - 5);
    }

    /* N2: 2x2 blocks of same color */
    for (int r = 0; r < size - 1; r++) {
        for (int c = 0; c < size - 1; c++) {
            int val = m->modules[r][c];
            if (val == m->modules[r + 1][c] &&
                val == m->modules[r][c + 1] &&
                val == m->modules[r + 1][c + 1]) {
                penalty += 3;
            }
        }
    }

    return penalty;
}

int qr_print_terminal(FILE *fp, const char *text) {
    gf_init();

    int len = (int)strlen(text);
    if (len == 0) return -1;

    /* Find smallest version that fits data (Level L) */
    int version = 0;
    for (int v = 1; v <= MAX_VERSION; v++) {
        /* Byte mode header: 4 bits mode + 8 bits len for v <= 9 (16 bits for v >= 10) */
        int header_bits = (v >= 10) ? (4 + 16) : (4 + 8);
        int total_data_bits = header_bits + len * 8;
        int max_data_bits = VINFO[v].data_bytes * 8;
        if (total_data_bits <= max_data_bits) {
            version = v;
            break;
        }
    }

    if (version == 0) {
        return -1; /* Text too long for up to Version 10 */
    }

    const QRVersionInfo *vi = &VINFO[version];

    /* 1. Data encoding in Byte Mode */
    BitBuffer bb;
    bb_init(&bb);
    bb_append(&bb, 0x4, 4); /* Byte mode indicator: 0100 */
    bb_append(&bb, (uint32_t)len, (version >= 10) ? 16 : 8);
    for (int i = 0; i < len; i++) {
        bb_append(&bb, (uint8_t)text[i], 8);
    }

    /* Terminator */
    int max_data_bits = vi->data_bytes * 8;
    int term_bits = max_data_bits - bb.bit_len;
    if (term_bits > 4) term_bits = 4;
    if (term_bits > 0) bb_append(&bb, 0, term_bits);

    /* Byte padding to 8-bit boundary */
    if (bb.bit_len % 8 != 0) {
        bb_append(&bb, 0, 8 - (bb.bit_len % 8));
    }

    /* Pad bytes alternating 0xEC and 0x11 */
    int pad = 0;
    while (bb.bit_len < max_data_bits) {
        bb_append(&bb, (pad % 2 == 0) ? 0xEC : 0x11, 8);
        pad++;
    }

    /* 2. Error Correction & Interleaving */
    int total_blocks = vi->num_blocks_g1 + vi->num_blocks_g2;
    uint8_t data_blocks[8][128];
    uint8_t ec_blocks[8][64];
    int block_data_lens[8];

    int offset = 0;
    for (int b = 0; b < vi->num_blocks_g1; b++) {
        block_data_lens[b] = vi->data_bytes_g1;
        memcpy(data_blocks[b], &bb.buf[offset], vi->data_bytes_g1);
        offset += vi->data_bytes_g1;
        rs_encode(data_blocks[b], vi->data_bytes_g1, vi->ec_bytes_per_block, ec_blocks[b]);
    }
    for (int b = 0; b < vi->num_blocks_g2; b++) {
        int idx = vi->num_blocks_g1 + b;
        block_data_lens[idx] = vi->data_bytes_g2;
        memcpy(data_blocks[idx], &bb.buf[offset], vi->data_bytes_g2);
        offset += vi->data_bytes_g2;
        rs_encode(data_blocks[idx], vi->data_bytes_g2, vi->ec_bytes_per_block, ec_blocks[idx]);
    }

    /* Interleave data */
    uint8_t final_stream[512];
    int fs_len = 0;
    int max_block_len = vi->data_bytes_g1 > vi->data_bytes_g2 ? vi->data_bytes_g1 : vi->data_bytes_g2;

    for (int i = 0; i < max_block_len; i++) {
        for (int b = 0; b < total_blocks; b++) {
            if (i < block_data_lens[b]) {
                final_stream[fs_len++] = data_blocks[b][i];
            }
        }
    }

    /* Interleave EC */
    for (int i = 0; i < vi->ec_bytes_per_block; i++) {
        for (int b = 0; b < total_blocks; b++) {
            final_stream[fs_len++] = ec_blocks[b][i];
        }
    }

    /* 3. Pick best mask pattern (0..7) */
    QRMatrix best_matrix;
    memset(&best_matrix, 0, sizeof(best_matrix));
    int best_penalty = 10000000;

    for (int mask = 0; mask < 8; mask++) {
        QRMatrix m;
        init_matrix(&m, version);
        place_data(&m, final_stream, fs_len, mask);
        apply_format_info(&m, mask);
        int penalty = evaluate_penalty(&m);
        if (penalty < best_penalty) {
            best_penalty = penalty;
            best_matrix = m;
        }
    }

    /* 4. Render to terminal with quiet zone using Unicode half-blocks */
    int size = best_matrix.size;
    int border = 2; /* Quiet zone border modules */

    /*
     * We print inverted colors: \033[47;30m (white bg, black fg)
     * A black QR module (1) will be drawn as dark; white module (0) as light.
     * Top half = r, Bottom half = r+1.
     * - Top=0, Bottom=0: ' ' (white space)
     * - Top=1, Bottom=1: '█' (full black block)
     * - Top=1, Bottom=0: '▀' (upper black half block)
     * - Top=0, Bottom=1: '▄' (lower black half block)
     */
    for (int r = -border; r < size + border; r += 2) {
        fprintf(fp, "  \033[47;30m");
        for (int c = -border; c < size + border; c++) {
            int top = 0;
            int bot = 0;

            if (r >= 0 && r < size && c >= 0 && c < size) {
                top = best_matrix.modules[r][c];
            }
            if ((r + 1) >= 0 && (r + 1) < size && c >= 0 && c < size) {
                bot = best_matrix.modules[r + 1][c];
            }

            if (top && bot) {
                fprintf(fp, "\xe2\x96\x88"); /* █ full block */
            } else if (top && !bot) {
                fprintf(fp, "\xe2\x96\x80"); /* ▀ top half */
            } else if (!top && bot) {
                fprintf(fp, "\xe2\x96\x84"); /* ▄ bottom half */
            } else {
                fprintf(fp, " ");           /* empty space */
            }
        }
        fprintf(fp, "\033[0m\n");
    }

    return 0;
}
