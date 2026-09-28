#define main registry_tests_main
#include "horizon_registry.c"
#undef main

static void expect_value( struct horizon_reg_key *key, const char *name, int expected_type,
                          const void *expected, unsigned int expected_size )
{
    unsigned char data[512];
    unsigned int len, size, total;
    const unsigned short *value = w( name, &len );
    int type;

    assert( !horizon_reg_get_value( key, value, len, &type, &total, data, sizeof(data), &size ) );
    assert( type == expected_type && size == expected_size && !memcmp( data, expected, size ) );
}

static void expect_string( struct horizon_reg_key *key, const char *name, const char *text )
{
    unsigned short value[512] = {0};
    unsigned int len;
    const unsigned short *str = w( text, &len );

    memcpy( value, str, len );
    expect_value( key, name, HORIZON_REG_SZ, value, len + 2 );
}

int main( int argc, char **argv )
{
    static const char provider[] = "Software\\Microsoft\\Cryptography\\Defaults\\Provider\\"
                                   "Microsoft Base Cryptographic Provider v1.0";
    static const char overrides[] = "WINE REGISTRY Version 2\n"
        "[Software\\\\Microsoft\\\\Cryptography\\\\Defaults\\\\Provider Types\\\\Type 001]\n"
        "\"Name\"=\"User Provider\"\n";
    struct horizon_reg reg;
    struct horizon_reg_key *machine, *key;
    const unsigned int kind = 1;
    unsigned int errors, count;
    char *text;
    long size;
    FILE *file;

    assert( argc == 2 );
    init( &reg, &machine );
    assert( open_key( &reg, machine, provider, 0, &key ) != HORIZON_REG_SUCCESS );
    assert( (file = fopen( argv[1], "rb" )) );
    assert( !fseek( file, 0, SEEK_END ) && (size = ftell( file )) > 0 && !fseek( file, 0, SEEK_SET ) );
    assert( (text = malloc( size )) && fread( text, 1, size, file ) == (size_t)size );
    fclose( file );
    assert( !horizon_reg_load( &reg, machine, text, size, &errors ) && !errors );
    assert( !open_key( &reg, machine, provider, 0, &key ) );
    expect_value( key, "Type", HORIZON_REG_DWORD, &kind, sizeof(kind) );
    expect_string( key, "Image Path", "C:\\windows\\system32\\rsaenh.dll" );
    horizon_reg_release( &reg, key );
    count = count_keys( machine );
    assert( !horizon_reg_load( &reg, machine, text, size, &errors ) && !errors );
    assert( count_keys( machine ) == count );
    assert( !horizon_reg_load( &reg, machine, overrides, sizeof(overrides) - 1, &errors ) && !errors );
    assert( !open_key( &reg, machine,
                      "Software\\Microsoft\\Cryptography\\Defaults\\Provider Types\\Type 001", 0, &key ) );
    expect_string( key, "Name", "User Provider" );
    horizon_reg_release( &reg, key );
    horizon_reg_release( &reg, reg.root );
    free( text );
    puts( "Horizon crypto registry: provider lookup, value types, idempotence and saved overrides passed" );
    return 0;
}
