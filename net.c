/*
 * Diary Server client: HTTP/1.0 over bsdsocket.library (Roadshow).
 *
 * bsdsocket.library is only opened when a server is used, so Diary still
 * runs on Amigas without a TCP/IP stack.
 */

#include <sys/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>

#include <string.h>

#include "net.h"

struct Library *SocketBase;

#define kDefaultPort    8642
#define kConnectSeconds 5       /* waiting for the server to accept */
#define kReplySeconds   15      /* waiting for each part of the reply */
#define kMaxReply       (80 * 1024)

static char gHost[64];
static UWORD gPort = kDefaultPort;
static char gKey[64];

static void SetError(char *error, LONG size, const char *text, const char *detail)
{
    strncpy(error, text, size - 1);
    error[size - 1] = 0;
    if (detail != NULL && strlen(error) + strlen(detail) < (size_t)size - 1)
        strcat(error, detail);
}

void NetSetServer(const char *address, const char *key)
{
    const char *colon = strchr(address, ':');
    LONG length = colon != NULL ? colon - address : (LONG)strlen(address);

    if (length >= (LONG)sizeof(gHost))
        length = sizeof(gHost) - 1;
    memcpy(gHost, address, length);
    gHost[length] = 0;

    gPort = kDefaultPort;
    if (colon != NULL) {
        LONG port = 0;
        const char *p;
        for (p = colon + 1; *p >= '0' && *p <= '9'; p++)
            port = port * 10 + (*p - '0');
        if (port > 0 && port < 65536)
            gPort = port;
    }
    strncpy(gKey, key, sizeof(gKey) - 1);
    gKey[sizeof(gKey) - 1] = 0;
}

void NetCleanup(void)
{
    if (SocketBase != NULL)
        CloseLibrary(SocketBase);
    SocketBase = NULL;
}

void NetEncode(const char *text, char *out, LONG outSize)
{
    static const char kHex[] = "0123456789ABCDEF";
    LONG n = 0;

    for (; *text != 0 && n + 4 < outSize; text++) {
        UBYTE c = *text;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '-' || c == '.' || c == '_') {
            out[n++] = c;
        } else {
            out[n++] = '%';
            out[n++] = kHex[c >> 4];
            out[n++] = kHex[c & 15];
        }
    }
    out[n] = 0;
}

static void AppendNumber(char *s, LONG n)
{
    char digits[12];
    int count = 0;

    s += strlen(s);
    do {
        digits[count++] = '0' + n % 10;
        n /= 10;
    } while (n > 0);
    while (count > 0)
        *s++ = digits[--count];
    *s = 0;
}

/* Waits until the socket can be read (or written); FALSE on timeout or Ctrl-C. */
static BOOL WaitSocket(LONG s, BOOL forWriting, LONG seconds)
{
    fd_set set;
    struct timeval timeout;
    ULONG signals = SIGBREAKF_CTRL_C;

    FD_ZERO(&set);
    FD_SET(s, &set);
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    return WaitSelect(s + 1, forWriting ? NULL : &set, forWriting ? &set : NULL,
                      NULL, &timeout, &signals) > 0;
}

static LONG Connect(char *error, LONG errorSize)
{
    struct sockaddr_in address;
    struct hostent *host;
    in_addr_t ip;
    LONG s, nonBlocking = 1, problem = 0;
    socklen_t problemSize = sizeof(problem);

    if (SocketBase == NULL)
        SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SocketBase == NULL) {
        SetError(error, errorSize, "No TCP/IP stack is running (bsdsocket.library is missing).", NULL);
        return -1;
    }
    if (gHost[0] == 0) {
        SetError(error, errorSize, "No server is set.", NULL);
        return -1;
    }

    ip = inet_addr((STRPTR)gHost);
    if (ip == INADDR_NONE) {
        host = gethostbyname((STRPTR)gHost);
        if (host == NULL) {
            SetError(error, errorSize, "Can't find the server ", gHost);
            return -1;
        }
        memcpy(&ip, host->h_addr, sizeof(ip));
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        SetError(error, errorSize, "Can't create a network connection.", NULL);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(gPort);
    address.sin_addr.s_addr = ip;

    /* Connect without blocking, so an unreachable server only costs a few seconds. */
    IoctlSocket(s, FIONBIO, &nonBlocking);
    if (connect(s, (struct sockaddr *)&address, sizeof(address)) < 0 && Errno() != EINPROGRESS) {
        problem = Errno();
    } else if (!WaitSocket(s, TRUE, kConnectSeconds)) {
        problem = ETIMEDOUT;
    } else {
        getsockopt(s, SOL_SOCKET, SO_ERROR, &problem, &problemSize);
    }
    if (problem != 0) {
        CloseSocket(s);
        if (problem == ECONNREFUSED) {
            SetError(error, errorSize, "The server refused the connection. Is Diary Server running on ", gHost);
            if (strlen(error) + 1 < (size_t)errorSize)
                strcat(error, "?");
        }
        else if (problem == ETIMEDOUT)
            SetError(error, errorSize, "The server didn't answer: ", gHost);
        else
            SetError(error, errorSize, "Can't connect to the server ", gHost);
        return -1;
    }
    nonBlocking = 0;
    IoctlSocket(s, FIONBIO, &nonBlocking);
    return s;
}

