#include <assert.h>
#include <stdio.h>

#include "../src/dpi_options.h"

int main(void)
{
    struct MPDpiOptions options;
    int only_600[] = { 600 };
    int both[] = { 300, 600 };
    int epson_generic[] = { 360, 720 };
    int epson_pwg[] = { 360 };
    int four_modes[] = { 300, 360, 600, 720 };

    assert(mp_dpi_engine_allows_compat("pwg-raster"));
    assert(mp_dpi_engine_allows_compat("urf"));
    assert(!mp_dpi_engine_allows_compat("jpeg"));
    assert(!mp_dpi_engine_allows_compat("pdf"));
    assert(!mp_dpi_engine_allows_compat("postscript"));
    assert(!mp_dpi_engine_allows_compat(NULL));

    mp_dpi_build_options(only_600, 1, 1, 300, 0, &options);
    assert(options.count == 2);
    assert(options.values[0] == 600 && options.compatibility[0] == 0);
    assert(options.values[1] == 300 && options.compatibility[1] == 1);
    assert(options.active == 0 && options.selected == 600);

    mp_dpi_build_options(only_600, 1, 1, 300, 1, &options);
    assert(options.active == 1 && options.selected == 300);

    mp_dpi_build_options(only_600, 1, 0, 300, 1, &options);
    assert(options.count == 1);
    assert(options.values[0] == 600 && options.selected == 600);

    mp_dpi_build_options(both, 2, 1, 300, 0, &options);
    assert(options.count == 2);
    assert(options.values[0] == 300 && options.compatibility[0] == 0);
    assert(options.values[1] == 600 && options.compatibility[1] == 0);
    assert(options.active == 0 && options.selected == 300);

    mp_dpi_build_options(NULL, 0, 1, 600, 0, &options);
    assert(options.count == 1);
    assert(options.values[0] == 300 && options.compatibility[0] == 0);
    assert(options.selected == 300);


    mp_dpi_build_options(epson_generic, 2, 0, 300, 0, &options);
    assert(options.count == 2);
    assert(options.values[0] == 360 && options.compatibility[0] == 0);
    assert(options.values[1] == 720 && options.compatibility[1] == 0);
    assert(options.active == 0 && options.selected == 360);

    mp_dpi_build_options(epson_generic, 2, 0, 720, 1, &options);
    assert(options.active == 1 && options.selected == 720);

    mp_dpi_build_options(epson_pwg, 1, 1, 300, 0, &options);
    assert(options.count == 2);
    assert(options.values[0] == 360 && options.compatibility[0] == 0);
    assert(options.values[1] == 300 && options.compatibility[1] == 1);
    assert(options.active == 0 && options.selected == 360);

    mp_dpi_build_options(four_modes, 4, 1, 360, 1, &options);
    assert(options.count == 4);
    assert(options.values[0] == 300);
    assert(options.values[1] == 360);
    assert(options.values[2] == 600);
    assert(options.values[3] == 720);
    assert(options.active == 1 && options.selected == 360);

    puts("DPI option tests passed");
    return 0;
}
