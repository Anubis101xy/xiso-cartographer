#include "xcat/error.h"

const char *xc_err_str(xc_err err)
{
    switch (err) {
    case XC_OK:
        return "OK";
    case XC_ERR_INVALID_ARG:
        return "Invalid argument";
    case XC_ERR_NOMEM:
        return "Out of memory";
    case XC_ERR_IO:
        return "Read or write failed";
    case XC_ERR_RANGE:
        return "Tried to read past the end of the image";
    case XC_ERR_NOT_XDVDFS:
        return "Not an Xbox disc image";
    case XC_ERR_CORRUPT:
        return "The image is damaged or invalid";
    case XC_ERR_LIMIT:
        return "The image exceeds a safety limit";
    case XC_ERR_CANCELLED:
        return "Cancelled";
    case XC_ERR_NOT_INSTALLABLE:
        return "This game has files the Xbox hard disk can't store";
    case XC_ERR_URL:
        return "Invalid or unsupported URL";
    case XC_ERR_HTTP_PROTOCOL:
        return "The server sent an invalid response";
    case XC_ERR_NET_DNS:
        return "Couldn't find the server (name lookup failed)";
    case XC_ERR_NET_CONNECT:
        return "Couldn't connect to the server";
    case XC_ERR_NET_TIMEOUT:
        return "The server stopped responding";
    case XC_ERR_NET_IO:
        return "The network connection failed";
    case XC_ERR_HTTP_NO_RANGE:
        return "This server doesn't support resumable downloads (HTTP Range)";
    case XC_ERR_HTTP_DENIED:
        return "Access denied by the server (login missing or expired?)";
    case XC_ERR_HTTP_NOT_FOUND:
        return "The file wasn't found on the server";
    case XC_ERR_HTTP_STATUS:
        return "The server returned an error";
    case XC_ERR_HTTP_REDIRECTS:
        return "Too many redirects";
    case XC_ERR_HTTP_CHANGED:
        return "The file on the server changed during the download";
    case XC_ERR_UNSUPPORTED:
        return "Not supported by this version";
    case XC_ERR_CREDENTIALS:
        return "The credentials file is invalid, so no credentials are used";
    case XC_ERR_MAP:
        return "The map file can't be used";
    case XC_ERR_NO_SPACE:
        return "Not enough free space on the drive";
    case XC_ERR_VERIFY:
        return "An installed file doesn't match what was downloaded";
    case XC_ERR_FOLDER:
        return "No free install folder name left for this title";
    case XC_ERR_TLS:
        return "The secure (HTTPS) connection failed";
    case XC_ERR_TLS_CERT:
        return "The server's certificate isn't trusted (unknown issuer or wrong name)";
    case XC_ERR_TLS_CERT_TIME:
        return "The server's certificate isn't valid at the current time (is the clock right?)";
    case XC_ERR_TLS_NO_TIME:
        return "Couldn't get the correct time (NTP server unreachable and the console's clock is "
               "wrong)";
    case XC_ERR_TLS_NO_RANDOM:
        return "Couldn't gather enough randomness for a secure connection";
    case XC_ERR_TLS_NOT_TLS:
        return "The server didn't answer in HTTPS (is it a plain http:// server?)";
    case XC_ERR_IMAGE_HASH:
        return "The downloaded image doesn't match its published checksum";
    case XC_ERR_NTP:
        return "The time server's answer was refused (not synchronized, rate limit or invalid)";
    case XC_ERR_NOT_ZIP:
        return "Not a zip file (no end of central directory record)";
    case XC_ERR_ZIP_UNSUPPORTED:
        return "This zip can't be read: split archive, encryption or an unsupported compression "
               "method";
    case XC_ERR_ZIP_MEMBER:
        return "The zip must hold exactly one .iso or .xiso image";
    case XC_ERR_ZIP_CRC:
        return "The image in the zip failed its CRC-32 check (damaged download?)";
    case XC_ERR_SOURCE_SIZE:
        return "The file isn't the size its listing gives (a different file?)";
    case XC_ERR_CATALOG:
        return "The catalog file is damaged or invalid";
    case XC_ERR_SETTINGS:
        return "The settings file is invalid";
    case XC_ERR_UI_SCRIPT:
        return "The UI script is invalid";
    case XC_ERR_SMB_URL:
        return "Invalid smb:// address";
    case XC_ERR_SMB_LOGIN:
        return "The NAS refused the user name or password";
    case XC_ERR_SMB_SHARE:
        return "The NAS has no shared folder of that name";
    case XC_ERR_SMB_DENIED:
        return "This login may not open that folder or file on the NAS";
    case XC_ERR_SMB_NOT_FOUND:
        return "The NAS has no folder or file of that name";
    case XC_ERR_SMB:
        return "The NAS (SMB) refused or failed the request";
    case XC_ERR_GAME_FOLDER:
        return "The game folder has a name or file that can't be installed";
    case XC_ERR_COUNT_:
        break;
    }
    return "Unknown error";
}

