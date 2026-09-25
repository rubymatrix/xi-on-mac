/* The PlayOnline session behind a handle, for the launcher's Rust side (src-tauri/src/pol.rs), which
 * cannot see PolSession's layout. One handle is one sign-in; use it from one thread. */
#include <stdlib.h>

#include "polsession.h"

PolSession* polffi_new(void)
{
    PolSession* s = (PolSession*)calloc(1, sizeof *s);
    if (s)
        s->irc = POL_NO_SOCKET;
    return s;
}

int polffi_signin(PolSession* s, const char* host, const char* pol_id, const char* password)
{
    return pol_signin(s, host, POL_PORT_IRC, pol_id, password);
}

int polffi_session_value(PolSession* s, uint8_t v[16])
{
    return pol_ffxi_session_value(s, POL_PORT_POLPRO, v);
}

int polffi_pump(PolSession* s, int timeout_ms)
{
    return pol_pump(s, timeout_ms);
}

const char* polffi_error(const PolSession* s)
{
    return s->error;
}

/* Signs out (if signed in) and frees the handle. */
void polffi_free(PolSession* s)
{
    if (!s)
        return;
    pol_signout(s);
    free(s);
}
