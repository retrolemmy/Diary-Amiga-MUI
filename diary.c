/*
 * Diary - a simple day-by-day journal for MUI: MorphOS, and AmigaOS 3.x and
 * 4 with MUI 3.8 or later (68k).
 *
 * One window: a month calendar and the list of all entries on the left,
 * the selected day's text on the right. Entries are plain text files named
 * YYYY-MM-DD in a "Diary Entries" drawer next to the program, or on a
 * Diary Server.
 *
 * The same program as Diary for AmigaOS 3.2, with a MUI window instead of
 * ReAction: everything but the window is shared with it.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <libraries/mui.h>
#include <mui/TextEditor_mcc.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>

#include <string.h>

#include "net.h"

static const char kVersion[] __attribute__((used)) = "$VER: Diary 1.0.1 (1.11.2026) MUI";

/* MUI needs more stack than the 4 KB AmigaOS starts programs with by
   default. Referencing __stkinit links in libnix's swapstack, which moves
   Diary to a stack of __stack bytes before main() if it got less. */
extern void __stkinit(void);
void *__stkinit_ref = __stkinit;
unsigned long __stack = 32768;

#define kEntriesDir     "PROGDIR:Diary Entries"
#define kSettingsFile   "PROGDIR:Diary Entries/Diary Settings"
#define kMaxSearch      100

#ifndef MAKE_ID
#define MAKE_ID(a, b, c, d) ((ULONG)(a) << 24 | (ULONG)(b) << 16 | (ULONG)(c) << 8 | (ULONG)(d))
#endif

/* What MUIM_Application_NewInput returns: menu items return their command,
   buttons and the list return an ID_ value. */
enum {
    CMD_ABOUT = 1, CMD_SAVE, CMD_QUIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELECTALL,
    CMD_FIND, CMD_FINDAGAIN, CMD_SHOWALL,
    CMD_PREVDAY, CMD_NEXTDAY, CMD_TODAY,
    CMD_SERVER, CMD_MUIPREFS,
    ID_LIST = 100, ID_CALPREV, ID_CALNEXT, ID_ASK_OK, ID_ASK_CANCEL,
    ID_DAY0 = 200       /* ID_DAY0 + 0..41: the calendar's day cells */
};

struct IntuitionBase *IntuitionBase;
struct Library *MUIMasterBase;

/* A list of days (days since 1 Jan 1978, the Amiga epoch), newest first. */
typedef struct {
    LONG *days;
    LONG count, capacity;
} DayList;

static DayList gAllDays, gShownDays;    /* every entry / the ones listed */
static char gSearch[kMaxSearch + 1];    /* current filter, "" = none */
static LONG gDay;                       /* the open day */
static LONG gCalYear, gCalMonth;        /* the month the calendar shows */
static BOOL gHaveFolder;

/* With a Diary Server set, entries live there instead of in the drawer. */
static char gServer[64];                /* "host" or "host:port", "" = local */
static char gServerKey[64];
static BOOL gUseServer;                 /* FALSE also after "Work Locally" */
static char gEntryVersion[16];          /* server version of the open entry */
static char gTitle[100] = "Diary";

/* Kept in the settings file so that Diary for AmigaOS 3.2 can share it;
   this version takes its fonts from the MUI settings. */
static char gFontName[64];
static UWORD gFontSize;

static Object *gApp, *gWin, *gList, *gDateLabel, *gEditor;
static Object *gMonthLabel, *gDayCells[42];
static Object *gAskWin, *gAskBody, *gAskString, *gAskOK, *gAskCancel;
static char gDateText[64], gMonthText[32];