/* ADR 0015 decision 5. Every code is listed, so a new one can't build without an action. */
const char *xc_err_action(xc_err err)
{
    switch (err) {
    case XC_OK:
        return "";
    case XC_ERR_INVALID_ARG:
        return "This is a bug: please report it with E:\\xcat\\install.log.";
    case XC_ERR_NOMEM:
        return "Return to the dashboard and start the app again.";
    case XC_ERR_IO:
        return "Check that the hard disk has room and works, then try again.";
    case XC_ERR_RANGE:
    case XC_ERR_CORRUPT:
        return "Get a fresh copy of the image: this one is damaged or cut short.";
    case XC_ERR_NOT_XDVDFS:
        return "Use an Xbox disc image: a trimmed .xiso.iso or a full-disc .iso.";
    case XC_ERR_LIMIT:
        return "If it's a real game, please report it with E:\\xcat\\install.log.";
    case XC_ERR_CANCELLED:
        return "Install the game again to carry on where it stopped.";
    case XC_ERR_NOT_INSTALLABLE:
        return "It can't be installed as it is: the files listed break the hard disk's rules.";
    case XC_ERR_URL:
        return "Fix the address in the map: it must start with http:// or https://.";
    case XC_ERR_HTTP_PROTOCOL:
        return "Check the address, or serve the file from another server.";
    case XC_ERR_NET_DNS:
        return "Check the server's name and the dashboard's DNS settings, then try again.";
    case XC_ERR_NET_CONNECT:
        return "Check the network cable and that the server is running, then try again.";
    case XC_ERR_NET_TIMEOUT:
    case XC_ERR_NET_IO:
        return "Check the network, then try again: what's written is kept.";
    case XC_ERR_HTTP_NO_RANGE:
        return "Serve the file from a server that supports Range requests.";
    case XC_ERR_HTTP_DENIED:
        return "Log in again in SETTINGS, or check the server's login in the credentials file.";
    case XC_ERR_HTTP_NOT_FOUND:
        return "Check the address in the map: the server doesn't have that file.";
    case XC_ERR_HTTP_STATUS:
        return "The server had a problem: try again later.";
    case XC_ERR_HTTP_REDIRECTS:
        return "Check the address in the map: the server keeps sending it elsewhere.";
    case XC_ERR_HTTP_CHANGED:
        return "Try again: the install starts over with the new file.";
    case XC_ERR_UNSUPPORTED:
        return "Use a source this version supports, or a newer xiso-cartographer.";
    case XC_ERR_CREDENTIALS:
        return "Fix or delete E:\\Apps\\xcat\\credentials: install.log names the line.";
    case XC_ERR_MAP:
        return "Fix E:\\xcat\\map.txt: it must start with \"# xcat-map v1\".";
    case XC_ERR_NO_SPACE:
        return "Free up space, or choose another install location in SETTINGS, then try again.";
    case XC_ERR_VERIFY:
        return "Try again to write the file again; if it keeps failing, check the hard disk.";
    case XC_ERR_FOLDER:
        return "Rename or remove the folders named after this game in the install folder.";
    case XC_ERR_TLS:
        return "Try again; if it keeps failing, use the server's http:// address.";
    case XC_ERR_TLS_CERT:
        return "Check the address; for your own server, add its CA to E:\\Apps\\xcat\\cacert.pem.";
    case XC_ERR_TLS_CERT_TIME:
    case XC_ERR_TLS_NO_TIME:
        return "Set the console's clock in the dashboard, or let it reach a time server.";
    case XC_ERR_TLS_NO_RANDOM:
        return "Try again in a moment.";
    case XC_ERR_TLS_NOT_TLS:
        return "Use the server's http:// address instead of https://.";
    case XC_ERR_NTP:
        return "Check the network and E:\\Apps\\xcat\\ntp.txt, then try again.";
    case XC_ERR_IMAGE_HASH:
    case XC_ERR_SOURCE_SIZE:
        return "Check the item on archive.org: the file isn't the one its listing describes.";
    case XC_ERR_NOT_ZIP:
        return "Check the address: the file isn't a zip.";
    case XC_ERR_ZIP_UNSUPPORTED:
        return "Repack the image as an ordinary zip: stored or Deflate, one file, no password.";
    case XC_ERR_ZIP_MEMBER:
        return "Use a zip that holds exactly one .iso or .xiso image.";
    case XC_ERR_ZIP_CRC:
        return "Try again: the zip is downloaded again.";
    case XC_ERR_CATALOG:
        return "Delete E:\\Apps\\xcat\\catalog.bin, or replace it with the release's.";
    case XC_ERR_SETTINGS:
        return "Fix or delete E:\\Apps\\xcat\\settings.";
    case XC_ERR_UI_SCRIPT:
        return "Fix the line the log names in ui-script.txt, or delete the file.";
    case XC_ERR_SMB_URL:
        return "Fix the address in the map: smb://<NAS>/<share>/<folder or file>.";
    case XC_ERR_SMB_LOGIN:
        return "Add the NAS again in SETTINGS > NAS with the right user name and password.";
    case XC_ERR_SMB_SHARE:
    case XC_ERR_SMB_NOT_FOUND:
        return "Check the shared folder on the NAS, or add the NAS folder again in SETTINGS > NAS.";
    case XC_ERR_SMB_DENIED:
        return "Give this user read access to the shared folder in the NAS's settings.";
    case XC_ERR_SMB:
        return "Try again; if it keeps failing, check SMB2 or SMB3 is on in the NAS's settings.";
    case XC_ERR_GAME_FOLDER:
        return "The log names it: rename or remove it in the game's folder on the NAS.";
    case XC_ERR_COUNT_:
        break;
    }
    return "Go back, and report it with E:\\xcat\\install.log.";
}

bool xc_err_retry(xc_err err)
{
    switch (err) {
    case XC_ERR_IO:
    case XC_ERR_CANCELLED:
    case XC_ERR_NET_DNS:
    case XC_ERR_NET_CONNECT:
    case XC_ERR_NET_TIMEOUT:
    case XC_ERR_NET_IO:
    case XC_ERR_HTTP_STATUS:
    case XC_ERR_HTTP_CHANGED:
    case XC_ERR_NO_SPACE:
    case XC_ERR_VERIFY:
    case XC_ERR_TLS:
    case XC_ERR_TLS_NO_RANDOM:
    case XC_ERR_NTP:
    case XC_ERR_ZIP_CRC:
    case XC_ERR_SMB:
        return true;
    default:
        return false;
    }
}