static BOOL SendAll(LONG s, const char *data, LONG length)
{
    while (length > 0) {
        LONG sent = send(s, (APTR)data, length, 0);
        if (sent <= 0)
            return FALSE;
        data += sent;
        length -= sent;
    }
    return TRUE;
}

/* Case-insensitive test whether line starts with name (e.g. "X-Diary-Version:"). */
static BOOL HeaderIs(const char *line, const char *name)
{
    for (; *name != 0; line++, name++) {
        char a = *line, b = *name;
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return FALSE;
    }
    return TRUE;
}

/* Splits a raw reply into status, version and body (moved to the buffer's start). */
static BOOL ParseReply(char *buf, LONG length, HttpResponse *response)
{
    char *end = NULL, *line;
    LONG i;

    for (i = 0; i + 3 < length; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = buf + i;
            break;
        }
    if (end == NULL || strncmp(buf, "HTTP/", 5) != 0)
        return FALSE;
    *end = 0;

    line = strchr(buf, ' ');
    if (line == NULL)
        return FALSE;
    response->status = 0;
    for (line++; *line >= '0' && *line <= '9'; line++)
        response->status = response->status * 10 + (*line - '0');

    response->version[0] = 0;
    for (line = strchr(buf, '\n'); line != NULL; line = strchr(line, '\n')) {
        line++;
        if (HeaderIs(line, "X-Diary-Version:")) {
            const char *v = line + 16;
            int n = 0;
            while (*v == ' ')
                v++;
            while (*v > ' ' && n < (int)sizeof(response->version) - 1)
                response->version[n++] = *v++;
            response->version[n] = 0;
        }
    }

    response->length = length - (end + 4 - buf);
    memmove(buf, end + 4, response->length);
    buf[response->length] = 0;
    response->body = buf;
    return TRUE;
}

BOOL HttpRequest(const char *method, const char *path,
                 const char *body, LONG bodyLength, const char *base,
                 HttpResponse *response, char *error, LONG errorSize)
{
    char header[512];
    char *buf;
    LONG s, length = 0;
    BOOL ok;

    response->status = 0;
    response->version[0] = 0;
    response->body = NULL;
    response->length = 0;

    if (strlen(path) + strlen(gHost) + strlen(gKey) + 200 > sizeof(header)) {
        SetError(error, errorSize, "The request is too long.", NULL);
        return FALSE;
    }
    strcpy(header, method);
    strcat(header, " ");
    strcat(header, path);
    strcat(header, strchr(path, '?') != NULL ? "&charset=latin1" : "?charset=latin1");
    strcat(header, " HTTP/1.0\r\nHost: ");
    strcat(header, gHost);
    strcat(header, "\r\nUser-Agent: Diary for MUI\r\n");
    if (gKey[0] != 0) {
        strcat(header, "X-Diary-Key: ");
        strcat(header, gKey);
        strcat(header, "\r\n");
    }
    if (base != NULL) {
        strcat(header, "X-Diary-Base: ");
        strcat(header, base);
        strcat(header, "\r\n");
    }
    if (body != NULL) {
        strcat(header, "Content-Length: ");
        AppendNumber(header, bodyLength);
        strcat(header, "\r\n");
    }
    strcat(header, "\r\n");

    buf = AllocVec(kMaxReply + 1, MEMF_ANY);
    if (buf == NULL) {
        SetError(error, errorSize, "Not enough memory.", NULL);
        return FALSE;
    }
    s = Connect(error, errorSize);
    if (s < 0) {
        FreeVec(buf);
        return FALSE;
    }

    ok = SendAll(s, header, strlen(header)) && (body == NULL || SendAll(s, body, bodyLength));
    while (ok) {
        LONG got;
        if (!WaitSocket(s, FALSE, kReplySeconds)) {
            ok = FALSE;
            break;
        }
        got = recv(s, buf + length, kMaxReply - length, 0);
        if (got < 0)
            ok = FALSE;
        if (got <= 0)
            break;
        length += got;
        if (length == kMaxReply)
            break;
    }
    CloseSocket(s);

    if (!ok) {
        FreeVec(buf);
        SetError(error, errorSize, "The connection to the server broke off.", NULL);
        return FALSE;
    }
    if (!ParseReply(buf, length, response)) {
        FreeVec(buf);
        SetError(error, errorSize, "The server's reply wasn't understood. Is it a Diary Server?", NULL);
        return FALSE;
    }
    return TRUE;
}

void HttpFree(HttpResponse *response)
{
    if (response->body != NULL)
        FreeVec(response->body);
    response->body = NULL;
}
