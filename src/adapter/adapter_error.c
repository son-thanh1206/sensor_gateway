/* adapter_error.c - reason of the last PARSE_ERROR (see adapters.h) */
#include "adapters.h"

static const char *last_error = "unknown";

void adapter_set_error(const char *reason)
{
    last_error = reason;
}

const char *adapter_last_error(void)
{
    return last_error;
}
