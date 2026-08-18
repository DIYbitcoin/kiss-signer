// backup rehearsal decisions: see kiss_rehearse.h
#include "kiss_rehearse.h"

#include <string.h>

bool kiss_fp_known(const uint8_t fp[4])
{
    return fp && (fp[0] | fp[1] | fp[2] | fp[3]) != 0;
}

kiss_rehearse_next_t kiss_rehearse_after_words(int no_passphrase)
{
    return no_passphrase ? KISS_REHEARSE_VERIFIED
                         : KISS_REHEARSE_NEED_PASSPHRASE;
}

bool kiss_rehearse_pass_ok(const uint8_t got[4], const uint8_t want[4])
{
    // The known check runs on WANT, not got: want is the session's identity,
    // and a zeroed one means no identity is open -- matching against it would
    // let a failed derivation verify against a wiped session, which is the
    // 00000000 fault wearing a tick.
    return got && kiss_fp_known(want) && memcmp(got, want, 4) == 0;
}
