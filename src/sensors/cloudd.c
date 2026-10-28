/* cloudd: reads the MLX90614 and sends its records to stored. */
#include "sensors/daemon.h"

int main(int argc, char **argv)
{
    return sensor_main(&mlx_ops, argc, argv);
}
