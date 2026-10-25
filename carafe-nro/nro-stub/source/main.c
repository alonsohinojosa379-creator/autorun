// Lanzador de Carafe-NRO: lee <este nro>.argv y arranca el runtime con esos argumentos.
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void fail(const char* text) {
    consoleInit(NULL);
    printf("Carafe\n\n%s\n\nPulsa + para salir.\n", text);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

int main(int argc, char** argv) {
    char self[FS_MAX_PATH + 16];
    char line[2048];
    if (argc < 1 || !argv[0]) { fail("No se conoce la ruta del lanzador."); return 1; }
    if (argv[0][0] == '/') snprintf(self, sizeof(self), "sdmc:%s", argv[0]);
    else snprintf(self, sizeof(self), "%s", argv[0]);
    size_t n = strlen(self);
    if (n < 4 || strcasecmp(self + n - 4, ".nro") != 0) { fail("El lanzador no es un .nro."); return 1; }
    strcpy(self + n - 4, ".argv");

    FILE* f = fopen(self, "r");
    if (!f) { char m[FS_MAX_PATH + 64]; snprintf(m, sizeof(m), "No encuentro %s", self); fail(m); return 1; }
    size_t got = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    line[got] = 0;
    while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r' || line[got - 1] == ' ')) line[--got] = 0;
    if (got == 0) { fail("El archivo .argv esta vacio."); return 1; }

    // La primera palabra es el .nro del runtime.
    char nro[FS_MAX_PATH];
    size_t i = 0;
    while (line[i] && line[i] != ' ' && i < sizeof(nro) - 1) { nro[i] = line[i]; i++; }
    nro[i] = 0;
    FILE* r = fopen(nro, "rb");
    if (!r) { char m[FS_MAX_PATH + 96]; snprintf(m, sizeof(m), "Falta el runtime: %s\nInstala el runtime en la SD primero.", nro); fail(m); return 1; }
    fclose(r);

    envSetNextLoad(nro, line);
    return 0;
}
