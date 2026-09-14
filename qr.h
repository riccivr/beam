#ifndef BEAM_QR_H
#define BEAM_QR_H

#include <stdio.h>
#include <stdbool.h>

/*
 * Generates and renders a QR code for the given text to fp.
 * Uses UTF-8 half-block characters for compact terminal display.
 * Returns 0 on success, or non-zero if the text is too long or generation fails.
 */
int qr_print_terminal(FILE *fp, const char *text);

#endif /* BEAM_QR_H */