static const char *kWeekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *kMonths[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static struct NewMenu kMenus[] = {
    { NM_TITLE, (STRPTR)"Project",          NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"About...",         NULL, 0, 0, (APTR)CMD_ABOUT },
    { NM_ITEM,  NM_BARLABEL,                NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Save",             (STRPTR)"S", 0, 0, (APTR)CMD_SAVE },
    { NM_ITEM,  NM_BARLABEL,                NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",             (STRPTR)"Q", 0, 0, (APTR)CMD_QUIT },
    { NM_TITLE, (STRPTR)"Edit",             NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Undo",             (STRPTR)"Z", 0, 0, (APTR)CMD_UNDO },
    { NM_ITEM,  (STRPTR)"Redo",             (STRPTR)"Y", 0, 0, (APTR)CMD_REDO },
    { NM_ITEM,  NM_BARLABEL,                NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Cut",              (STRPTR)"X", 0, 0, (APTR)CMD_CUT },
    { NM_ITEM,  (STRPTR)"Copy",             (STRPTR)"C", 0, 0, (APTR)CMD_COPY },
    { NM_ITEM,  (STRPTR)"Paste",            (STRPTR)"V", 0, 0, (APTR)CMD_PASTE },
    { NM_ITEM,  NM_BARLABEL,                NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Select All",       (STRPTR)"A", 0, 0, (APTR)CMD_SELECTALL },
    { NM_ITEM,  NM_BARLABEL,                NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Find...",          (STRPTR)"F", 0, 0, (APTR)CMD_FIND },
    { NM_ITEM,  (STRPTR)"Find Again",       (STRPTR)"G", 0, 0, (APTR)CMD_FINDAGAIN },
    { NM_ITEM,  (STRPTR)"Show All Entries", NULL, 0, 0, (APTR)CMD_SHOWALL },
    { NM_TITLE, (STRPTR)"Go",               NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Previous Day",     (STRPTR)"[", 0, 0, (APTR)CMD_PREVDAY },
    { NM_ITEM,  (STRPTR)"Next Day",         (STRPTR)"]", 0, 0, (APTR)CMD_NEXTDAY },
    { NM_ITEM,  NM_BARLABEL,                NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Today",            (STRPTR)"T", 0, 0, (APTR)CMD_TODAY },
    { NM_TITLE, (STRPTR)"Settings",         NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Diary Server...",  NULL, 0, 0, (APTR)CMD_SERVER },
    { NM_ITEM,  (STRPTR)"MUI...",           NULL, 0, 0, (APTR)CMD_MUIPREFS },
    { NM_END,   NULL,                       NULL, 0, 0, NULL }
};

static void RefreshList(void);
static void UpdateCalendar(void);

/* ---------- Small helpers, dates and day lists (as in Diary for AmigaOS 3.2) ---------- */

static void AppendNum(char *s, LONG n, int minDigits)
{
    char digits[12];
    int count = 0;

    s += strlen(s);
    do {
        digits[count++] = '0' + n % 10;
        n /= 10;
    } while (n > 0);
    while (count < minDigits)
        digits[count++] = '0';
    while (count > 0)
        *s++ = digits[--count];
    *s = 0;
}

/* Converts days since 1 Jan 1978 to a civil date (H. Hinnant's algorithm). */
static void DayToDate(LONG day, LONG *year, LONG *month, LONG *dayOfMonth)
{
    LONG z = day + 2922 + 719468;   /* days since 1 Mar 0000 */
    LONG era = z / 146097;
    LONG doe = z - era * 146097;
    LONG yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    LONG doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    LONG mp = (5 * doy + 2) / 153;
    LONG m = mp < 10 ? mp + 3 : mp - 9;

    *year = yoe + era * 400 + (m <= 2);
    *month = m;
    *dayOfMonth = doy - (153 * mp + 2) / 5 + 1;
}

/* The inverse of DayToDate. */
static LONG DateToDay(LONG year, LONG month, LONG dayOfMonth)
{
    LONG y = year - (month <= 2);
    LONG era = y / 400;
    LONG yoe = y - era * 400;
    LONG doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + dayOfMonth - 1;
    LONG doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468 - 2922;
}

static LONG Today(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return ds.ds_Days;
}

static int Weekday(LONG day)
{
    return (day % 7 + 7) % 7;   /* 1 Jan 1978 was a Sunday */
}

static void MakeEntryName(LONG day, char *name)
{
    LONG y, m, d;

    DayToDate(day, &y, &m, &d);
    name[0] = 0;
    AppendNum(name, y, 4);
    strcat(name, "-");
    AppendNum(name, m, 2);
    strcat(name, "-");
    AppendNum(name, d, 2);
}

/* Parses a YYYY-MM-DD file name; rejects anything else. */
static BOOL ParseEntryName(const char *name, LONG *day)
{
    static const char kPattern[] = "####-##-##";
    LONG y, m, d, checkYear, checkMonth, checkDay;
    int i;

    if (strlen(name) != 10)
        return FALSE;
    for (i = 0; i < 10; i++) {
        char c = name[i];
        if (kPattern[i] == '#' ? (c < '0' || c > '9') : c != '-')
            return FALSE;
    }
    y = (name[0] - '0') * 1000 + (name[1] - '0') * 100 + (name[2] - '0') * 10 + (name[3] - '0');
    m = (name[5] - '0') * 10 + (name[6] - '0');
    d = (name[8] - '0') * 10 + (name[9] - '0');
    if (y < 1978 || m < 1 || m > 12 || d < 1 || d > 31)
        return FALSE;

    *day = DateToDay(y, m, d);
    DayToDate(*day, &checkYear, &checkMonth, &checkDay);    /* rejects e.g. 31 April */
    return checkYear == y && checkMonth == m && checkDay == d;
}

static void MakeLongDate(LONG day, char *s)
{
    LONG y, m, d;

    DayToDate(day, &y, &m, &d);
    strcpy(s, kWeekdays[Weekday(day)]);
    strcat(s, ", ");
    AppendNum(s, d, 1);
    strcat(s, " ");
    strcat(s, kMonths[m - 1]);
    strcat(s, " ");
    AppendNum(s, y, 4);
}

static void MakeShortDate(LONG day, char *s)
{
    LONG y, m, d;

    DayToDate(day, &y, &m, &d);
    strncpy(s, kWeekdays[Weekday(day)], 3);
    s[3] = 0;
    strcat(s, " ");
    AppendNum(s, d, 2);
    strcat(s, " ");
    strncat(s, kMonths[m - 1], 3);
    strcat(s, " ");
    AppendNum(s, y, 4);
}

static LONG FindDay(const DayList *list, LONG day)
{
    LONG i;

    for (i = 0; i < list->count; i++)
        if (list->days[i] == day)
            return i;
    return -1;
}

static BOOL ReserveDays(DayList *list, LONG capacity)
{
    LONG *days;

    if (capacity <= list->capacity)
        return TRUE;
    capacity += 64;
    days = AllocVec(capacity * sizeof(LONG), MEMF_ANY);
    if (days == NULL)
        return FALSE;
    if (list->days != NULL) {
        CopyMem(list->days, days, list->count * sizeof(LONG));
        FreeVec(list->days);
    }
    list->days = days;
    list->capacity = capacity;
    return TRUE;
}

/* Inserts a day, keeping the list sorted newest first. */
static BOOL InsertDay(DayList *list, LONG day)
{
    LONG i, j;

    if (FindDay(list, day) >= 0)
        return TRUE;
    if (!ReserveDays(list, list->count + 1))
        return FALSE;
    for (i = 0; i < list->count && list->days[i] > day; i++)
        ;
    for (j = list->count; j > i; j--)
        list->days[j] = list->days[j - 1];
    list->days[i] = day;
    list->count++;
    return TRUE;
}

static void RemoveDay(DayList *list, LONG day)
{
    LONG i = FindDay(list, day);

    if (i < 0)
        return;
    for (; i < list->count - 1; i++)
        list->days[i] = list->days[i + 1];
    list->count--;
}

static BOOL CopyDays(DayList *to, const DayList *from)
{
    if (!ReserveDays(to, from->count))
        return FALSE;
    if (from->count > 0)
        CopyMem(from->days, to->days, from->count * sizeof(LONG));
    to->count = from->count;
    return TRUE;
}

static void FreeDays(DayList *list)
{
    if (list->days != NULL)
        FreeVec(list->days);
    list->days = NULL;
    list->count = list->capacity = 0;
}

/* ---------- Messages ---------- */

/* Shows a MUI requester; buttons as for EasyRequest ("Retry|Work Locally"),
   returns 1 for the first, 2 for the second, ... and 0 for the last. */
static LONG Ask(const char *text, const char *buttons)
{
    return MUI_RequestA(gApp, gWin, 0, (STRPTR)"Diary", (STRPTR)buttons,
                        (STRPTR)"%s", (APTR)&text);  /* a '%' in text is shown as it is */
}

static void ShowMessage(const char *text)
{
    Ask(text, "*_OK");
}

static void ShowError(const char *text)
{
    char message[200];
    char reason[80];

    strcpy(message, text);
    if (IoErr() != 0 && Fault(IoErr(), NULL, reason, sizeof(reason))) {
        strcat(message, "\n");
        strcat(message, reason);
    }
    ShowMessage(message);
}

static void ShowAbout(void)
{
    ShowMessage("\33cDiary 1.0.1 for MUI\n\n"
                "A simple day-by-day journal for MorphOS\n"
                "and AmigaOS with MUI.\n\n"
                "https://github.com/retrolemmy/Diary-Amiga-MUI\n"
                "No rights reserved (CC0).");
}

static void SetBusy(BOOL busy)
{
    if (gApp != NULL)
        set(gApp, MUIA_Application_Sleep, busy);
}

/* Shows a server problem: what failed, then why. */
static void ShowServerError(const char *what, const char *why)
{
    char message[300];

    strcpy(message, what);
    strcat(message, "\n");
    strncat(message, why, 200);
    ShowMessage(message);
}

static void UpdateTitle(void)
{
    strcpy(gTitle, "Diary");
    if (gUseServer) {
        strcat(gTitle, " - ");
        strcat(gTitle, gServer);
    }
    if (gWin != NULL)
        set(gWin, MUIA_Window_Title, gTitle);
}

/* ---------- Entry files and settings ---------- */

static void EntryPath(LONG day, char *path)
{
    strcpy(path, kEntriesDir "/");
    MakeEntryName(day, path + strlen(path));
}

static void FindEntriesFolder(void)
{
    BPTR lock = Lock(kEntriesDir, ACCESS_READ);

    if (lock == 0)
        lock = CreateDir(kEntriesDir);
    gHaveFolder = lock != 0;
    if (lock != 0)
        UnLock(lock);
}

static void ScanLocalEntries(void)
{
    struct FileInfoBlock *fib;
    BPTR lock;

    if (!gHaveFolder)
        return;
    fib = AllocDosObject(DOS_FIB, NULL);
    lock = Lock(kEntriesDir, ACCESS_READ);
    if (fib != NULL && lock != 0 && Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            LONG day;
            if (fib->fib_DirEntryType < 0 && fib->fib_Size > 0
                    && ParseEntryName(fib->fib_FileName, &day))
                InsertDay(&gAllDays, day);
        }
    }
    if (lock != 0)
        UnLock(lock);
    if (fib != NULL)
        FreeDosObject(DOS_FIB, fib);
}

/* Adds the days of a server reply ("YYYY-MM-DD ..." lines) to list. */
static void AddDaysFromReply(DayList *list, const char *reply)
{
    while (*reply != 0) {
        char name[11];
        LONG day;

        strncpy(name, reply, 10);
        name[10] = 0;
        if (ParseEntryName(name, &day))
            InsertDay(list, day);
        reply = strchr(reply, '\n');
        if (reply == NULL)
            break;
        reply++;
    }
}

static BOOL ScanServerEntries(char *error, LONG errorSize)
{
    HttpResponse response;

    if (!HttpRequest("GET", "/entries", NULL, 0, NULL, &response, error, errorSize))
        return FALSE;
    if (response.status != 200) {
        strcpy(error, response.status == 403 ? "The server wants a different key."
                                             : "The server refused the entry list.");
        HttpFree(&response);
        return FALSE;
    }
    AddDaysFromReply(&gAllDays, response.body);
    HttpFree(&response);
    return TRUE;
}

/* Reads the list of entries, from the server if one is set. If it can't be
   reached, Diary can work with the local drawer for this session. */
static void ScanEntries(void)
{
    gAllDays.count = 0;
    while (gUseServer) {
        char error[200], message[300];

        SetBusy(TRUE);
        if (ScanServerEntries(error, sizeof(error))) {
            SetBusy(FALSE);
            return;
        }
        SetBusy(FALSE);
        gAllDays.count = 0;
        strcpy(message, "Diary couldn't get the entries from the Diary Server:\n");
        strcat(message, error);
        if (Ask(message, "Retry|Work Locally") == 0) {
            gUseServer = FALSE;     /* for this session; the setting stays */
            UpdateTitle();
        }
    }
    ScanLocalEntries();
}

/* ISO Latin-1 for MacRoman 0x80..0xFF; plain look-alikes for typographic
   characters, '?' where Latin-1 has no match. */
static const UBYTE kMacToLatin1[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1,  /* 80 */
    0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,  /* 88 */
    0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3,  /* 90 */
    0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,  /* 98 */
    0x2B, 0xB0, 0xA2, 0xA3, 0xA7, 0xB7, 0xB6, 0xDF,  /* A0 */
    0xAE, 0xA9, 0x3F, 0xB4, 0xA8, 0x3F, 0xC6, 0xD8,  /* A8 */
    0x3F, 0xB1, 0x3F, 0x3F, 0xA5, 0xB5, 0x3F, 0x3F,  /* B0 */
    0x3F, 0x3F, 0x3F, 0xAA, 0xBA, 0x3F, 0xE6, 0xF8,  /* B8 */
    0xBF, 0xA1, 0xAC, 0x3F, 0x3F, 0x3F, 0x3F, 0xAB,  /* C0 */
    0xBB, 0x2E, 0xA0, 0xC0, 0xC3, 0xD5, 0x3F, 0x3F,  /* C8 */
    0x2D, 0x2D, 0x22, 0x22, 0x27, 0x27, 0xF7, 0x3F,  /* D0 */
    0xFF, 0x3F, 0x2F, 0x3F, 0x3C, 0x3E, 0x3F, 0x3F,  /* D8 */
    0x3F, 0xB7, 0x2C, 0x22, 0x3F, 0xC2, 0xCA, 0xC1,  /* E0 */
    0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,  /* E8 */
    0x3F, 0xD2, 0xDA, 0xDB, 0xD9, 0x3F, 0x5E, 0x7E,  /* F0 */
    0xAF, 0x3F, 0x3F, 0x3F, 0xB8, 0x3F, 0x3F, 0x3F   /* F8 */
};

/* Converts text from the Mac version in place: CR and CR LF line ends become
   LF, and MacRoman characters become Latin-1. Diary always ends its files
   with LF, so text without any LF, or with bytes 0x80..0x9F (which only
   MacRoman uses), came from the Mac. Returns the new length. */
static LONG ConvertForeignText(UBYTE *text, LONG size)
{
    BOOL hasLF = FALSE, hasC1 = FALSE, isMacRoman;
    LONG i, j;

    for (i = 0; i < size; i++) {
        if (text[i] == '\n')
            hasLF = TRUE;
        else if (text[i] >= 0x80 && text[i] < 0xA0)
            hasC1 = TRUE;
    }
    isMacRoman = !hasLF || hasC1;

    for (i = j = 0; i < size; i++) {
        UBYTE c = text[i];

        if (c == '\r') {
            c = '\n';
            if (i + 1 < size && text[i + 1] == '\n')
                i++;
        } else if (c >= 0x80 && isMacRoman) {
            c = kMacToLatin1[c - 0x80];
        }
        text[j++] = c;
    }
    return j;
}

/* Reads a whole file into a new NUL-terminated buffer (free with FreeVec),
   converting text from the Mac version. Returns NULL if the file doesn't
   exist or can't be read. */
static char *ReadTextFile(const char *path, LONG *length)
{
    BPTR file = Open(path, MODE_OLDFILE);
    char *buf = NULL;
    LONG size;

    if (file == 0)
        return NULL;
    Seek(file, 0, OFFSET_END);
    size = Seek(file, 0, OFFSET_BEGINNING);
    if (size >= 0)
        buf = AllocVec(size + 1, MEMF_ANY);
    if (buf != NULL && Read(file, buf, size) != size) {
        FreeVec(buf);
        buf = NULL;
    }
    Close(file);
    if (buf == NULL)
        return NULL;

    *length = ConvertForeignText((UBYTE *)buf, size);
    buf[*length] = 0;
    return buf;
}

static BOOL WriteTextFile(const char *path, const char *text, LONG length)
{
    BPTR file = Open(path, MODE_NEWFILE);
    BOOL ok;

    if (file == 0)
        return FALSE;
    ok = Write(file, (APTR)text, length) == length && Write(file, (APTR)"\n", 1) == 1;
    if (!Close(file))
        ok = FALSE;
    return ok;
}

/* Copies the next line of text into out (at most size - 1 characters). */
static char *NextLine(char *text, char *out, LONG size)
{
    LONG n = 0;

    while (*text != 0 && *text != '\n') {
        if (n < size - 1)
            out[n++] = *text;
        text++;
    }
    out[n] = 0;
    return *text == '\n' ? text + 1 : text;
}

/* The settings file has one value per line: font name, font size,
   server address and server key. */
static void LoadSettings(void)
{
    LONG length, size = 0;
    char *text = ReadTextFile(kSettingsFile, &length);
    char *p, name[sizeof(gFontName)], number[8];

    if (text == NULL)
        return;
    p = NextLine(text, name, sizeof(name));
    p = NextLine(p, number, sizeof(number));
    for (length = 0; number[length] >= '0' && number[length] <= '9'; length++)
        size = size * 10 + (number[length] - '0');
    if (name[0] != 0 && size > 0 && size < 256) {
        strcpy(gFontName, name);
        gFontSize = size;
    }
    p = NextLine(p, gServer, sizeof(gServer));
    NextLine(p, gServerKey, sizeof(gServerKey));
    FreeVec(text);
}

static void SaveSettings(void)
{
    char text[sizeof(gFontName) + sizeof(gServer) + sizeof(gServerKey) + 16];

    if (!gHaveFolder)
        return;
    strcpy(text, gFontName);
    strcat(text, "\n");
    AppendNum(text, gFontName[0] ? gFontSize : 0, 1);
    strcat(text, "\n");
    strcat(text, gServer);
    strcat(text, "\n");
    strcat(text, gServerKey);
    WriteTextFile(kSettingsFile, text, strlen(text));
}

/* ---------- The open entry ---------- */

static BOOL EditorChanged(void)
{
    ULONG changed = FALSE;

    get(gEditor, MUIA_TextEditor_HasChanged, &changed);
    return changed != FALSE;
}

static void EditorCommand(const char *command)
{
    DoMethod(gEditor, MUIM_TextEditor_ARexxCmd, (ULONG)command);
}

static void ShowDate(void)
{
    MakeLongDate(gDay, gDateText);
    set(gDateLabel, MUIA_Text_Contents, gDateText);
}

static void ShowMonthOf(LONG day)
{
    LONG y, m, d;

    DayToDate(day, &y, &m, &d);
    gCalYear = y;
    gCalMonth = m;
    UpdateCalendar();
}

static void SyncListSelection(void)
{
    LONG index = FindDay(&gShownDays, gDay);

    nnset(gList, MUIA_List_Active, index >= 0 ? index : MUIV_List_Active_Off);
    if (index >= 0)
        DoMethod(gList, MUIM_List_Jump, index);
}

/* Fetches the day's entry from the server; "" if there's none. Returns a
   buffer to free with FreeVec(), like ReadTextFile(). */
static char *ReadServerEntry(LONG day)
{
    char path[32], error[200];
    HttpResponse response;
    char *text;

    strcpy(path, "/entries/");
    MakeEntryName(day, path + strlen(path));
    strcpy(gEntryVersion, "none");
    SetBusy(TRUE);
    if (!HttpRequest("GET", path, NULL, 0, NULL, &response, error, sizeof(error))) {
        SetBusy(FALSE);
        ShowServerError("Diary couldn't load the entry from the Diary Server:", error);
        return NULL;
    }
    SetBusy(FALSE);
    if (response.status == 200) {
        strcpy(gEntryVersion, response.version);
        text = response.body;       /* keep the buffer; HttpFree() isn't needed */
        return text;
    }
    if (response.status != 404)
        ShowServerError("Diary couldn't load the entry from the Diary Server:",
                        response.status == 403 ? "The server wants a different key." : response.body);
    HttpFree(&response);
    return NULL;
}

static void LoadEntry(void)
{
    char path[64];
    LONG length;
    char *text;

    if (gUseServer) {
        text = ReadServerEntry(gDay);
    } else {
        EntryPath(gDay, path);
        text = ReadTextFile(path, &length);
        if (text != NULL && length > 0 && text[length - 1] == '\n')
            text[length - 1] = 0;   /* SaveEntry adds one line end */
    }

    set(gEditor, MUIA_TextEditor_Contents, text != NULL ? text : "");
    set(gEditor, MUIA_TextEditor_HasChanged, FALSE);
    if (text != NULL)
        FreeVec(text);
    EditorCommand("POSITION EOF");

    ShowDate();
    ShowMonthOf(gDay);
    SyncListSelection();
}

/* Saves (or, if length is 0, deletes) the open entry on the server. If it
   was changed on another computer since it was loaded, asks first. */
static BOOL SaveServerEntry(const char *text, LONG length)
{
    char path[32], error[200];
    const char *base = gEntryVersion;

    strcpy(path, "/entries/");
    MakeEntryName(gDay, path + strlen(path));
    for (;;) {
        HttpResponse response;
        LONG status;

        SetBusy(TRUE);
        if (!HttpRequest("PUT", path, text, length, base, &response, error, sizeof(error))) {
            SetBusy(FALSE);
            ShowServerError("Diary couldn't save the entry on the Diary Server; "
                            "your text is still in the window:", error);
            return FALSE;
        }
        SetBusy(FALSE);
        status = response.status;
        if (status == 200)
            strcpy(gEntryVersion, response.version);
        else if (status != 409)
            ShowServerError("The Diary Server refused to save the entry; "
                            "your text is still in the window:",
                            status == 403 ? "The server wants a different key." : response.body);
        HttpFree(&response);

        if (status == 200)
            return TRUE;
        if (status != 409)
            return FALSE;
        if (Ask("This entry was changed on another computer\n"
                "since you opened it. Save your version over it?",
                "Overwrite|Keep Editing") == 0)
            return FALSE;
        base = NULL;                /* replace it unconditionally */
    }
}

static BOOL SaveEntry(void)
{
    char path[64];
    STRPTR text;
    LONG length;

    if (!EditorChanged())
        return TRUE;
    if (!gUseServer && !gHaveFolder) {
        ShowMessage("There is no \"Diary Entries\" drawer to save into.");
        return FALSE;
    }

    text = (STRPTR)DoMethod(gEditor, MUIM_TextEditor_ExportText);
    length = text != NULL ? (LONG)strlen(text) : 0;
    while (length > 0 && text[length - 1] == '\n')
        length--;

    if (gUseServer) {
        BOOL ok = SaveServerEntry(length > 0 ? (const char *)text : "", length);

        if (text != NULL)
            FreeVec(text);
        if (!ok)
            return FALSE;
        if (length == 0) {
            RemoveDay(&gAllDays, gDay);
            RemoveDay(&gShownDays, gDay);
            RefreshList();
            UpdateCalendar();
        } else if (FindDay(&gAllDays, gDay) < 0) {
            InsertDay(&gAllDays, gDay);
            if (gSearch[0] == 0)
                InsertDay(&gShownDays, gDay);
            RefreshList();
            UpdateCalendar();
        }
        set(gEditor, MUIA_TextEditor_HasChanged, FALSE);
        return TRUE;
    }

    EntryPath(gDay, path);
    if (length == 0) {
        /* An emptied entry is removed rather than kept as an empty file. */
        if (text != NULL)
            FreeVec(text);
        DeleteFile(path);
        RemoveDay(&gAllDays, gDay);
        RemoveDay(&gShownDays, gDay);
        RefreshList();
        UpdateCalendar();
    } else {
        BOOL ok = WriteTextFile(path, text, length);

        FreeVec(text);
        if (!ok) {
            ShowError("Could not save the entry.");
            return FALSE;
        }
        if (FindDay(&gAllDays, gDay) < 0) {
            InsertDay(&gAllDays, gDay);
            if (gSearch[0] == 0)
                InsertDay(&gShownDays, gDay);
            RefreshList();
            UpdateCalendar();
        }
    }
    set(gEditor, MUIA_TextEditor_HasChanged, FALSE);
    return TRUE;
}

static void GoToDay(LONG day)
{
    if (day == gDay) {
        SyncListSelection();
        return;
    }
    if (!SaveEntry()) {
        SyncListSelection();
        return;
    }
    gDay = day;
    LoadEntry();
}

/* ---------- Searching ---------- */

/* Case folding for ASCII and the Latin-1 letters. */
static UBYTE FoldCase(UBYTE c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 32;
    return c;
}

static BOOL TextContains(const char *text, LONG length, const char *pattern)
{
    LONG n = strlen(pattern), i, j;

    for (i = 0; i + n <= length; i++) {
        for (j = 0; j < n; j++)
            if (FoldCase(text[i + j]) != FoldCase(pattern[j]))
                break;
        if (j == n)
            return TRUE;
    }
    return FALSE;
}

static BOOL EntryContains(LONG day)
{
    char path[64];
    LONG length;
    char *text;
    BOOL found;

    /* With a server, the listed days are its search result. */
    if (gUseServer)
        return FindDay(&gShownDays, day) >= 0;

    EntryPath(day, path);
    text = ReadTextFile(path, &length);
    if (text == NULL)
        return FALSE;
    found = TextContains(text, length, gSearch);
    FreeVec(text);
    return found;
}

/* Selects the first match in the open entry, or the next one after the cursor. */
static BOOL SelectMatch(BOOL fromTop)
{
    return DoMethod(gEditor, MUIM_TextEditor_Search, (ULONG)gSearch,
                    fromTop ? MUIF_TextEditor_Search_FromTop
                            : MUIF_TextEditor_Search_Next) != 0;
}

static void OpenDay(LONG day)
{
    GoToDay(day);
    if (gSearch[0] != 0 && day == gDay)
        SelectMatch(TRUE);
}

static void ShowAllEntries(void)
{
    gSearch[0] = 0;
    CopyDays(&gShownDays, &gAllDays);
    RefreshList();
    UpdateCalendar();
}

/* Asks for a line of text, edited in place in buffer, in a small window of
   its own; buttons as for Ask ("_Find|_Cancel"). */
static BOOL AskString(const char *title, const char *body, const char *buttons,
                      char *buffer, LONG maxChars)
{
    char ok[32], cancel[32];
    const char *bar = strchr(buttons, '|');
    ULONG signals = 0, id;
    BOOL result = FALSE, done = FALSE;
    LONG n = bar != NULL ? bar - buttons : (LONG)strlen(buttons);

    if (n >= (LONG)sizeof(ok))
        n = sizeof(ok) - 1;
    memcpy(ok, buttons, n);
    ok[n] = 0;
    strcpy(cancel, bar != NULL && strlen(bar + 1) < sizeof(cancel) ? bar + 1 : "_Cancel");

    SetAttrs(gAskWin, MUIA_Window_Title, (ULONG)title, TAG_DONE);
    set(gAskBody, MUIA_Text_Contents, body);
    set(gAskOK, MUIA_Text_Contents, ok);
    set(gAskCancel, MUIA_Text_Contents, cancel);
    set(gAskString, MUIA_String_Contents, buffer);

    set(gWin, MUIA_Window_Sleep, TRUE);
    set(gAskWin, MUIA_Window_Open, TRUE);
    set(gAskWin, MUIA_Window_ActiveObject, gAskString);
    while (!done) {
        id = DoMethod(gApp, MUIM_Application_NewInput, (ULONG)&signals);
        if (id == ID_ASK_OK || id == ID_ASK_CANCEL || id == (ULONG)MUIV_Application_ReturnID_Quit) {
            result = id == ID_ASK_OK;
            done = TRUE;
        } else if (signals != 0) {
            signals = Wait(signals);
        }
    }
    if (result) {
        STRPTR text = NULL;

        get(gAskString, MUIA_String_Contents, &text);
        if (text != NULL && (LONG)strlen(text) <= maxChars)
            strcpy(buffer, text);
    }
    set(gAskWin, MUIA_Window_Open, FALSE);
    set(gWin, MUIA_Window_Sleep, FALSE);
    return result;
}

static void DoFind(void)
{
    char text[kMaxSearch + 1], previous[kMaxSearch + 1];
    DayList matches = { NULL, 0, 0 };
    LONG i;

    strcpy(text, gSearch);
    if (!AskString("Find", "Find entries containing:", "_Find|_Cancel", text, kMaxSearch))
        return;
    if (text[0] == 0) {
        ShowAllEntries();
        return;
    }
    if (!SaveEntry())
        return;

    SetBusy(TRUE);
    strcpy(previous, gSearch);
    strcpy(gSearch, text);
    if (gUseServer) {
        char path[3 * kMaxSearch + 16], error[200];
        HttpResponse response;

        strcpy(path, "/search?q=");
        NetEncode(text, path + strlen(path), sizeof(path) - strlen(path));
        if (!HttpRequest("GET", path, NULL, 0, NULL, &response, error, sizeof(error))) {
            SetBusy(FALSE);
            strcpy(gSearch, previous);
            ShowServerError("Diary couldn't search on the Diary Server:", error);
            return;
        }
        if (response.status != 200) {
            SetBusy(FALSE);
            strcpy(gSearch, previous);
            ShowServerError("The Diary Server refused the search:",
                            response.status == 403 ? "The server wants a different key." : response.body);
            HttpFree(&response);
            return;
        }
        AddDaysFromReply(&matches, response.body);
        HttpFree(&response);
    } else {
        for (i = 0; i < gAllDays.count; i++)
            if (EntryContains(gAllDays.days[i]))
                InsertDay(&matches, gAllDays.days[i]);
    }
    SetBusy(FALSE);

    if (matches.count == 0) {
        ShowMessage("No entries contain that text.");
        strcpy(gSearch, previous);      /* keep the current filter */
    } else {
        CopyDays(&gShownDays, &matches);
        RefreshList();
        UpdateCalendar();
        if (FindDay(&gShownDays, gDay) < 0)
            GoToDay(gShownDays.days[0]);
        SelectMatch(TRUE);
    }
    FreeDays(&matches);
}

/* Moves to the next match: first within the open entry, then on to the
   following (older) matching entries, wrapping around to the newest. */
static void DoFindAgain(void)
{
    LONG start, step;

    if (gSearch[0] == 0) {
        DoFind();
        return;
    }
    if (SelectMatch(FALSE))
        return;

    start = FindDay(&gShownDays, gDay);
    for (step = 1; step <= gShownDays.count; step++) {
        LONG i = start < 0 ? step - 1 : (start + step) % gShownDays.count;
        LONG day = gShownDays.days[i];

        if (day == gDay) {
            if (SelectMatch(TRUE))
                return;
        } else if (EntryContains(day)) {
            GoToDay(day);
            if (day == gDay)
                SelectMatch(TRUE);
            return;
        }
    }
    DisplayBeep(NULL);
}

/* ---------- Entry list ---------- */

static void RefreshList(void)
{
    LONG i;

    set(gList, MUIA_List_Quiet, TRUE);
    DoMethod(gList, MUIM_List_Clear);
    for (i = 0; i < gShownDays.count; i++) {
        char label[32];

        MakeShortDate(gShownDays.days[i], label);
        DoMethod(gList, MUIM_List_InsertSingle, (ULONG)label, MUIV_List_Insert_Bottom);
    }
    set(gList, MUIA_List_Quiet, FALSE);
    SyncListSelection();
}

static void DoListClick(void)
{
    LONG index = MUIV_List_Active_Off;

    get(gList, MUIA_List_Active, &index);
    if (index >= 0 && index < gShownDays.count)
        OpenDay(gShownDays.days[index]);
}

/* ---------- Calendar ---------- */

/* The calendar is a group of buttons: a title row (arrows and month), a row
   of weekday initials and 42 day cells, numbered row by row from a Sunday.
   Days with an entry are bold, today is underlined and the open day is
   shown pressed. */

static LONG CalendarFirstDay(void)
{
    return DateToDay(gCalYear, gCalMonth, 1);
}

static LONG CalendarMonthLength(void)
{
    LONG next = gCalMonth == 12 ? DateToDay(gCalYear + 1, 1, 1)
                                : DateToDay(gCalYear, gCalMonth + 1, 1);
    return next - CalendarFirstDay();
}

static void UpdateCalendar(void)
{
    LONG first = CalendarFirstDay(), length = CalendarMonthLength(), today = Today();
    int lead = Weekday(first), i;

    if (gMonthLabel == NULL)
        return;
    strcpy(gMonthText, kMonths[gCalMonth - 1]);
    strcat(gMonthText, " ");
    AppendNum(gMonthText, gCalYear, 4);
    set(gMonthLabel, MUIA_Text_Contents, gMonthText);

    for (i = 0; i < 42; i++) {
        LONG n = i - lead + 1, day = first + n - 1;
        BOOL used = n >= 1 && n <= length;
        char label[16] = "";

        if (used) {
            if (FindDay(&gShownDays, day) >= 0)
                strcat(label, "\33b");
            if (day == today)
                strcat(label, "\33u");
            AppendNum(label, n, 1);
        }
        SetAttrs(gDayCells[i],
                 MUIA_NoNotify, TRUE,
                 MUIA_Text_Contents, (ULONG)label,
                 MUIA_Selected, used && day == gDay,
                 MUIA_Disabled, !used,
                 TAG_DONE);
    }
}

static void ChangeCalendarMonth(int delta)
{
    gCalMonth += delta;
    if (gCalMonth < 1) {
        gCalMonth = 12;
        gCalYear--;
    } else if (gCalMonth > 12) {
        gCalMonth = 1;
        gCalYear++;
    }
    UpdateCalendar();
}

static void DoDayClick(int cell)
{
    LONG first = CalendarFirstDay();
    LONG index = cell - Weekday(first);

    if (index >= 0 && index < CalendarMonthLength())
        OpenDay(first + index);
}

/* ---------- Window ---------- */

static Object *MakeButton(const char *text, ULONG id)
{
    Object *button = MUI_NewObject(MUIC_Text,
        MUIA_Frame, MUIV_Frame_Button,
        MUIA_Background, MUII_ButtonBack,
        MUIA_Font, MUIV_Font_Button,
        MUIA_InputMode, MUIV_InputMode_RelVerify,
        MUIA_Text_PreParse, (ULONG)"\33c",
        MUIA_Text_Contents, (ULONG)text,
        MUIA_CycleChain, TRUE,
        TAG_DONE);

    if (button != NULL && id != 0)
        DoMethod(button, MUIM_Notify, MUIA_Pressed, FALSE,
                 (ULONG)gApp, 2, MUIM_Application_ReturnID, id);
    return button;
}

static Object *MakeLabel(const char *text)
{
    return MUI_NewObject(MUIC_Text,
        MUIA_Text_PreParse, (ULONG)"\33c",
        MUIA_Text_Contents, (ULONG)text,
        TAG_DONE);
}

static Object *MakeCalendar(void)
{
    static const char *kInitials[7] = { "S", "M", "T", "W", "T", "F", "S" };
    Object *grid, *prev, *next;
    int i;

    grid = MUI_NewObject(MUIC_Group,
        MUIA_Group_Columns, 7,
        MUIA_Group_SameSize, TRUE,
        MUIA_Group_Spacing, 1,
        TAG_DONE);
    if (grid == NULL)
        return NULL;
    for (i = 0; i < 7; i++)
        DoMethod(grid, OM_ADDMEMBER, (ULONG)MakeLabel(kInitials[i]));
    for (i = 0; i < 42; i++) {
        gDayCells[i] = MUI_NewObject(MUIC_Text,
            MUIA_Frame, MUIV_Frame_Button,
            MUIA_Background, MUII_ButtonBack,
            MUIA_InputMode, MUIV_InputMode_RelVerify,
            MUIA_Text_PreParse, (ULONG)"\33c",
            MUIA_Text_Contents, (ULONG)"",
            MUIA_FixWidthTxt, (ULONG)"00",
            TAG_DONE);
        if (gDayCells[i] == NULL)
            return NULL;
        DoMethod(gDayCells[i], MUIM_Notify, MUIA_Pressed, FALSE,
                 (ULONG)gApp, 2, MUIM_Application_ReturnID, ID_DAY0 + i);
        DoMethod(grid, OM_ADDMEMBER, (ULONG)gDayCells[i]);
    }

    prev = MakeButton("<", ID_CALPREV);
    next = MakeButton(">", ID_CALNEXT);
    gMonthLabel = MUI_NewObject(MUIC_Text,
        MUIA_Text_PreParse, (ULONG)"\33c\33b",
        MUIA_Text_Contents, (ULONG)"September 2026",
        TAG_DONE);
    return MUI_NewObject(MUIC_Group,
        MUIA_Group_Child, MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz, TRUE,
            MUIA_Group_Child, prev,
            MUIA_Group_Child, gMonthLabel,
            MUIA_Group_Child, next,
            TAG_DONE),
        MUIA_Group_Child, grid,
        TAG_DONE);
}

/* The main window and the small window AskString() uses. */
static BOOL CreateApplication(void)
{
    ULONG menuArgs[2] = { (ULONG)kMenus, 0 };
    Object *menus, *slider, *calendar, *prev, *today, *next;

    /* The application object first: the buttons notify it. */
    gApp = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title, (ULONG)"Diary",
        MUIA_Application_Version, (ULONG)kVersion,
        MUIA_Application_Copyright, (ULONG)"No rights reserved (CC0)",
        MUIA_Application_Author, (ULONG)"retrolemmy",
        MUIA_Application_Description, (ULONG)"A simple day-by-day journal",
        MUIA_Application_Base, (ULONG)"DIARY",
        TAG_DONE);
    if (gApp == NULL)
        return FALSE;

    menus = MUI_MakeObjectA(MUIO_MenustripNM, menuArgs);
    calendar = MakeCalendar();
    prev = MakeButton("<", CMD_PREVDAY);
    today = MakeButton("Today", CMD_TODAY);
    next = MakeButton(">", CMD_NEXTDAY);
    slider = MUI_NewObject(MUIC_Scrollbar, TAG_DONE);
    gEditor = MUI_NewObject(MUIC_TextEditor,
        MUIA_TextEditor_Slider, (ULONG)slider,
        MUIA_CycleChain, TRUE,
        TAG_DONE);
    gDateLabel = MUI_NewObject(MUIC_Text,
        MUIA_Text_PreParse, (ULONG)"\33b",
        MUIA_Text_Contents, (ULONG)"Wednesday, 30 September 2026",
        TAG_DONE);
    gList = MUI_NewObject(MUIC_List,
        MUIA_Frame, MUIV_Frame_InputList,
        MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
        MUIA_List_DestructHook, MUIV_List_DestructHook_String,
        TAG_DONE);
    if (calendar == NULL || gEditor == NULL || gList == NULL) {
        /* Without TextEditor.mcc there is no text field. */
        MUI_DisposeObject(gApp);
        gApp = NULL;
        return FALSE;
    }

    gWin = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title, (ULONG)gTitle,
        MUIA_Window_ScreenTitle, (ULONG)"Diary 1.0.1",
        MUIA_Window_ID, MAKE_ID('D', 'I', 'A', 'R'),
        MUIA_Window_Menustrip, (ULONG)menus,
        MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz, TRUE,
            MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                MUIA_HorizWeight, 0,
                MUIA_Group_Child, calendar,
                MUIA_Group_Child, MUI_NewObject(MUIC_Listview,
                    MUIA_Listview_List, (ULONG)gList,
                    MUIA_CycleChain, TRUE,
                    TAG_DONE),
                TAG_DONE),
            MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Child, prev,
                    MUIA_Group_Child, today,
                    MUIA_Group_Child, next,
                    MUIA_Group_Child, gDateLabel,
                    TAG_DONE),
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Spacing, 0,
                    MUIA_Group_Child, gEditor,
                    MUIA_Group_Child, slider,
                    TAG_DONE),
                TAG_DONE),
            TAG_DONE),
        TAG_DONE);

    gAskBody = MUI_NewObject(MUIC_Text, MUIA_Text_Contents, (ULONG)"", TAG_DONE);
    gAskString = MUI_NewObject(MUIC_String,
        MUIA_Frame, MUIV_Frame_String,
        MUIA_String_MaxLen, 128,        /* fits a search and a server address */
        MUIA_CycleChain, TRUE,
        TAG_DONE);
    gAskOK = MakeButton("_OK", ID_ASK_OK);
    gAskCancel = MakeButton("_Cancel", ID_ASK_CANCEL);
    gAskWin = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title, (ULONG)"Diary",
        MUIA_Window_ID, MAKE_ID('D', 'I', 'A', 'A'),
        MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,
            MUIA_Group_Child, gAskBody,
            MUIA_Group_Child, gAskString,
            MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                MUIA_Group_Horiz, TRUE,
                MUIA_Group_SameSize, TRUE,
                MUIA_Group_Child, gAskOK,
                MUIA_Group_Child, MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                MUIA_Group_Child, gAskCancel,
                TAG_DONE),
            TAG_DONE),
        TAG_DONE);
    if (gWin == NULL || gAskWin == NULL) {
        MUI_DisposeObject(gApp);
        gApp = NULL;
        return FALSE;
    }
    DoMethod(gApp, OM_ADDMEMBER, (ULONG)gWin);
    DoMethod(gApp, OM_ADDMEMBER, (ULONG)gAskWin);

    DoMethod(gWin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (ULONG)gApp, 2, MUIM_Application_ReturnID, CMD_QUIT);
    DoMethod(gAskWin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (ULONG)gApp, 2, MUIM_Application_ReturnID, ID_ASK_CANCEL);
    DoMethod(gAskString, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (ULONG)gApp, 2, MUIM_Application_ReturnID, ID_ASK_OK);
    DoMethod(gList, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             (ULONG)gApp, 2, MUIM_Application_ReturnID, ID_LIST);
    set(gWin, MUIA_Window_DefaultObject, gEditor);
    return TRUE;
}

