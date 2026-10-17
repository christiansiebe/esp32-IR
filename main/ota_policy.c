#include "ota_policy.h"

#include <string.h>

bool ota_url_is_trusted_policy(const char *url)
{
    /* https only: an image pulled over cleartext HTTP could be swapped on the
     * wire and brick the device. The sandbox front door presents a Let's
     * Encrypt certificate that the ESP certificate bundle already trusts. */
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

bool ota_ranges_disjoint(uint32_t a_start, uint32_t a_size,
                         uint32_t b_start, uint32_t b_size)
{
    if (a_size == 0 || b_size == 0) {
        return true;
    }
    uint32_t a_end = a_start + a_size;
    uint32_t b_end = b_start + b_size;
    return a_start >= b_end || b_start >= a_end;
}

bool ota_target_is_safe_range(uint32_t target_start, uint32_t target_size,
                              uint32_t store_start, uint32_t store_size)
{
    /* No measurement partition configured: nothing to protect. */
    if (store_size == 0) {
        return true;
    }
    return ota_ranges_disjoint(target_start, target_size, store_start, store_size);
}
