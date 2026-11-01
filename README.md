# Diary for MUI

A simple one-page-per-day journal for MorphOS, and for AmigaOS with MUI.
It's the MUI sibling of [Diary for AmigaOS 3.2](https://github.com/retrolemmy/Diary-Amiga):
the same program with a MUI window instead of ReAction. Like the
[classic Mac OS](https://github.com/retrolemmy/Diary),
[Mac OS X](https://github.com/retrolemmy/Diary-MacOSX) and
[Windows 98](https://github.com/retrolemmy/Diary-Win98) versions, it can keep
its entries on a [Diary Server](https://github.com/retrolemmy/Diary-Server),
so all your computers share one diary.

- One window: a month calendar and a list of all entries (newest first) on
  the left, the selected day on the right with `<`, `Today` and `>` buttons
- In the calendar, days with an entry are **bold**, today is underlined and
  the open day is shown pressed; the arrows switch months. Click a day (or a
  list entry) to open it
- **Find** (Amiga-F) searches every entry (ignoring case) and narrows the list
  to the matches; **Find Again** (Amiga-G) jumps to the next match, across
  entries; **Show All Entries** clears the search
- **Go** menu: Previous Day (Amiga-[), Next Day (Amiga-]), Today (Amiga-T)
- The current entry is saved automatically when you switch days or quit;
  Amiga-S saves at any time
- Entries are plain text files named `YYYY-MM-DD` (ISO Latin-1) in a
  **Diary Entries** drawer next to the program, in the same format as Diary
  for AmigaOS 3.2, so both can use the same drawer. Entries from the Mac
  version can be copied in and read as they are. Clearing an entry
  completely deletes its file
- **Settings → Diary Server...** keeps the entries on a Diary Server instead
  (host or host:port, and the server's key if it has one). Search then runs
  on the server, and if an entry was changed on another computer since you
  opened it, Diary asks before saving over it. It needs a TCP/IP stack
  (MorphOS' own, or Roadshow, AmiTCP, ... on AmigaOS); without a server set,
  Diary never touches the network
- Fonts and looks follow your MUI settings (**Settings → MUI...**); the text
  field's font is set in TextEditor's own MUI settings

Diary is a 68k program, so one file runs everywhere: on MorphOS (which runs
68k MUI programs natively, through its 68k emulation), and on AmigaOS 3.x
and 4 with MUI 3.8 or later and the TextEditor.mcc class (MorphOS has it;
on AmigaOS it's on Aminet as
[MCC_TextEditor](https://aminet.net/package/dev/mui/MCC_TextEditor-15.56)
if your MUI doesn't include it). Without them, Diary says what's missing.
Tested on a Mac mini G4 with MorphOS and on an A4000 with AmigaOS 3.2 and
MUI, both with a Diary Server.

## Diary Server

To share the entries between your computers, run
[Diary Server](https://github.com/retrolemmy/Diary-Server) on a machine in
your network, such as a RetroNAS or any small Linux box with Python 3.9 or
later: download `diary_server.py` from its
[latest release](https://github.com/retrolemmy/Diary-Server/releases/latest)
and start it as its README describes. Diary works with Diary Server 1.0 or
later. The same server also gives modern browsers a Diary web page.

## Building

You need [bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc) with its
MUI and TextEditor SDKs:

```sh
make sdk=mui
make sdk=mcc_texteditor
```

Then, in this directory:

```sh
make
```

`dist/` then holds `Diary` and its icon `Diary.info`. Set `TOOLCHAIN` if the
compiler isn't in `~/Projects/amiga-toolchain`.

The non-window parts (dates, entry files, character conversion, the Diary
Server client in `net.c`) are the same as in Diary for AmigaOS 3.2.

## License

No rights reserved: Diary is dedicated to the public domain under
[CC0 1.0](LICENSE). Use it for anything, no attribution needed.