/* Settings > Diary Server...: where the entries are kept. */
static void ChooseServer(void)
{
    char server[sizeof(gServer)], key[sizeof(gServerKey)];

    strcpy(server, gServer);
    if (!AskString("Diary Server",
                   "Keep the entries on a Diary Server at this address\n"
                   "(host or host:port; leave it empty for the\n"
                   "\"Diary Entries\" drawer):",
                   "_OK|_Cancel", server, sizeof(server) - 1))
        return;
    strcpy(key, gServerKey);
    if (server[0] != 0
            && !AskString("Diary Server", "The server's key (leave it empty if it has none):",
                          "_OK|_Cancel", key, sizeof(key) - 1))
        return;
    if (!SaveEntry())
        return;

    strcpy(gServer, server);
    strcpy(gServerKey, server[0] != 0 ? key : "");
    gUseServer = gServer[0] != 0;
    NetSetServer(gServer, gServerKey);
    SaveSettings();
    UpdateTitle();

    ScanEntries();
    ShowAllEntries();
    LoadEntry();
}

/* ---------- Commands ---------- */

static BOOL DoQuit(void)
{
    if (SaveEntry())
        return TRUE;
    return Ask("The entry could not be saved.\nQuit anyway?", "Quit|Cancel") == 1;
}

/* Returns TRUE when Diary should quit. */
static BOOL DoCommand(ULONG command)
{
    if (command >= ID_DAY0 && command < ID_DAY0 + 42) {
        DoDayClick(command - ID_DAY0);
        UpdateCalendar();       /* keeps the open day pressed */
        return FALSE;
    }
    switch (command) {
    case CMD_ABOUT:     ShowAbout(); break;
    case CMD_SAVE:      SaveEntry(); break;
    case (ULONG)MUIV_Application_ReturnID_Quit:
    case CMD_QUIT:      return DoQuit();
    case CMD_UNDO:      EditorCommand("UNDO"); break;
    case CMD_REDO:      EditorCommand("REDO"); break;
    case CMD_CUT:       EditorCommand("CUT"); break;
    case CMD_COPY:      EditorCommand("COPY"); break;
    case CMD_PASTE:     EditorCommand("PASTE"); break;
    case CMD_SELECTALL:
        EditorCommand("POSITION SOF");
        EditorCommand("MARK ON");
        EditorCommand("POSITION EOF");
        break;
    case CMD_FIND:      DoFind(); break;
    case CMD_FINDAGAIN: DoFindAgain(); break;
    case CMD_SHOWALL:   ShowAllEntries(); break;
    case CMD_PREVDAY:   OpenDay(gDay - 1); break;
    case CMD_NEXTDAY:   OpenDay(gDay + 1); break;
    case CMD_TODAY:     OpenDay(Today()); break;
    case CMD_SERVER:    ChooseServer(); break;
    case CMD_MUIPREFS:  DoMethod(gApp, MUIM_Application_OpenConfigWindow, 0); break;
    case ID_LIST:       DoListClick(); break;
    case ID_CALPREV:    ChangeCalendarMonth(-1); break;
    case ID_CALNEXT:    ChangeCalendarMonth(1); break;
    }
    return FALSE;
}

