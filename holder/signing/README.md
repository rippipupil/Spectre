# Firma del APK de Spectre Holder

Android solo instala una versión nueva de una app encima de la que ya hay si
las dos van firmadas con la misma clave. Por eso la CI firma siempre con una
clave fija.

`spectre-holder.jks` es esa clave (PKCS12, alias y contraseña
`spectre-holder`). Está en el repositorio para que cada release se pueda
instalar encima de la anterior sin configurar nada. Como el repositorio es
público, cualquiera podría firmar un APK con ella: instala Spectre Holder solo
desde las releases de este repositorio.

Para usar una clave privada, crea en GitHub (Settings > Secrets and variables >
Actions) estos secretos y la CI la usará en vez de esta:

- `HOLDER_KEYSTORE_BASE64`: el archivo .jks en base64
- `HOLDER_KEYSTORE_PASSWORD`, `HOLDER_KEY_ALIAS`, `HOLDER_KEY_PASSWORD`

Al cambiar de clave hay que desinstalar Spectre Holder una vez y volver a
emparejar el móvil.
