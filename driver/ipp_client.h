#ifndef MINTPRINT_IPP_CLIENT_H
#define MINTPRINT_IPP_CLIENT_H

#include <exec/types.h>
#include "config.h"

struct MPIPPResult {
    LONG error;
    LONG http_status;
    UWORD ipp_status;
    ULONG document_bytes;
    ULONG document_bytes_sent;
};

/* Opens bsdsocket.library V4 and creates a harmless unconnected socket.
 * The dedicated spool Process calls this during driver startup so socket
 * functions never run from an arbitrary printer.device caller Task. */
BOOL mp_ipp_socket_available(void);

/* Query the four IPP media-*-margin-supported values used to build a safe
 * PostScript imageable rectangle. Outputs are hundredths of a millimetre.
 * A missing or conflicting side resolves to zero rather than guessing.
 * Successful results are cached per endpoint for the resident driver. */
LONG mp_ipp_query_imageable_margins(const struct MPConfig *cfg,
                                    ULONG *left_100mm,
                                    ULONG *right_100mm,
                                    ULONG *top_100mm,
                                    ULONG *bottom_100mm);

/* Capture the exact IPP Print-Job operation body (attributes through
 * end-of-attributes, excluding document bytes) next to document_filename as
 * <document_filename>.ipp. No socket is opened. This performs AmigaDOS file
 * I/O and must therefore be called from a Process (MintPrintSettings does so
 * after each regression capture), never from a printer.device callback Task. */
LONG mp_ipp_capture_request(const struct MPConfig *cfg,
                            CONST_STRPTR document_format,
                            CONST_STRPTR document_filename);

LONG mp_ipp_print_document(const struct MPConfig *cfg, CONST_STRPTR filename,
                           CONST_STRPTR document_format,
                           struct MPIPPResult *result);

#endif
