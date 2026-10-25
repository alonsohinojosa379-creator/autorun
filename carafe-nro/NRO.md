# Carafe-NRO

Fork de Carafe que en vez de un NSP deja una carpeta lista para la SD con un NRO por juego.
Solo cambia el empaquetado (`app/crates/carafe-pack/src/nro.rs`); el asistente es el mismo.

Salida (carpeta `<TitleID>-sd` dentro de tu biblioteca): copiala tal cual a la raiz de la SD.

- `switch/wine/drive_c/Games/<juego>/` el juego con sus archivos de ajustes
- `switch/Carafe/<Titulo>.nro` + `<Titulo>.argv` el lanzador (sale en hbmenu con nombre e icono)

El runtime (`switch/wine/wine-nx-runtime.nro` y el prefijo de Wine) se instala una sola vez, aparte.
No hace falta `prod.keys` ni Docker para empaquetar.

## Runtime incluido

Si `carafe-runtime.bin` (del instalador oficial de Carafe) esta junto a la app, la primera vez se
desempaqueta en `<biblioteca>/runtime-sd`. Copiala tambien a la SD (una sola vez): trae
`switch/wine/wine-nx-runtime.nro` y el prefijo de Wine. Fuera de hbmenu el overlay de Carafe se
desactiva solo ("not installed") y corre como Autorun normal.
