#include "check.h"

#include <stdio.h>

int check_count;
int check_failures;

int main(void)
{
    /* Each step adds its suite here. */

    printf("%d checks, %d failed\n", check_count, check_failures);
    return check_failures == 0 ? 0 : 1;
}
