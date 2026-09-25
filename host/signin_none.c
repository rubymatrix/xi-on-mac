/* No sign-in screen (host/signin.c): for hosts whose own UI signs in and passes --session or --user,
 * as the UWP app (FFXIXbox) does. host64 then says so if it was started with neither. */
#include "signin.h"

int signin_run(const SigninSetup* setup, SigninResult* out)
{
    (void)setup, (void)out;
    return -1;
}
