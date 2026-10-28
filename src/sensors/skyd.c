/* skyd: reads the TSL2591 and sends its records to stored. */
#include "sensors/daemon.h"

int main(int argc, char **argv)
{
    return sensor_main(&tsl_ops, argc, argv);
}
