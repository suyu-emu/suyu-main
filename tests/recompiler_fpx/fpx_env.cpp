// L6(d): the host's own FP-environment check, as ArmRecomp runs it on entry to
// generated code once FPX1 is negotiated.
#include "core/arm/recomp/guest_fp_env.h"

extern "C" int fpx_env_shim(void) {
    return Core::RecompFpEnv::Ensure() ? 1 : 0;
}
