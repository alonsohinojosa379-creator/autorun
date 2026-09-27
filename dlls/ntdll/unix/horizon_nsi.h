#ifndef WINE_HORIZON_NSI_H
#define WINE_HORIZON_NSI_H

int horizon_nsi_device_name( const void *name, unsigned int size );
unsigned int horizon_nsi_ioctl( unsigned int code, const void *input, unsigned int input_size,
                                void **output, unsigned int capacity, unsigned int *size );

#endif
