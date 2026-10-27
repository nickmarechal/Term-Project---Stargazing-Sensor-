#include "check.h"
#include "suites.h"

#include <stdio.h>

int check_count;
int check_failures;

int main(void)
{
    test_crc32();
    test_record();
    test_store();

    printf("%d checks, %d failed\n", check_count, check_failures);
    return check_failures == 0 ? 0 : 1;
}
