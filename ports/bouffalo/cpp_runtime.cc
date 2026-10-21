// The SDK uses -nostartfiles, so GCC's crtbegin does not supply this token.
// This firmware is a single image with no dynamically unloaded libraries.
extern "C" {
void* __dso_handle = &__dso_handle;
}
