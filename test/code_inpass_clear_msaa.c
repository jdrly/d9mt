/* code_inpass_clear.c into a 4x multisampled render target and a 4x D24S8 surface, resolved into
 * the back buffer with StretchRect while the pass that holds the in-pass clears is still open. */
#define IPC_MSAA 1
#define IPC_NAME "d9mt_inpass_clear_msaa"
#include "code_inpass_clear.c"