/* ---------- Start-up ---------- */

int main(void)
{
    ULONG signals = 0;
    int rc = 20;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    if (IntuitionBase == NULL)
        return 20;
    MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, 19);
    if (MUIMasterBase == NULL) {
        struct EasyStruct es = { sizeof(es), 0, (STRPTR)"Diary",
                                 (STRPTR)"Diary needs MUI 3.8 or later.", (STRPTR)"OK" };
        EasyRequestArgs(NULL, &es, NULL, NULL);
        goto done;
    }
    if (!CreateApplication()) {
        struct EasyStruct es = { sizeof(es), 0, (STRPTR)"Diary",
                                 (STRPTR)"Could not open the Diary window.\n"
                                 "Diary needs MUI 3.8 or later and TextEditor.mcc.",
                                 (STRPTR)"OK" };
        EasyRequestArgs(NULL, &es, NULL, NULL);
        goto done;
    }

    FindEntriesFolder();
    LoadSettings();
    gUseServer = gServer[0] != 0;
    NetSetServer(gServer, gServerKey);
    UpdateTitle();
    ScanEntries();
    CopyDays(&gShownDays, &gAllDays);
    gDay = Today();

    set(gWin, MUIA_Window_Open, TRUE);
    {
        ULONG open = FALSE;

        get(gWin, MUIA_Window_Open, &open);
        if (!open) {
            ShowMessage("Could not open the Diary window.");
            goto done;
        }
    }
    RefreshList();
    LoadEntry();
    set(gWin, MUIA_Window_ActiveObject, gEditor);
    rc = 0;

    for (;;) {
        ULONG id = DoMethod(gApp, MUIM_Application_NewInput, (ULONG)&signals);

        if (id != 0 && DoCommand(id))
            break;
        if (signals != 0) {
            signals = Wait(signals | SIGBREAKF_CTRL_C);
            if (signals & SIGBREAKF_CTRL_C) {
                SaveEntry();
                break;
            }
        }
    }

done:
    if (gApp != NULL)
        MUI_DisposeObject(gApp);
    NetCleanup();
    FreeDays(&gAllDays);
    FreeDays(&gShownDays);
    if (MUIMasterBase != NULL)
        CloseLibrary(MUIMasterBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
