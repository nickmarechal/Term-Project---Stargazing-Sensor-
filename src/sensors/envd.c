/* envd: reads the BME280 and sends its records to stored. */
#include "sensors/daemon.h"

int main(int argc, char **argv)
{
    return sensor_main(&bme_ops, argc, argv);
}
